// Double-buffered asynchronous log writer (interface and purpose in log_switch.h).
//
// Producers append to buffers[pending] under `mutex`. The writer thread swaps buffers (pending ^= 1), writes
// the full one outside the lock and fsyncs at most once a second, or right away when someone asked for a
// flush. Flush requests are counted (requested/completed) so a caller can wait for "everything I wrote so
// far is on the card" without stopping other producers.
#include "log_switch.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#if defined(__SWITCH__)
#include <switch.h>
#endif

namespace {
// Size of each of the two buffers. When a burst fills one before the writer empties it, whole records are
// dropped and counted (reported as dropped_records in the next LOGIO line).
constexpr size_t Capacity = 128 * 1024;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t wake = PTHREAD_COND_INITIALIZER, flushed = PTHREAD_COND_INITIALIZER;
pthread_t worker;
char buffers[2][Capacity];
size_t used = 0;
unsigned pending = 0;
bool running = false, stopping = false;
// Flush tickets: melee_nx_log_flush takes ticket ++requested and waits until the writer has set completed
// to at least that value.
uint64_t requested = 0, completed = 0, dropped = 0;
pthread_once_t clockOnce = PTHREAD_ONCE_INIT;
uint64_t clockStart = 0;
int fileFd = STDOUT_FILENO; // where the writer puts the bytes (the log file once stdio is captured)

// Monotonic clock in nanoseconds.
uint64_t now_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}
void init_clock() { clockStart = now_ns(); }
// write(2) until all bytes are out; short writes are retried, EINTR is ignored.
bool write_all(const char* data, size_t length) {
    while (length) {
        const ssize_t n = write(fileFd, data, length);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        data += n;
        length -= size_t(n);
    }
    return true;
}
// Writer thread. Wakes when 16 KiB are queued, a flush is requested, or after 100 ms; writes the filled
// buffer; every 2 s (Extra logging) or 60 s (default) appends a LOGIO line with its own write/fsync cost; fsyncs at most once a second
// unless a flush asked for it.
void* writer(void*) {
#if defined(__SWITCH__)
    // I/O work should not preempt game/audio/FIFO work. No compiler-core pin.
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x32);
#endif
    uint64_t lastSync = now_ns(), lastReport = lastSync;
    uint64_t writeNs = 0, syncNs = 0, bytes = 0, errors = 0;
    bool dirty = false;
    for (;;) {
        pthread_mutex_lock(&mutex);
        if (used < 16384 && requested == completed && !stopping) {
            timespec deadline{};
            clock_gettime(CLOCK_REALTIME, &deadline);
            deadline.tv_nsec += 100000000;
            if (deadline.tv_nsec >= 1000000000) {
                ++deadline.tv_sec;
                deadline.tv_nsec -= 1000000000;
            }
            pthread_cond_timedwait(&wake, &mutex, &deadline);
        }
        // Swap buffers under the lock: producers continue in the other buffer while this one is written.
        const size_t length = used;
        const unsigned ready = pending;
        pending ^= 1;
        used = 0;
        const uint64_t request = requested, lost = dropped;
        dropped = 0;
        const bool stop = stopping;
        pthread_mutex_unlock(&mutex);

        if (length) {
            const auto start = now_ns();
            if (!write_all(buffers[ready], length))
                ++errors;
            writeNs += now_ns() - start;
            bytes += length;
            dirty = true;
        }
        auto now = now_ns();
        // The LOGIO cadence follows the Extra logging setting; records lost to a full buffer are always reported.
        const uint64_t reportEvery = sfa_log_extra() ? 2000000000ull : 60000000000ull;
        if (lost || now - lastReport >= reportEvery || stop) {
            char line[256];
            const int n = std::snprintf(
                line, sizeof(line),
                "[%9.3f] LOGIO bytes %llu dropped_records %llu errors %llu write_ms %.3f flush_ms %.3f\n",
                melee_nx_log_now_ms(), (unsigned long long)bytes, (unsigned long long)lost,
                (unsigned long long)errors, writeNs / 1e6, syncNs / 1e6);
            if (n > 0)
                write_all(line, size_t(n));
            bytes = writeNs = syncNs = errors = 0;
            dirty = true;
            lastReport = now;
        }
        if (request != completed || stop || (dirty && now - lastSync >= 1000000000)) {
            const auto start = now_ns();
            if (fsync(fileFd) != 0)
                ++errors;
            syncNs += now_ns() - start;
            lastSync = now_ns();
            dirty = false;
        }
        pthread_mutex_lock(&mutex);
        completed = request;
        pthread_cond_broadcast(&flushed);
        pthread_mutex_unlock(&mutex);
        if (stop)
            break;
    }
    return nullptr;
}
} // namespace

// Extra logging flag (log_switch.h). Off until main_sfa.cpp reads switch_menu.ini.
extern "C" {
int sfa_log_extra_on = 0;
}
extern "C" void sfa_log_set_extra(int on) {
    on = on != 0;
    if (__atomic_exchange_n(&sfa_log_extra_on, on, __ATOMIC_RELAXED) != on) {
        char line[64];
        const int n = std::snprintf(line, sizeof(line), "[sfa-nx] extra logging %s\n", on ? "on" : "off");
        if (n > 0)
            melee_nx_log_write(line, size_t(n));
    }
}
extern "C" double melee_nx_log_now_ms(void) {
    pthread_once(&clockOnce, init_clock);
    return (now_ns() - clockStart) / 1e6;
}
// Starts the writer thread. If thread creation fails, writes stay synchronous (slower but nothing is lost).
extern "C" void melee_nx_log_init(void) {
    pthread_mutex_lock(&mutex);
    if (!running) {
        stopping = false;
        running = pthread_create(&worker, nullptr, writer, nullptr) == 0;
        if (!running)
            write_all("[LOGIO] writer unavailable; synchronous fallback\n",
                      sizeof("[LOGIO] writer unavailable; synchronous fallback\n") - 1);
    }
    pthread_mutex_unlock(&mutex);
}
#if defined(__SWITCH__)
#include <fcntl.h>
#include <sys/iosupport.h>
#include <sys/stat.h>
namespace {
// "sfalog:" device: fd 1/2 point here, so every printf/fprintf (any thread, any reent) is a memcpy into
// the writer's buffer instead of an SD write on the caller's thread (~1 ms/frame on the game thread).
// newlib routes file descriptors through devoptab entries; this one only needs write and fstat (fstat
// reports a character device so stdio does not try to seek it).
int dev_open(struct _reent*, void*, const char*, int, int) { return 0; }
int dev_close(struct _reent*, void*) { return 0; }
ssize_t dev_write(struct _reent*, void*, const char* ptr, size_t len) {
    melee_nx_log_write(ptr, len);
    return static_cast<ssize_t>(len);
}
int dev_fstat(struct _reent*, void*, struct stat* st) {
    std::memset(st, 0, sizeof(*st));
    st->st_mode = S_IFCHR;
    return 0;
}
devoptab_t g_logDevice;
} // namespace
// Registers the sfalog: device, opens it and dup2s it over stdout and stderr. The writer thread then
// writes to logFd (logs/log.txt). Returns 0 if any step fails, and main falls back to
// pointing stdout straight at the file.
extern "C" int melee_nx_log_capture_stdio(int logFd) {
    std::memset(&g_logDevice, 0, sizeof(g_logDevice));
    g_logDevice.name = "sfalog";
    g_logDevice.structSize = sizeof(int);
    g_logDevice.open_r = dev_open;
    g_logDevice.close_r = dev_close;
    g_logDevice.write_r = dev_write;
    g_logDevice.fstat_r = dev_fstat;
    std::fflush(stdout);
    std::fflush(stderr);
    pthread_mutex_lock(&mutex);
    fileFd = logFd;
    pthread_mutex_unlock(&mutex);
    melee_nx_log_init();
    if (AddDevice(&g_logDevice) < 0) {
        return 0;
    }
    const int fd = open("sfalog:/stdout", O_WRONLY);
    if (fd < 0) {
        return 0;
    }
    const bool ok = dup2(fd, STDOUT_FILENO) >= 0 && dup2(fd, STDERR_FILENO) >= 0;
    close(fd);
    return ok ? 1 : 0;
}
#endif
// Queues one record. Before the writer starts it writes straight to the file.
extern "C" void melee_nx_log_write(const char* data, size_t size) {
    if (!data || !size)
        return;
    pthread_mutex_lock(&mutex);
    if (!running) {
        write_all(data, size);
    } else if (stopping || size > Capacity - used) {
        // Drop whole records, never block producers on a slow SD card. The
        // writer reports losses explicitly; no unbounded allocation or queue.
        ++dropped;
    } else {
        std::memcpy(buffers[pending] + used, data, size);
        used += size;
        if (used >= 16384)
            pthread_cond_signal(&wake);
    }
    pthread_mutex_unlock(&mutex);
}
// Waits until everything queued so far has been written and fsynced.
extern "C" void melee_nx_log_flush(void) {
    pthread_mutex_lock(&mutex);
    if (running && !stopping) {
        const uint64_t target = ++requested;
        pthread_cond_signal(&wake);
        while (completed < target)
            pthread_cond_wait(&flushed, &mutex);
    } else if (!running) {
        fsync(fileFd);
    }
    pthread_mutex_unlock(&mutex);
}
extern "C" void melee_nx_log_write_critical(const char* data, size_t size) {
    if (!data || !size)
        return;
    // Critical records may block for space. Routine telemetry never does.
    // Split only if a caller supplies more than the entire buffer capacity.
    while (size) {
        const size_t chunk = size < Capacity ? size : Capacity;
        pthread_mutex_lock(&mutex);
        while (running && !stopping && chunk > Capacity - used) {
            const auto target = ++requested;
            pthread_cond_signal(&wake);
            while (completed < target)
                pthread_cond_wait(&flushed, &mutex);
        }
        if (!running)
            write_all(data, chunk);
        else if (!stopping) {
            std::memcpy(buffers[pending] + used, data, chunk);
            used += chunk;
        }
        pthread_mutex_unlock(&mutex);
        data += chunk;
        size -= chunk;
    }
    melee_nx_log_flush();
}
// Flushes, stops the writer thread and returns to synchronous writes.
extern "C" void melee_nx_log_shutdown(void) {
    melee_nx_log_flush();
    pthread_mutex_lock(&mutex);
    const bool join = running;
    stopping = true;
    pthread_cond_signal(&wake);
    pthread_mutex_unlock(&mutex);
    if (join)
        pthread_join(worker, nullptr);
    pthread_mutex_lock(&mutex);
    running = false;
    pthread_mutex_unlock(&mutex);
}
