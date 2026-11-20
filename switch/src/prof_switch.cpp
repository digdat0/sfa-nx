// SFA-NX profiler, switched on and off from the - menu (switch_menu.cpp, "Profiler").
//
// Two parts:
//  * Frame sections. port/src/vi_shim.c marks the frame's phases (sfaProfMark): game (game logic and
//    GX command building, from the frame opening to VIWaitForRetrace), present (overlays plus
//    aurora_end_frame: deko3d translation, submit and present), wait (the 60 Hz deadline) and acquire
//    (event pump and aurora_begin_frame, which waits for a free swapchain image). Shown live under
//    the FPS counter.
//  * Function samples. A thread on another core pauses the main thread 1000 times a second, reads its
//    PC and LR (svcGetThreadContext3) and resumes it. A thread blocked in libnx mutexLock is also
//    attributed to its lock: the mutex address and four return addresses from the stack (the "K" rows).
//    Samples are counted per (pc, lr) in a fixed
//    table: nothing allocates or prints while the main thread is stopped, and the sampler takes the
//    table lock before pausing, so it never stops the main thread while that thread holds the lock.
//
// Reports go to sdmc:/switch/SFA-NX/logs/profile-<n>.txt when the profiler is switched off and every
// two minutes while it runs. Incremental: every report is a new file holding only the time since the
// previous one (header: session, part, from = seconds into the session). switch/tools/prof_report.py
// turns a report into per-function times with the build's .elf
// from debug/ (the header's build stamp names it: SFA-NX-HHMM.elf).
#include <switch.h>

#include <atomic>
#include <cstdio>
#include <cstring>

extern "C" void pc_log_line(const char* fmt, ...);
extern "C" const char sfa_nx_build_stamp[]; // main_sfa.cpp
extern "C" void sfa_nx_prof_anchor(void);
extern "C" void fhAIStats(int* underruns, int* low, int* maxWaitUs); // port/src/ai_shim.c
extern "C" bool aurora_direct_renderer(void);                         // switch/deko/direct.cpp
extern "C" bool aurora_frame_overlap(void);                           // extern/aurora/lib/aurora.cpp

namespace {

// Sampling rate, table sizes and the periodic report interval (two minutes).
constexpr const char* kLogDir = "sdmc:/switch/SFA-NX/logs";
constexpr u32 kHz = 1000;
constexpr int kMaxTargets = 8; // main, FIFO, render, shader compiler, audio, spare
constexpr u32 kTableSize = 1u << 15; // power of two
constexpr u64 kDumpEveryNs = 120ull * 1000000000ull;

// One open-addressed hash table entry: how often a thread of `role` was caught at this pc with this lr.
// The lr adds the caller, which tells apart time spent in a shared helper (memcpy, a lock) by call site.
struct Slot {
    u64 pc;
    u64 lr;
    u32 count;
    u32 role;
};
Slot g_table[kTableSize];
Slot g_copy[kTableSize]; // filled under the lock, written to the SD card after it
u64 g_samples, g_dropped, g_failed;

// Lock waits: which mutex a blocked thread waits for, and from where.
constexpr u32 kLockTableSize = 1u << 12;
constexpr int kLockDepth = 4;
struct LockSlot {
    u64 mutex;
    u64 ret[kLockDepth];
    u32 count;
    u32 role;
};
LockSlot g_locks[kLockTableSize];
LockSlot g_lockCopy[kLockTableSize];

// Per-frame sample log used for slow-frame reports. kind 0: a = pc, b = lr; kind 1 (blocked on a lock):
// a = mutex, b/c = return addresses. section is the frame phase (game/present/wait/acquire) at that moment.
struct FrameSample {
    u64 a, b, c;
    u8 role, section, kind, pad[5];
};
constexpr u32 kFrameMax = 512;
constexpr u32 kSlowMax = 256;
constexpr u32 kPoolMax = 1u << 15;
// A frame longer than kSlowNs (20 ms) keeps its samples: up to kSlowMax slow frames per report, their
// samples copied from g_frame into the shared pool g_pool.
constexpr u64 kSlowNs = 20000000ull;
struct SlowFrame {
    u64 tNs, frame;
    u32 secUs[4];
    u32 first, n, lost, flags;
};
FrameSample g_frame[kFrameMax];
u32 g_frameN, g_frameLost;
SlowFrame g_slow[kSlowMax];
FrameSample g_pool[kPoolMax];
u32 g_slowN, g_poolN, g_slowLost, g_pendingFlags;
u64 g_frameNo;
std::atomic<int> g_section{0};

constexpr int kSectionsN = 4;
// Everything one report needs, snapshotted by dump() on the game thread and written to the SD card by the
// writer thread, so the game thread never waits for the card.
struct DumpJob {
    const char* why;
    u64 samples, dropped, failed, frames, slowFrames;
    u64 sumNs[kSectionsN];
    double fromS, seconds;
    int part, session, underruns, low, maxWaitUs;
    bool direct, overlap;
    u32 slowN, slowLost;
};
DumpJob g_job;
SlowFrame g_slowCopy[kSlowMax];
FrameSample g_poolCopy[kPoolMax];
std::atomic<bool> g_writing{false};
Thread g_writer;
bool g_writerStarted;
alignas(0x1000) u8 g_writerStack[0x8000];
char g_fileBuf[64 * 1024];

// Adds a sample to the current frame's log (sampler thread, under g_lock).
void frame_add(u32 role, u8 kind, u64 a, u64 b, u64 c) {
    if (g_frameN < kFrameMax) {
        g_frame[g_frameN++] = {a, b, c, static_cast<u8>(role),
                               static_cast<u8>(g_section.load(std::memory_order_relaxed)), kind, {}};
    } else {
        ++g_frameLost;
    }
}

// Threads being sampled: the main thread plus the workers that registered through melee_nx_prof_register.
struct Target {
    Handle handle;
    u32 role;
};
Target g_targets[kMaxTargets];
int g_targetCount;

// g_lock guards the tables and targets; the sampler holds it while a thread is paused, so a sampled thread
// can never be paused while it holds g_lock itself.
Mutex g_lock; // zero = unlocked
std::atomic<bool> g_enabled{false};
bool g_threadStarted;
Thread g_thread;
alignas(0x1000) u8 g_stack[0x8000];

// A session runs from switching the profiler on to switching it off; each report is one part of it.
u64 g_sessionStartNs, g_partStartNs, g_lastDumpNs;
int g_fileIndex = -1; // next free profile-NNN.txt, found once per boot
int g_session, g_part;

// Frame sections: 0 game, 1 present, 2 wait, 3 acquire.
constexpr int kSections = 4;
u64 g_markNs[4];
u64 g_secSumNs[kSections], g_secMaxNs[kSections];
u32 g_secFrames;
double g_shownAvgMs[kSections], g_shownMaxMs[kSections];
u64 g_shownAtNs;
// Session totals for the report header.
u64 g_totSumNs[kSections];
u64 g_totFrames, g_slowFrames;

u64 now_ns() { return armTicksToNs(armGetSystemTick()); }

// Counts one (role, pc, lr) sample with linear probing; gives up after 64 probes and counts it as dropped
// (the table never grows, so nothing allocates while a thread is paused).
void insert(u32 role, u64 pc, u64 lr) {
    const u64 h = (pc * 0x9E3779B97F4A7C15ull) ^ (lr * 0xC2B2AE3D27D4EB4Full) ^ role;
    for (u32 probe = 0; probe < 64; ++probe) {
        Slot& s = g_table[(h + probe) & (kTableSize - 1)];
        if (s.count == 0) {
            s = {pc, lr, 1, role};
            return;
        }
        if (s.pc == pc && s.lr == lr && s.role == role) {
            ++s.count;
            return;
        }
    }
    ++g_dropped;
}

// Same for lock waits, keyed on the mutex and its four return addresses.
void insert_lock(u32 role, const LockSlot& k) {
    u64 h = k.mutex * 0x9E3779B97F4A7C15ull ^ role;
    for (int i = 0; i < kLockDepth; ++i) {
        h = (h ^ k.ret[i]) * 0xC2B2AE3D27D4EB4Full;
    }
    for (u32 probe = 0; probe < 32; ++probe) {
        LockSlot& s = g_locks[(h + probe) & (kLockTableSize - 1)];
        if (s.count == 0) {
            s = k;
            s.count = 1;
            s.role = role;
            return;
        }
        if (s.role == role && s.mutex == k.mutex && std::memcmp(s.ret, k.ret, sizeof(k.ret)) == 0) {
            ++s.count;
            return;
        }
    }
    ++g_dropped;
}

// The thread is paused inside svcArbitrateLock called from libnx mutexLock, which saved its frame as
// stp x29, x30, [sp, #-32]! and the mutex pointer at [sp, #16] (checked against this build's
// mutexLock: the return address must follow a bl). Reads stay inside the stack's mapping; the
// frame-pointer chain stops at the first frame that does not look like one.
bool sample_lock(u32 role, const ThreadContext& ctx, LockSlot* out) {
    const u64 sp = ctx.sp;
    MemoryInfo mi{};
    u32 page = 0;
    if (R_FAILED(svcQueryMemory(&mi, &page, sp)) || (mi.perm & Perm_Rw) != Perm_Rw) {
        return false;
    }
    const u64 end = mi.addr + mi.size;
    if ((sp & 15) != 0 || sp + 32 > end) {
        return false;
    }
    LockSlot k{};
    k.mutex = *reinterpret_cast<const u64*>(sp + 16);
    k.ret[0] = *reinterpret_cast<const u64*>(sp + 8);
    u64 fp = *reinterpret_cast<const u64*>(sp);
    for (int i = 1; i < kLockDepth; ++i) {
        if ((fp & 15) != 0 || fp < sp || fp + 16 > end) {
            break;
        }
        k.ret[i] = *reinterpret_cast<const u64*>(fp + 8);
        const u64 next = *reinterpret_cast<const u64*>(fp);
        if (next <= fp) {
            break;
        }
        fp = next;
    }
    insert_lock(role, k);
    *out = k;
    return true;
}

// Sampler thread: every 1 ms pauses each target, reads its registers, resumes it, and records the sample.
// The lock-wait check only runs when the thread is exactly at the return point of mutexLock's svc call,
// which is verified once at start (the instruction before it must be a bl), so a different libnx build
// can only disable the K rows, never misread a stack.
void sampler_main(void*) {
    const u64 svcLock = reinterpret_cast<u64>(&svcArbitrateLock);
    const u64 lockRet = reinterpret_cast<u64>(&mutexLock) + 0x50; // after mutexLock's bl svcArbitrateLock
    const bool lockRetOk = (*reinterpret_cast<const u32*>(lockRet - 4) >> 26) == 0x25; // a bl there
    const u64 period = 1000000000ull / kHz;
    u64 next = now_ns();
    for (;;) {
        if (!g_enabled.load(std::memory_order_acquire)) {
            svcSleepThread(50000000ll);
            next = now_ns();
            continue;
        }
        const u64 now = now_ns();
        if (next > now) {
            svcSleepThread(static_cast<s64>(next - now));
        }
        next = now_ns() + period; // no catch-up bursts
        mutexLock(&g_lock);
        for (int i = 0; i < g_targetCount; ++i) {
            if (R_FAILED(svcSetThreadActivity(g_targets[i].handle, ThreadActivity_Paused))) {
                ++g_failed;
                continue;
            }
            ThreadContext ctx;
            const Result rc = svcGetThreadContext3(&ctx, g_targets[i].handle);
            LockSlot k{};
            bool locked = false;
            if (R_SUCCEEDED(rc) && lockRetOk && ctx.lr == lockRet && ctx.pc.x - svcLock < 8) {
                locked = sample_lock(g_targets[i].role, ctx, &k); // still paused: its stack holds still
            }
            svcSetThreadActivity(g_targets[i].handle, ThreadActivity_Runnable);
            if (R_SUCCEEDED(rc)) {
                if (locked) {
                    frame_add(g_targets[i].role, 1, k.mutex, k.ret[0], k.ret[1]);
                } else {
                    frame_add(g_targets[i].role, 0, ctx.pc.x, ctx.lr, 0);
                }
                insert(g_targets[i].role, ctx.pc.x, ctx.lr);
                ++g_samples;
            } else {
                ++g_failed;
            }
        }
        mutexUnlock(&g_lock);
    }
}

// Adds a thread to sample, once per role.
void add_target(Handle handle, u32 role) {
    mutexLock(&g_lock);
    bool known = false;
    for (int i = 0; i < g_targetCount; ++i) {
        known |= g_targets[i].role == role;
    }
    if (!known && g_targetCount < kMaxTargets) {
        g_targets[g_targetCount++] = {handle, role};
    }
    mutexUnlock(&g_lock);
}

void writer_main(void*);

// Starts the sampler and report writer threads on first use. The writer runs at a low priority (0x3B):
// a report taking longer to write is fine, a delayed game frame is not.
bool start_thread() {
    if (g_threadStarted) {
        return true;
    }
    // Highest core the process may use (the main thread runs on the lowest), at a priority above the
    // game's threads so samples are not delayed.
    u64 allowed = 0;
    int core = -2; // libnx: default core
    if (R_SUCCEEDED(svcGetInfo(&allowed, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0))) {
        for (int c = 0; c < 64; ++c) {
            if (allowed & (1ull << c)) {
                core = c;
            }
        }
    }
    Result rc = threadCreate(&g_thread, sampler_main, nullptr, g_stack, sizeof(g_stack), 0x20, core);
    if (R_SUCCEEDED(rc)) {
        rc = threadStart(&g_thread);
    }
    pc_log_line("[prof] sampler thread core %d rc 0x%x", core, rc);
    g_threadStarted = R_SUCCEEDED(rc);
    if (g_threadStarted && !g_writerStarted) {
        Result wrc = threadCreate(&g_writer, writer_main, nullptr, g_writerStack, sizeof(g_writerStack), 0x3B, core);
        if (R_SUCCEEDED(wrc)) {
            wrc = threadStart(&g_writer);
        }
        g_writerStarted = R_SUCCEEDED(wrc);
        pc_log_line("[prof] writer thread rc 0x%x", wrc);
    }
    return g_threadStarted;
}

// Writer thread: writes g_job and the copied tables to the next free logs/profile-NNN.txt. Format: header
// lines, then "role count pc lr" rows, "K" lock rows, and "S"/"s" slow-frame rows (see the # lines).
void write_report() {
    const DumpJob& j = g_job;
    if (g_fileIndex < 0) {
        for (g_fileIndex = 1; g_fileIndex < 999; ++g_fileIndex) {
            char probe[96];
            std::snprintf(probe, sizeof(probe), "%s/profile-%03d.txt", kLogDir, g_fileIndex);
            FILE* f = std::fopen(probe, "r");
            if (f == nullptr) {
                break;
            }
            std::fclose(f);
        }
    }
    char path[96];
    std::snprintf(path, sizeof(path), "%s/profile-%03d.txt", kLogDir, g_fileIndex);
    ++g_fileIndex;
    const u64 t0 = now_ns();
    FILE* f = std::fopen(path, "w");
    if (f == nullptr) {
        pc_log_line("[prof] cannot write %s", path);
        return;
    }
    std::setvbuf(f, g_fileBuf, _IOFBF, sizeof(g_fileBuf));
    std::fprintf(f,
                 "SFA-NX profile v2\nbuild %s\nanchor 0x%llx\nhz %u\nsession %d\npart %d\nfrom %.1f\n"
                 "seconds %.1f\nsamples %llu\ndropped %llu\nfailed %llu\nreason %s\n",
                 sfa_nx_build_stamp,
                 static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(&sfa_nx_prof_anchor)), kHz,
                 j.session, j.part, j.fromS, j.seconds, static_cast<unsigned long long>(j.samples),
                 static_cast<unsigned long long>(j.dropped), static_cast<unsigned long long>(j.failed), j.why);
    static const char* const names[kSectionsN] = {"game", "present", "wait", "acquire"};
    std::fprintf(f, "frames %llu\nslow_frames %llu\n", static_cast<unsigned long long>(j.frames),
                 static_cast<unsigned long long>(j.slowFrames));
    for (int i = 0; i < kSectionsN; ++i) {
        std::fprintf(f, "section %s %.3f\n", names[i],
                     j.frames ? static_cast<double>(j.sumNs[i]) / 1e6 / static_cast<double>(j.frames) : 0.0);
    }
    std::fprintf(f, "audio_underruns %d\naudio_low %d\naudio_lock_wait_max_ms %.2f\n", j.underruns, j.low,
                 j.maxWaitUs / 1000.0);
    std::fprintf(f, "renderer %s\nframe_overlap %d\n", j.direct ? "direct" : "dawn", j.overlap ? 1 : 0);
    std::fprintf(f, "# role count pc lr\n");
    for (u32 i = 0; i < kTableSize; ++i) {
        const Slot& s = g_copy[i];
        if (s.count != 0) {
            std::fprintf(f, "%u %u %llx %llx\n", s.role, s.count, static_cast<unsigned long long>(s.pc),
                         static_cast<unsigned long long>(s.lr));
        }
    }
    std::fprintf(f, "# K role count mutex ret0 ret1 ret2 ret3\n");
    for (u32 i = 0; i < kLockTableSize; ++i) {
        const LockSlot& s = g_lockCopy[i];
        if (s.count != 0) {
            std::fprintf(f, "K %u %u %llx %llx %llx %llx %llx\n", s.role, s.count,
                         static_cast<unsigned long long>(s.mutex), static_cast<unsigned long long>(s.ret[0]),
                         static_cast<unsigned long long>(s.ret[1]), static_cast<unsigned long long>(s.ret[2]),
                         static_cast<unsigned long long>(s.ret[3]));
        }
    }
    std::fprintf(f, "slow_recorded %u\nslow_unrecorded %u\n", j.slowN, j.slowLost);
    std::fprintf(f, "# S frame t_ms game_ms present_ms wait_ms acquire_ms samples lost flags\n"
                    "# s role section kind a b c (kind 0: pc lr, kind 1: mutex ret0 ret1)\n");
    for (u32 i = 0; i < j.slowN; ++i) {
        const SlowFrame& s = g_slowCopy[i];
        std::fprintf(f, "S %llu %.1f %.2f %.2f %.2f %.2f %u %u %u\n", static_cast<unsigned long long>(s.frame),
                     static_cast<double>(s.tNs) / 1e6, s.secUs[0] / 1000.0, s.secUs[1] / 1000.0,
                     s.secUs[2] / 1000.0, s.secUs[3] / 1000.0, s.n, s.lost, s.flags);
        for (u32 k = 0; k < s.n; ++k) {
            const FrameSample& p = g_poolCopy[s.first + k];
            std::fprintf(f, "s %u %u %u %llx %llx %llx\n", p.role, p.section, p.kind,
                         static_cast<unsigned long long>(p.a), static_cast<unsigned long long>(p.b),
                         static_cast<unsigned long long>(p.c));
        }
    }
    std::fclose(f);
    pc_log_line("[prof] %s: %llu samples, %.0f s -> %s (written in %.0f ms)", j.why,
                static_cast<unsigned long long>(j.samples), j.seconds, path,
                static_cast<double>(now_ns() - t0) / 1e6);
}

// Writer thread loop: waits for dump() to hand over a job, writes it, marks it done.
void writer_main(void*) {
    for (;;) {
        if (!g_writing.load(std::memory_order_acquire)) {
            svcSleepThread(50000000ll);
            continue;
        }
        write_report();
        g_writing.store(false, std::memory_order_release);
    }
}

// Game thread: snapshots and resets the counters into g_job and wakes the writer. A periodic report is
// skipped if the previous one is still being written; a final one ("stopped") waits for it.
void dump(const char* why) {
    // Incremental: each report holds only the time since the previous one (counters reset here), in a
    // new numbered file; prof_report.py sums several files when given more than one.
    if (!g_writerStarted) {
        return;
    }
    if (g_writing.load(std::memory_order_acquire)) {
        if (std::strcmp(why, "periodic") == 0) {
            return;
        }
        while (g_writing.load(std::memory_order_acquire)) {
            svcSleepThread(1000000ll);
        }
    }
    mutexLock(&g_lock);
    std::memcpy(g_copy, g_table, sizeof(g_table));
    const u64 samples = g_samples, dropped = g_dropped, failed = g_failed;
    std::memset(g_table, 0, sizeof(g_table));
    std::memcpy(g_lockCopy, g_locks, sizeof(g_locks));
    std::memset(g_locks, 0, sizeof(g_locks));
    g_samples = g_dropped = g_failed = 0;
    mutexUnlock(&g_lock);
    const u64 nowNs = now_ns();
    const double fromS = static_cast<double>(g_partStartNs - g_sessionStartNs) / 1e9;
    const double seconds = static_cast<double>(nowNs - g_partStartNs) / 1e9;
    const u64 frames = g_totFrames, slowFrames = g_slowFrames;
    u64 sumNs[kSections];
    std::memcpy(sumNs, g_totSumNs, sizeof(sumNs));
    std::memset(g_totSumNs, 0, sizeof(g_totSumNs));
    g_totFrames = g_slowFrames = 0;
    g_partStartNs = nowNs;
    const int part = ++g_part;

    DumpJob& j = g_job;
    j.why = why;
    j.samples = samples;
    j.dropped = dropped;
    j.failed = failed;
    j.frames = frames;
    j.slowFrames = slowFrames;
    std::memcpy(j.sumNs, sumNs, sizeof(j.sumNs));
    j.fromS = fromS;
    j.seconds = seconds;
    j.part = part;
    j.session = g_session;
    fhAIStats(&j.underruns, &j.low, &j.maxWaitUs);
    j.direct = aurora_direct_renderer();
    j.overlap = aurora_frame_overlap();
    mutexLock(&g_lock);
    j.slowN = g_slowN;
    j.slowLost = g_slowLost;
    std::memcpy(g_slowCopy, g_slow, g_slowN * sizeof(SlowFrame));
    std::memcpy(g_poolCopy, g_pool, g_poolN * sizeof(FrameSample));
    g_slowN = g_poolN = g_slowLost = 0;
    mutexUnlock(&g_lock);
    g_pendingFlags = 1;
    g_writing.store(true, std::memory_order_release);
}

} // namespace

// Address anchor: the report subtracts this symbol's .elf address from the recorded one to get the
// load address.
extern "C" __attribute__((noinline, used)) void sfa_nx_prof_anchor(void) { __asm__ volatile(""); }

// Called from the main (game) thread by the - menu.
extern "C" void sfaProfSetEnabled(int on) {
    if ((on != 0) == g_enabled.load()) {
        return;
    }
    if (on) {
        add_target(envGetMainThreadHandle(), 0);
        if (!start_thread()) {
            return;
        }
        mutexLock(&g_lock);
        std::memset(g_table, 0, sizeof(g_table));
        g_samples = g_dropped = g_failed = 0;
        g_frameN = g_frameLost = 0;
        mutexUnlock(&g_lock);
        g_slowN = g_poolN = g_slowLost = g_pendingFlags = 0;
        std::memset(g_totSumNs, 0, sizeof(g_totSumNs));
        g_totFrames = g_slowFrames = 0;
        g_part = 0;
        ++g_session;
        g_sessionStartNs = g_partStartNs = g_lastDumpNs = now_ns();
        g_enabled.store(true, std::memory_order_release);
        pc_log_line("[prof] on");
    } else {
        g_enabled.store(false, std::memory_order_release);
        dump("stopped");
    }
}

extern "C" int sfaProfEnabled(void) { return g_enabled.load() ? 1 : 0; }

// Frame phase marks from vi_shim.c (main thread): 0 frame opened, 1 VIWaitForRetrace entered,
// 2 aurora_end_frame done, 3 retrace deadline reached. A frame closes at the next mark 0.
extern "C" void sfaProfMark(int id) {
    if (id < 0 || id > 3) {
        return;
    }
    const u64 t = now_ns();
    g_section.store(id, std::memory_order_relaxed);
    if (id == 0 && g_enabled.load(std::memory_order_relaxed)) {
        const bool valid = g_markNs[0] != 0 && g_markNs[1] > g_markNs[0] && g_markNs[2] >= g_markNs[1] &&
                           g_markNs[3] >= g_markNs[2];
        const bool slow = valid && t - g_markNs[0] > kSlowNs;
        mutexLock(&g_lock);
        if (slow && g_slowN < kSlowMax) {
            SlowFrame& s = g_slow[g_slowN++];
            s.tNs = t - g_sessionStartNs;
            s.frame = g_frameNo;
            s.secUs[0] = static_cast<u32>((g_markNs[1] - g_markNs[0]) / 1000);
            s.secUs[1] = static_cast<u32>((g_markNs[2] - g_markNs[1]) / 1000);
            s.secUs[2] = static_cast<u32>((g_markNs[3] - g_markNs[2]) / 1000);
            s.secUs[3] = static_cast<u32>((t - g_markNs[3]) / 1000);
            const u32 room = kPoolMax - g_poolN;
            const u32 n = g_frameN < room ? g_frameN : room;
            s.first = g_poolN;
            s.n = n;
            s.lost = g_frameLost + (g_frameN - n);
            s.flags = g_pendingFlags;
            std::memcpy(&g_pool[g_poolN], g_frame, n * sizeof(FrameSample));
            g_poolN += n;
        } else if (slow) {
            ++g_slowLost;
        }
        g_frameN = g_frameLost = 0;
        mutexUnlock(&g_lock);
        if (valid) {
            g_pendingFlags = 0;
        }
        ++g_frameNo;
    }
    if (id == 0 && g_markNs[0] != 0 && g_markNs[1] > g_markNs[0] && g_markNs[2] >= g_markNs[1] &&
        g_markNs[3] >= g_markNs[2]) {
        const u64 sec[kSections] = {g_markNs[1] - g_markNs[0], g_markNs[2] - g_markNs[1],
                                    g_markNs[3] - g_markNs[2], t - g_markNs[3]};
        u64 total = 0;
        for (int i = 0; i < kSections; ++i) {
            g_secSumNs[i] += sec[i];
            g_secMaxNs[i] = sec[i] > g_secMaxNs[i] ? sec[i] : g_secMaxNs[i];
            g_totSumNs[i] += sec[i];
            total += sec[i];
        }
        ++g_secFrames;
        ++g_totFrames;
        // The live display (shown under the FPS counter) is refreshed every half second.
        if (total > 20000000ull) { // longer than 1.2 refreshes at 60 Hz
            ++g_slowFrames;
        }
        if (t - g_shownAtNs >= 500000000ull) {
            for (int i = 0; i < kSections; ++i) {
                g_shownAvgMs[i] = static_cast<double>(g_secSumNs[i]) / 1e6 / g_secFrames;
                g_shownMaxMs[i] = static_cast<double>(g_secMaxNs[i]) / 1e6;
                g_secSumNs[i] = g_secMaxNs[i] = 0;
            }
            g_secFrames = 0;
            g_shownAtNs = t;
        }
        if (g_enabled.load() && t - g_lastDumpNs >= kDumpEveryNs) {
            g_lastDumpNs = t;
            dump("periodic");
        }
    }
    g_markNs[id] = t;
}

// Last half second of frame sections in ms (game, present, wait, acquire) and the sample count.
extern "C" void sfaProfSections(double* avgMs, double* maxMs, unsigned long long* samples) {
    for (int i = 0; i < kSections; ++i) {
        avgMs[i] = g_shownAvgMs[i];
        maxMs[i] = g_shownMaxMs[i];
    }
    *samples = g_samples;
}

// Kept for perf_switch.cpp (Melee-NX thread placement): a worker that names itself becomes a target.
extern "C" void melee_nx_prof_register(int role) {
    if (role > 0) {
        add_target(threadGetCurHandle(), static_cast<u32>(role));
    }
}
extern "C" void melee_nx_prof_init(void) {}
