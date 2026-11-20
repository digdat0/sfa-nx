// Clang reads TLS through tpidr_el0; libnx supplies its base through
// __aarch64_read_tp(). Synchronize them before constructors and worker entry.
// Adapted from KartPad-NX (via Melee-NX).
//
// Why: libnx keeps the thread pointer in its own per-thread structure and returns it from
// __aarch64_read_tp(), while code compiled by Clang (the game, see switch/cmake/clang-game.sh) reads
// thread-local variables straight from the tpidr_el0 register. Each thread therefore copies libnx's value
// into tpidr_el0 before running anything else: the main thread in a constructor, every other thread in
// the pthread_create trampoline below.
//
// The file also wraps (-Wl,--wrap=..., switch/CMakeLists.txt) four more libc functions:
//   pthread_create  gives every thread an 8 MiB stack and the TLS trampoline
//   pthread_detach  makes detach work (libnx returns ENOSYS) through a reaper thread
//   pthread_join    frees the stack this file allocated
//   exit/_exit/abort  flush caches and logs, then end the process without libnx teardown

#include <pthread.h>
#include <stdlib.h>
#include <malloc.h>
#include <errno.h>
#include <stddef.h>

// Tint's recursive shader analysis can overflow libnx's 128 KiB default stack.
#define KARTPAD_MIN_THREAD_STACK ((size_t)8 * 1024 * 1024)

extern void* __aarch64_read_tp(void);

// Copies libnx's thread pointer into tpidr_el0 for the calling thread.
void kartpad_sync_clang_tls(void) {
    __asm__ volatile("msr tpidr_el0, %0" : : "r"(__aarch64_read_tp()) : "memory");
}

// Initialize main-thread TLS before default-priority constructors.
__attribute__((constructor(101))) static void kartpad_sync_clang_tls_main(void) {
    kartpad_sync_clang_tls();
}

// Worker entry is intercepted through the pthread_create linker wrapper.
extern int __real_pthread_create(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);

/* Boot tracing commits each failure independently of stdio. */
extern void melee_boot_trace(const char* fmt, ...);

/* Thread and stack totals for allocation-failure diagnostics. */
static int kartpad_threads_created;
static size_t kartpad_stack_bytes;

/* libnx pthread_detach returns ENOSYS. Reap finished detached threads with
   pthread_join to release their handles and stacks. Queue by completion so
   a long-lived worker cannot block short-lived ones. Requires linker wrappers
   for pthread_detach and pthread_join. */
extern int __real_pthread_join(pthread_t, void**);

typedef struct KpThreadRec {
    pthread_t id;
    void* stack_buf;               /* the memalign'd stack below; NULL if the caller supplied one */
    int finished;                  /* set by the thread itself, just before it returns */
    int detached;                  /* set by __wrap_pthread_detach */
    struct KpThreadRec* next;      /* registry, keyed by id for detach/join lookup */
    struct KpThreadRec* reap_next; /* reap queue, finished-and-detached only */
} KpThreadRec;

static pthread_mutex_t kp_thread_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t kp_reap_cond = PTHREAD_COND_INITIALIZER;
static KpThreadRec* kp_registry;
static KpThreadRec* kp_reap_head;
static KpThreadRec* kp_reap_tail;
static int kp_reaper_running;

/* All three helpers below require kp_thread_mutex. */
static KpThreadRec* kp_find_locked(pthread_t id) {
    for (KpThreadRec* r = kp_registry; r != NULL; r = r->next) {
        if (pthread_equal(r->id, id)) {
            return r;
        }
    }
    return NULL;
}

static void kp_unlink_locked(KpThreadRec* rec) {
    KpThreadRec** pp = &kp_registry;
    while (*pp != NULL) {
        if (*pp == rec) {
            *pp = rec->next;
            return;
        }
        pp = &(*pp)->next;
    }
}

static void kp_push_reap_locked(KpThreadRec* rec) {
    rec->reap_next = NULL;
    if (kp_reap_tail != NULL) {
        kp_reap_tail->reap_next = rec;
    } else {
        kp_reap_head = rec;
    }
    kp_reap_tail = rec;
    pthread_cond_signal(&kp_reap_cond);
}

// Reaper thread: joins detached threads as they finish (in finishing order) and frees their stacks.
static void* kp_reaper_main(void* unused) {
    (void)unused;
    kartpad_sync_clang_tls();
    for (;;) {
        pthread_mutex_lock(&kp_thread_mutex);
        while (kp_reap_head == NULL) {
            pthread_cond_wait(&kp_reap_cond, &kp_thread_mutex);
        }
        KpThreadRec* rec = kp_reap_head;
        kp_reap_head = rec->reap_next;
        if (kp_reap_head == NULL) {
            kp_reap_tail = NULL;
        }
        kp_unlink_locked(rec);
        pthread_mutex_unlock(&kp_thread_mutex);

        /* Joining must finish before its stack can be freed. */
        __real_pthread_join(rec->id, NULL);
        free(rec->stack_buf);
        free(rec);
    }
    return NULL;
}

// Marks a thread detached; if it already finished it is queued for the reaper right away, otherwise its
// trampoline queues it when it returns. The reaper starts on the first detach.
int __wrap_pthread_detach(pthread_t thread) {
    int reaper_failed = 0;
    pthread_mutex_lock(&kp_thread_mutex);
    if (!kp_reaper_running) {
        /* The reaper is persistent and does not need the worker stack floor. */
        pthread_t reaper;
        if (__real_pthread_create(&reaper, NULL, kp_reaper_main, NULL) == 0) {
            kp_reaper_running = 1;
        } else {
            reaper_failed = 1;
        }
    }
    KpThreadRec* rec = kp_find_locked(thread);
    if (rec != NULL) {
        rec->detached = 1;
        if (rec->finished) {
            kp_push_reap_locked(rec);
        }
    }
    pthread_mutex_unlock(&kp_thread_mutex);

    if (reaper_failed) {
        /* Preserve detach success; report unreclaimed handles if the reaper fails. */
        melee_boot_trace("!! pthread_detach: reaper thread would not start; handles will leak");
    }
    return 0;
}

// Normal join, then frees the stack and record this file allocated for the thread.
int __wrap_pthread_join(pthread_t thread, void** retval) {
    int rc = __real_pthread_join(thread, retval);
    if (rc != 0) {
        return rc;
    }
    pthread_mutex_lock(&kp_thread_mutex);
    KpThreadRec* rec = kp_find_locked(thread);
    /* A detached record belongs to the reaper; joining one is a caller bug, and
     unlinking it here would leave the reap queue pointing at freed memory. */
    if (rec != NULL && rec->detached) {
        rec = NULL;
    }
    if (rec != NULL) {
        kp_unlink_locked(rec);
    }
    pthread_mutex_unlock(&kp_thread_mutex);
    if (rec != NULL) {
        free(rec->stack_buf);
        free(rec);
    }
    return rc;
}

typedef struct {
    void* (*start)(void*);
    void* arg;
    KpThreadRec* rec;
} kartpad_tls_trampoline_ctx;

// Entry of every thread created through the wrapper: fixes TLS, runs the real start function, then
// marks the record finished (queueing it for the reaper if the thread was detached).
static void* kartpad_tls_thread_trampoline(void* raw) {
    kartpad_sync_clang_tls(); // first: even allocator internals may use TLS
    kartpad_tls_trampoline_ctx ctx = *(kartpad_tls_trampoline_ctx*)raw;
    free(raw);
    void* result = ctx.start(ctx.arg);
    /* This trampoline still uses the stack. Only a joiner may free it. */
    if (ctx.rec != NULL) {
        pthread_mutex_lock(&kp_thread_mutex);
        ctx.rec->finished = 1;
        if (ctx.rec->detached) {
            kp_push_reap_locked(ctx.rec);
        }
        pthread_mutex_unlock(&kp_thread_mutex);
    }
    return result;
}

// Skip static destruction and libnx service teardown: workers may still use
// fsdev, and partial teardown can invalidate their file handles. Flush caches
// and logs explicitly before svcExitProcess() stops every thread. All three
// exit paths (exit, abort, _exit) must be wrapped.
#include <stdio.h>
#include <switch.h>
#include <unistd.h>

/* Flush database handles and rollback journals before bypassing teardown. */
void melee_nx_sqlite_flush_all(void);

/* Commit pending cache batches before flushing the underlying file handles. */
void aurora_flush_caches(void);
void melee_nx_log_shutdown(void);

__attribute__((noreturn)) static void kartpad_fast_exit(void) {
    aurora_flush_caches();
    melee_nx_sqlite_flush_all();
    melee_nx_log_shutdown();
    fflush(NULL);
    /* fd 1 and fd 2 are the run's logs/stdout-NNN.log (main_sfa.cpp's
       redirect_stdio); commit them before the FS session goes away. */
    fsync(STDOUT_FILENO);
    fsync(STDERR_FILENO);
    svcExitProcess();
    __builtin_unreachable();
}

__attribute__((noreturn)) void __wrap_exit(int code) {
    (void)code; /* Horizon has no exit status to report. */
    kartpad_fast_exit();
}

__attribute__((noreturn)) void __wrap__exit(int code) {
    (void)code;
    kartpad_fast_exit();
}

__attribute__((noreturn)) void __wrap_abort(void) { kartpad_fast_exit(); }

// pthread_create with an 8 MiB stack floor and the TLS trampoline. A caller's attributes are used as
// they are when their stack is already big enough.
int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*start)(void*),
                          void* arg) {
    kartpad_tls_trampoline_ctx* ctx = (kartpad_tls_trampoline_ctx*)malloc(sizeof(*ctx));
    if (!ctx) {
        // pthread_create returns an error number, rather than setting errno. Never
        // launch an unwrapped thread: callers can handle resource exhaustion, but
        // cannot recover from a worker accessing the wrong TLS base.
        return EAGAIN;
    }
    KpThreadRec* rec = (KpThreadRec*)calloc(1, sizeof(*rec));
    if (!rec) {
        free(ctx);
        return EAGAIN;
    }
    ctx->start = start;
    ctx->arg = arg;
    ctx->rec = rec;

    // An explicit stack buffer is required: setting only the size leaves libnx
    // using its default stack. A joiner frees the buffer after the thread exits.
    const size_t want_size = KARTPAD_MIN_THREAD_STACK;
    int honour_attr = 0;
    if (attr != NULL) {
        size_t current = 0;
        pthread_attr_getstacksize(attr, &current);
        // Preserve caller-supplied attributes when the stack meets the floor.
        honour_attr = (current >= KARTPAD_MIN_THREAD_STACK);
    }

    pthread_attr_t local_attr;
    const pthread_attr_t* use_attr = attr;
    if (!honour_attr) {
        // Horizon requires a page-aligned stack.
        void* stack_buf = memalign(4096, want_size);
        if (!stack_buf) {
            melee_boot_trace(
                "!! pthread_create: memalign(%zu KiB) failed after %d threads (%zu MiB of stacks)",
                want_size / 1024, kartpad_threads_created, kartpad_stack_bytes / (1024 * 1024));
            free(rec);
            free(ctx);
            return EAGAIN;
        }
        rec->stack_buf = stack_buf;
        if (pthread_attr_init(&local_attr) != 0) {
            free(stack_buf);
            free(rec);
            free(ctx);
            return EAGAIN;
        }
        // Supply both the stack address and its size.
        pthread_attr_setstack(&local_attr, stack_buf, want_size);
        use_attr = &local_attr;
    }

    /* Publish the record under the same lock the new thread takes on its way
     out, so a thread that finishes instantly cannot race ahead of its own
     registration and leave detach() with nothing to find. */
    pthread_mutex_lock(&kp_thread_mutex);
    int rc = __real_pthread_create(thread, use_attr, kartpad_tls_thread_trampoline, ctx);
    if (rc == 0) {
        rec->id = *thread;
        rec->next = kp_registry;
        kp_registry = rec;
        kartpad_threads_created++;
        kartpad_stack_bytes += rec->stack_buf ? want_size : 0;
    }
    pthread_mutex_unlock(&kp_thread_mutex);

    if (!honour_attr) {
        pthread_attr_destroy(&local_attr);
    }
    if (rc != 0) {
        melee_boot_trace(
            "!! pthread_create: rc=%d with a %zu KiB stack after %d threads (%zu MiB of stacks)",
            rc, want_size / 1024, kartpad_threads_created, kartpad_stack_bytes / (1024 * 1024));
        free(rec->stack_buf);
        free(rec);
        free(ctx);
    }
    return rc;
}
