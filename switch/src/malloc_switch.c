// SFA-NX's heap allocator: dlmalloc 2.8.6 (switch/src/dlmalloc, public domain) in place of newlib's.
//
// Why: newlib's malloc is dlmalloc 2.6, whose large bins are sorted lists that malloc walks one chunk at a
// time. Over a session the frame-by-frame churn of Aurora's recorder and Dawn fills the 512-2559 byte bins
// with thousands of free chunks. Hardware logs 2026-11-20 (32 min of play): free chunks 4.7k -> 96k,
// the GX/FIFO thread's _malloc_r 5 -> 14 ms per frame, and the render worker waiting 5 -> 15 ms per frame
// on newlib's one heap lock while the other thread walked. FPS fell from 54 to 33-45 as play went on.
// dlmalloc 2.8 keeps large free chunks in bitwise tries (log time) and small ones in exact-size bins,
// so a request costs the same at minute 1 and minute 60.
// (This replaces the earlier --wrap=malloc that only skipped devkitA64's forced 32-byte memalign; blocks
// stay 16-byte aligned, as they were with that wrap.)
//
// How: this file defines every newlib malloc entry point (the plain names and the _r ones newlib calls
// internally, e.g. strdup -> _malloc_r), so the linker never pulls newlib's malloc objects
// (libc_a-mallocr.o and friends). build.sh fails the build if newlib's __malloc_av_ still links in.
// Memory still comes from libnx's heap through sbrk, under one libnx Mutex.
//
// Also counts calls and samples callers for the periodic "[dk] heap" line (switch/deko/device.cpp).
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/reent.h>
#include <unistd.h>
#include <switch.h>
#include "log_switch.h"  // sfa_log_extra: the leak finder runs only with Extra logging on

// ---- dlmalloc configuration ------------------------------------------------------------------------
#define USE_DL_PREFIX 1         // dlmalloc's functions are dlmalloc/dlfree/...; the exports are below
#define HAVE_MMAP 0             // no mmap on Horizon: everything comes from sbrk
#define HAVE_MREMAP 0
#define HAVE_MORECORE 1
#define MORECORE sbrk           // libnx's heap (fake_heap_start..fake_heap_end), contiguous
#define MORECORE_CONTIGUOUS 1
#define MORECORE_CANNOT_TRIM 1  // never hand memory back: the heap is ours for the whole run anyway
#define DEFAULT_GRANULARITY ((size_t)1U << 20)  // grow the heap 1 MiB at a time
#undef MALLOC_ALIGNMENT                  // newlib's sys/config.h sets 16 too
#define MALLOC_ALIGNMENT ((size_t)16U)          // alignof(max_align_t) on AArch64
#define NO_MALLOC_STATS 1                       // malloc_stats would printf from inside the allocator
#define LACKS_SCHED_H 1
// A corrupt heap or a bad free crashes on the spot (Atmosphere crash report with the PC) instead of
// going through abort(), which this port wraps into a silent process exit (clang_tls_switch.c).
#define CORRUPTION_ERROR_ACTION(m) __builtin_trap()
#define USAGE_ERROR_ACTION(m, p) __builtin_trap()
// Locks: one libnx Mutex (kernel-arbitrated, sleeps instead of spinning: the GX, render and audio
// threads sit on different cores and priorities, and a spin lock could starve a lower-priority holder).
#define USE_LOCKS 2
#define MLOCK_T Mutex
#define INITIAL_LOCK(lk) (mutexInit(lk), 0)
#define DESTROY_LOCK(lk) (0)
#define ACQUIRE_LOCK(lk) (mutexLock(lk), 0)
#define RELEASE_LOCK(lk) mutexUnlock(lk)
#define TRY_LOCK(lk) mutexTryLock(lk)
static MLOCK_T malloc_global_mutex = 0;  // dlmalloc's global lock (initialisation, sbrk); 0 = unlocked

#include "dlmalloc/malloc-2.8.6.c"

// ---- statistics for the "[dk] heap" line ------------------------------------------------------------
// Relaxed counters: a few ns per call, read every 30 s.
static uint64_t sMallocCalls;
static uint64_t sFreeCalls;

// Caller sampling: every 256th allocation adds its return address to a small open-addressing table.
// Allocations through operator new pass the real caller (malloc_switch_new.cpp); for the rest it is the
// address malloc was called from. Lock-free (CAS on the key), so any thread may sample.
#define SAMPLE_EVERY 256u
#define SAMPLE_SLOTS 512u
static uintptr_t sSampleKey[SAMPLE_SLOTS];
static uint32_t sSampleCount[SAMPLE_SLOTS];
static uint32_t sSampleTick;

static void sample_caller(uintptr_t caller) {
  if ((__atomic_fetch_add(&sSampleTick, 1, __ATOMIC_RELAXED) & (SAMPLE_EVERY - 1)) != 0) {
    return;
  }
  uint32_t i = (uint32_t)((caller >> 2) * 2654435761u) & (SAMPLE_SLOTS - 1);
  for (uint32_t probe = 0; probe < 16; ++probe, i = (i + 1) & (SAMPLE_SLOTS - 1)) {
    uintptr_t key = __atomic_load_n(&sSampleKey[i], __ATOMIC_RELAXED);
    if (key == 0) {
      uintptr_t expected = 0;
      // claim the empty slot; if another thread got there first, use what it wrote
      key = __atomic_compare_exchange_n(&sSampleKey[i], &expected, caller, 0, __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED)
                ? caller
                : expected;
    }
    if (key == caller) {
      __atomic_fetch_add(&sSampleCount[i], 1, __ATOMIC_RELAXED);
      return;
    }
  }
  // neighbourhood full: drop the sample (the report is for the big callers, not all of them)
}

// Counts one allocation made for `caller` (also used by the operator new replacements).
void sfa_heap_note_alloc(uintptr_t caller) {
  __atomic_fetch_add(&sMallocCalls, 1, __ATOMIC_RELAXED);
  sample_caller(caller);
}

// ---- live-allocation tracking (leak finder) ----------------------------------------------------------
// The caller samples above count allocations, not what stays allocated. A leak shows as a steady rise of
// "in use" (hardware 2026-11-21: 1.7 -> 3.0 GB over 85 min, then deko3d's out-of-memory abort). Every
// 64th allocation is also remembered here (pointer, size, caller) until it is freed, so the heap line
// can rank callers by how much of what they allocated is still live, against the first report.
// Lock-free open addressing on the pointer: a slot is 0 (never used), 1 (freed, reusable) or a pointer.
// free() looks its pointer up (up to 8 probes, a few ns); a full neighbourhood drops the sample.
#define TRACK_EVERY 64u
#define TRACK_SLOTS (1u << 17)  // 128k samples = 8M allocations still live before samples drop
#define TRACK_PROBES 8u
static uintptr_t sTrackPtr[TRACK_SLOTS];
static uintptr_t sTrackSite[TRACK_SLOTS];
static uint32_t sTrackSize[TRACK_SLOTS];
static uint32_t sTrackTick;
static uint32_t sTrackDropped;

static inline uint32_t track_hash(uintptr_t p) { return (uint32_t)((p >> 4) * 2654435761u) & (TRACK_SLOTS - 1); }

// Remembers allocation p (n bytes, for caller) if it is the sampled one.
// Only with the - menu's Extra logging on (log_switch.h): off, an allocation costs one flag load here, and
// free()'s lookup ends at the first probe once the table has emptied.
void sfa_heap_track(void* p, size_t n, uintptr_t caller) {
  if (p == NULL || !sfa_log_extra() ||(__atomic_fetch_add(&sTrackTick, 1, __ATOMIC_RELAXED) & (TRACK_EVERY - 1)) != 0) {
    return;
  }
  uint32_t i = track_hash((uintptr_t)p);
  for (uint32_t probe = 0; probe < TRACK_PROBES; ++probe, i = (i + 1) & (TRACK_SLOTS - 1)) {
    uintptr_t cur = __atomic_load_n(&sTrackPtr[i], __ATOMIC_RELAXED);
    // claim a never-used or freed slot; the size and site go in before the pointer is published
    if (cur <= 1) {
      sTrackSite[i] = caller;
      sTrackSize[i] = n > 0xffffffffu ? 0xffffffffu : (uint32_t)n;
      if (__atomic_compare_exchange_n(&sTrackPtr[i], &cur, (uintptr_t)p, 0, __ATOMIC_RELEASE,
                                      __ATOMIC_RELAXED)) {
        return;
      }
    }
  }
  __atomic_fetch_add(&sTrackDropped, 1, __ATOMIC_RELAXED);
}

// Forgets p if it was sampled (called before the block is freed or moved by realloc).
static inline void track_forget(void* p) {
  uint32_t i = track_hash((uintptr_t)p);
  for (uint32_t probe = 0; probe < TRACK_PROBES; ++probe, i = (i + 1) & (TRACK_SLOTS - 1)) {
    uintptr_t cur = __atomic_load_n(&sTrackPtr[i], __ATOMIC_RELAXED);
    if (cur == (uintptr_t)p) {
      __atomic_compare_exchange_n(&sTrackPtr[i], &cur, (uintptr_t)1, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
      return;
    }
    if (cur == 0) return;  // a never-used slot ends the chain: p was not sampled
  }
}

// Per-caller live bytes: the first report's values are the baseline, later reports rank the growth.
#define SITE_SLOTS 4096u
static uintptr_t sSiteKey[SITE_SLOTS];
static uint64_t sSiteLive[SITE_SLOTS];  // sampled live bytes, summed at the current report
static uint64_t sSiteBase[SITE_SLOTS];  // the same at the first report (0 for callers first seen later)
static int sSiteHaveBase;

static uint32_t site_slot(uintptr_t site) {
  uint32_t i = (uint32_t)((site >> 2) * 2654435761u) & (SITE_SLOTS - 1);
  for (uint32_t probe = 0; probe < SITE_SLOTS; ++probe, i = (i + 1) & (SITE_SLOTS - 1)) {
    if (sSiteKey[i] == site || sSiteKey[i] == 0) {
      sSiteKey[i] = site;
      return i;
    }
  }
  return 0;  // table full (4096 distinct callers): lumps the rest into slot 0
}

// Image base for printing callers as .elf addresses (the PIE image is linked at 0): the start of the
// code segment this function lives in.
static uintptr_t image_base(void) {
  static uintptr_t base;
  if (base == 0) {
    MemoryInfo mi;
    u32 pi;
    if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, (u64)(uintptr_t)&image_base))) {
      base = (uintptr_t)mi.addr;
    }
  }
  return base;
}

// Snapshot for switch/deko/device.cpp: totals since start, heap sizes, and the top sampled callers since
// the last call (as .elf addresses; resolve with aarch64-none-elf-addr2line -f -C -e debug/SFA-NX-HHMM.elf).
// Takes the heap lock and walks every chunk (dlmallinfo): call it rarely.
struct SfaHeapStats {
  uint64_t mallocCalls, freeCalls;
  size_t footprint, inUse, freeInside, freeChunks, topFree;
  uintptr_t topCaller[8];
  uint32_t topCount[8];
  uint32_t sampleEvery;
  // leak finder: the 8 callers whose live memory grew most since the first report (estimated bytes, i.e.
  // sampled bytes x TRACK_EVERY), all sampled live memory, and samples dropped for a full table
  uintptr_t growCaller[8];
  int64_t growBytes[8];
  uint64_t trackedLive;
  uint32_t trackDropped;
};
void sfa_heap_stats(struct SfaHeapStats* out) {
  memset(out, 0, sizeof(*out));
  out->mallocCalls = __atomic_load_n(&sMallocCalls, __ATOMIC_RELAXED);
  out->freeCalls = __atomic_load_n(&sFreeCalls, __ATOMIC_RELAXED);
  const struct mallinfo mi = dlmallinfo();
  out->footprint = dlmalloc_footprint();
  out->inUse = mi.uordblks;
  out->freeInside = mi.fordblks;
  out->freeChunks = mi.ordblks;
  out->topFree = mi.keepcost;
  out->sampleEvery = SAMPLE_EVERY;
  // take the counts (reset them to 0) and keep the 8 largest
  const uintptr_t base = image_base();
  for (uint32_t i = 0; i < SAMPLE_SLOTS; ++i) {
    const uint32_t n = __atomic_exchange_n(&sSampleCount[i], 0, __ATOMIC_RELAXED);
    if (n == 0) continue;
    const uintptr_t key = __atomic_load_n(&sSampleKey[i], __ATOMIC_RELAXED);
    for (int k = 0; k < 8; ++k) {
      if (n > out->topCount[k]) {
        memmove(&out->topCount[k + 1], &out->topCount[k], (size_t)(7 - k) * sizeof(out->topCount[0]));
        memmove(&out->topCaller[k + 1], &out->topCaller[k], (size_t)(7 - k) * sizeof(out->topCaller[0]));
        out->topCount[k] = n;
        out->topCaller[k] = key - base;
        break;
      }
    }
  }
  // leak finder: sum the live samples per caller (one pass over the table, ~1 ms every 30 s). The first
  // report after Extra logging is switched on becomes the new baseline, so growth counts from then.
  static int sTrackWasOn;
  const int trackOn = sfa_log_extra();
  if (trackOn && !sTrackWasOn) sSiteHaveBase = 0;
  sTrackWasOn = trackOn;
  for (uint32_t s = 0; s < SITE_SLOTS; ++s) sSiteLive[s] = 0;
  for (uint32_t i = 0; i < TRACK_SLOTS; ++i) {
    const uintptr_t p = __atomic_load_n(&sTrackPtr[i], __ATOMIC_ACQUIRE);
    if (p <= 1) continue;
    sSiteLive[site_slot(sTrackSite[i])] += sTrackSize[i];
    out->trackedLive += (uint64_t)sTrackSize[i] * TRACK_EVERY;
  }
  for (uint32_t s = 0; s < SITE_SLOTS; ++s) {
    if (sSiteKey[s] == 0) continue;
    if (!sSiteHaveBase) sSiteBase[s] = sSiteLive[s];
    const int64_t grow = ((int64_t)sSiteLive[s] - (int64_t)sSiteBase[s]) * (int64_t)TRACK_EVERY;
    for (int k = 0; k < 8; ++k) {
      if (grow > out->growBytes[k]) {
        memmove(&out->growBytes[k + 1], &out->growBytes[k], (size_t)(7 - k) * sizeof(out->growBytes[0]));
        memmove(&out->growCaller[k + 1], &out->growCaller[k], (size_t)(7 - k) * sizeof(out->growCaller[0]));
        out->growBytes[k] = grow;
        out->growCaller[k] = sSiteKey[s] - base;
        break;
      }
    }
  }
  sSiteHaveBase = 1;
  out->trackDropped = __atomic_load_n(&sTrackDropped, __ATOMIC_RELAXED);
}

// ---- exported entry points (newlib's names) ---------------------------------------------------------
#define CALLER ((uintptr_t)__builtin_return_address(0))

// Each allocating entry point counts the call, samples the caller, and offers the block to the leak finder.
void* malloc(size_t n) {
  sfa_heap_note_alloc(CALLER);
  void* p = dlmalloc(n);
  sfa_heap_track(p, n, CALLER);
  return p;
}
void free(void* p) {
  if (p != NULL) {
    __atomic_fetch_add(&sFreeCalls, 1, __ATOMIC_RELAXED);
    track_forget(p);
  }
  dlfree(p);
}
void* calloc(size_t count, size_t size) {
  sfa_heap_note_alloc(CALLER);
  void* p = dlcalloc(count, size);
  sfa_heap_track(p, count * size, CALLER);
  return p;
}
// realloc may move the block: the old pointer is forgotten first, the result is offered again.
void* realloc(void* p, size_t n) {
  sfa_heap_note_alloc(CALLER);
  if (p != NULL) track_forget(p);
  void* q = dlrealloc(p, n);
  sfa_heap_track(q, n, CALLER);
  return q;
}
void* reallocf(void* p, size_t n) {
  sfa_heap_note_alloc(CALLER);
  if (p != NULL) track_forget(p);
  void* q = dlrealloc(p, n);
  if (q == NULL && n != 0) dlfree(p);  // reallocf frees the old block when it fails
  sfa_heap_track(q, n, CALLER);
  return q;
}
void* memalign(size_t align, size_t n) {
  sfa_heap_note_alloc(CALLER);
  void* p = dlmemalign(align, n);
  sfa_heap_track(p, n, CALLER);
  return p;
}
void* aligned_alloc(size_t align, size_t n) {
  sfa_heap_note_alloc(CALLER);
  void* p = dlmemalign(align, n);
  sfa_heap_track(p, n, CALLER);
  return p;
}
int posix_memalign(void** out, size_t align, size_t n) {
  sfa_heap_note_alloc(CALLER);
  const int rc = dlposix_memalign(out, align, n);
  if (rc == 0) sfa_heap_track(*out, n, CALLER);
  return rc;
}
void* valloc(size_t n) {
  sfa_heap_note_alloc(CALLER);
  void* p = dlvalloc(n);
  sfa_heap_track(p, n, CALLER);
  return p;
}
void* pvalloc(size_t n) {
  sfa_heap_note_alloc(CALLER);
  void* p = dlpvalloc(n);
  sfa_heap_track(p, n, CALLER);
  return p;
}
void cfree(void* p) { free(p); }
size_t malloc_usable_size(void* p) { return dlmalloc_usable_size(p); }
struct mallinfo mallinfo(void) { return dlmallinfo(); }  // same 10 size_t fields as newlib's struct
int malloc_trim(size_t pad) { return dlmalloc_trim(pad); }
int mallopt(int param, int value) { return dlmallopt(param, value); }
void malloc_stats(void) {}  // NO_MALLOC_STATS: the "[dk] heap" line reports instead

// newlib's reentrant forms (newlib's own code calls these directly); the reent argument is unused.
void* _malloc_r(struct _reent* r, size_t n) {
  (void)r;
  sfa_heap_note_alloc(CALLER);
  void* p = dlmalloc(n);
  sfa_heap_track(p, n, CALLER);
  return p;
}
void _free_r(struct _reent* r, void* p) {
  (void)r;
  free(p);
}
void* _calloc_r(struct _reent* r, size_t count, size_t size) {
  (void)r;
  sfa_heap_note_alloc(CALLER);
  void* p = dlcalloc(count, size);
  sfa_heap_track(p, count * size, CALLER);
  return p;
}
void* _realloc_r(struct _reent* r, void* p, size_t n) {
  (void)r;
  sfa_heap_note_alloc(CALLER);
  if (p != NULL) track_forget(p);
  void* q = dlrealloc(p, n);
  sfa_heap_track(q, n, CALLER);
  return q;
}
void* _reallocf_r(struct _reent* r, void* p, size_t n) {
  (void)r;
  return reallocf(p, n);
}
void* _memalign_r(struct _reent* r, size_t align, size_t n) {
  (void)r;
  sfa_heap_note_alloc(CALLER);
  void* p = dlmemalign(align, n);
  sfa_heap_track(p, n, CALLER);
  return p;
}
void* _valloc_r(struct _reent* r, size_t n) {
  (void)r;
  return valloc(n);
}
void* _pvalloc_r(struct _reent* r, size_t n) {
  (void)r;
  return pvalloc(n);
}
size_t _malloc_usable_size_r(struct _reent* r, void* p) {
  (void)r;
  return dlmalloc_usable_size(p);
}
struct mallinfo _mallinfo_r(struct _reent* r) {
  (void)r;
  return dlmallinfo();
}
int _malloc_trim_r(struct _reent* r, size_t pad) {
  (void)r;
  return dlmalloc_trim(pad);
}
int _mallopt_r(struct _reent* r, int param, int value) {
  (void)r;
  return dlmallopt(param, value);
}
void _malloc_stats_r(struct _reent* r) { (void)r; }
