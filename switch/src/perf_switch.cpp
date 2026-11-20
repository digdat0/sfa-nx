// perf.cfg parsing, thread placement and frame-time statistics (interface in perf_switch.h).
//
// The Switch gives homebrew three CPU cores (0-2; core 3 belongs to the system). Where the game thread,
// Aurora's FIFO and render threads and the audio thread run makes a large difference, so each of them
// registers through melee_nx_perf_thread and place_thread applies the current policy. The - menu can change
// the policy at run time, which moves every registered thread at once.
#include "perf_switch.h"
#include <switch.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

extern "C" void pc_log_line(const char* fmt, ...);

namespace {
// Every `key value` line of perf.cfg with a numeric value (decimal or 0x hex), for melee_nx_perf_int
// and melee_nx_opt. The named keys parsed in melee_nx_perf_init keep their own handling.
struct CfgEntry {
    char key[32];
    long value;
};
constexpr int MaxCfgEntries = 64;
std::array<CfgEntry, MaxCfgEntries> cfg_entries{};
int cfg_count = 0;
const CfgEntry* cfg_find(const char* key) {
    for (int i = 0; i < cfg_count; ++i)
        if (std::strcmp(cfg_entries[i].key, key) == 0)
            return &cfg_entries[i];
    return nullptr;
}
std::mutex opt_log_mutex;
std::array<char[32], MaxCfgEntries> opt_logged{};
int opt_logged_count = 0;

enum class Affinity { Baseline, Main, Split, Tuned, Game };
// Tuned by default since 2026-11-19: the game thread alone on the middle core, the other threads on the
// rest. The GPU hang that sent this back to baseline on 2026-11-18 (2348-0006) also happened under baseline;
// the GPFIFO overflow fixed in build 2009 explains it. The - menu "Threads" item switches the policy live
// (switch_menu.ini threads=), and perf.cfg affinity= still picks the start value when the ini has none.
Affinity affinity = Affinity::Tuned;
const char* affinity_name = "tuned";
// Knobs passed to Aurora as environment variables in melee_nx_perf_init (see there). They come from
// Melee-NX's patched Aurora; Foxhollow's Aurora does not read any of these variables, so in SFA-NX they
// are only logged.
int compile_workers = 1;
int monolithic_cache = 0;
int launch_warm = 1; // 0 skips the startup shader warm-up
int gx_skip_unready = 0;
int warm_helpers = 0;
int uber = 0; // 0 off, 1 hybrid, 2 always (where supported); measured too slow on the GPU, see docs
// Frame-time histogram for the current 2 s window, plus per-stage totals and call counts.
constexpr uint64_t BinNs = 250000; // Report percentile upper bounds, 0.25 ms bins.
constexpr size_t BinCount = 4000;  // >=1000 ms is reported as overflow.
std::array<uint32_t, BinCount> bins{};
std::array<uint64_t, MELEE_PERF_STAGE_COUNT> stages{};
std::array<uint32_t, MELEE_PERF_STAGE_COUNT> calls{};
uint64_t window_ns, worst_ns;
uint32_t frames, overflow, late;

// Upper bound (in ms) of the histogram bin holding the given percentile of this window's frames.
double percentile(unsigned percent) {
    const uint32_t rank = (frames * percent + 99) / 100;
    uint32_t seen = 0;
    for (size_t i = 0; i < bins.size(); ++i) {
        seen += bins[i];
        if (seen >= rank)
            return (i + 1) * BinNs / 1e6;
    }
    return -1; // Quantile is beyond the histogram range; never clamp silently.
}
// Average time per call of one stage in this window, in ms.
double average(size_t stage) {
    return calls[stage] ? stages[stage] / (double)calls[stage] / 1e6 : 0;
}
} // namespace

// Reads perf.cfg once at startup (before Aurora creates its threads), stores every numeric value for
// melee_nx_perf_int / melee_nx_opt, handles the named keys below, and exports the Aurora knobs.
extern "C" void melee_nx_perf_init(void) {
    if (FILE* file = std::fopen("sdmc:/switch/SFA-NX/config/perf.cfg", "r")) {
        char line[128];
        while (std::fgets(line, sizeof(line), file)) {
            char key[32], value[32], extra[2];
            if (std::sscanf(line, " %31s %31s %1s", key, value, extra) != 2)
                continue;
            if (key[0] != '#' && cfg_count < MaxCfgEntries && !cfg_find(key)) {
                char* end = nullptr;
                const long number = std::strtol(value, &end, 0);
                if (end != value && *end == '\0') {
                    std::memcpy(cfg_entries[cfg_count].key, key, sizeof(key));
                    cfg_entries[cfg_count++].value = number;
                }
            }
            if (std::strcmp(key, "affinity") == 0) {
                if (std::strcmp(value, "main") == 0) {
                    affinity = Affinity::Main;
                    affinity_name = "main";
                } else if (std::strcmp(value, "split") == 0) {
                    affinity = Affinity::Split;
                    affinity_name = "split";
                } else if (std::strcmp(value, "tuned") == 0) {
                    affinity = Affinity::Tuned;
                    affinity_name = "tuned";
                } else if (std::strcmp(value, "game") == 0) {
                    affinity = Affinity::Game;
                    affinity_name = "game";
                } else if (std::strcmp(value, "baseline") == 0) {
                    affinity = Affinity::Baseline;
                    affinity_name = "baseline";
                }
            } else if (std::strcmp(key, "compile_workers") == 0) {
                compile_workers = (std::strcmp(value, "2") == 0) ? 2 : 1;
            } else if (std::strcmp(key, "monolithic_cache") == 0) {
                monolithic_cache = (std::strcmp(value, "1") == 0) ? 1 : 0;
            } else if (std::strcmp(key, "uber") == 0) {
                const int mode = std::atoi(value);
                uber = mode < 0 ? 0 : (mode > 2 ? 2 : mode);
            } else if (std::strcmp(key, "warm_helpers") == 0) {
                const int count = std::atoi(value);
                warm_helpers = count < 0 ? 0 : (count > 2 ? 2 : count);
            } else if (std::strcmp(key, "gx_skip_unready") == 0) {
                gx_skip_unready = (std::strcmp(value, "1") == 0) ? 1 : 0;
            } else if (std::strcmp(key, "launch_warm") == 0) {
                launch_warm = std::atoi(value) != 0 ? 1 : 0;
            }
        }
        std::fclose(file);
    }
    // Bridge perf.cfg knobs to aurora, which reads these env vars when it creates
    // the device / pipeline cache later in melee's main().
    setenv("AURORA_COMPILE_WORKERS", compile_workers == 2 ? "2" : "1", 1);
    // The uber pipelines all share one shader; without a shared Vulkan pipeline cache
    // each would recompile it.
    // (The comment above dates from Melee-NX's Vulkan backend; Foxhollow's Aurora ignores the variable.)
    setenv("AURORA_MONOLITHIC_PIPELINE_CACHE", (monolithic_cache == 1 || uber > 0) ? "1" : "0", 1);
    char warm[16];
    std::snprintf(warm, sizeof(warm), "%d", launch_warm);
    setenv("AURORA_LAUNCH_WARM_SECONDS", warm, 1);
    setenv("AURORA_GX_SKIP_UNREADY", gx_skip_unready == 1 ? "1" : "0", 1);
    char helpers[8];
    std::snprintf(helpers, sizeof(helpers), "%d", warm_helpers);
    setenv("AURORA_LAUNCH_WARM_HELPERS", helpers, 1);
    if (uber > 0) {
        // A previous run turned the uber-shader off after frames collapsed under it.
        if (FILE* marker = std::fopen("sdmc:/switch/SFA-NX/config/uber.tripped", "rb")) {
            std::fclose(marker);
            uber = 0;
            pc_log_line("SWEEP uber disabled by sdmc:/switch/SFA-NX/config/uber.tripped");
        }
    }
    char uberMode[8];
    std::snprintf(uberMode, sizeof(uberMode), "%d", uber);
    setenv("AURORA_UBER", uberMode, 1);
    pc_log_line(
        "SWEEP affinity %s compile_workers %d monolithic_cache %d launch_warm %d gx_skip_unready %d "
        "histogram_bin_ms 0.25 histogram_limit_ms 1000",
        affinity_name, compile_workers, monolithic_cache, launch_warm, gx_skip_unready);
}

// Any numeric perf.cfg value by name.
extern "C" int melee_nx_perf_int(const char* key, int fallback) {
    const CfgEntry* entry = cfg_find(key);
    return entry ? static_cast<int>(entry->value) : fallback;
}

// An optimization switch: `opt_safe 1` in perf.cfg turns every opt off except those the file sets
// explicitly. Each key is logged once, with where its value came from, so a capture says what ran.
extern "C" int melee_nx_opt(const char* key, int fallback) {
    const CfgEntry* entry = cfg_find(key);
    const char* source = "default";
    int value = fallback;
    if (entry) {
        value = static_cast<int>(entry->value);
        source = "perf.cfg";
    } else if (melee_nx_perf_int("opt_safe", 0) != 0) {
        value = 0;
        source = "opt_safe";
    }
    std::lock_guard lock(opt_log_mutex);
    for (int i = 0; i < opt_logged_count; ++i)
        if (std::strcmp(opt_logged[i], key) == 0)
            return value;
    if (opt_logged_count < MaxCfgEntries) {
        std::snprintf(opt_logged[opt_logged_count++], sizeof(opt_logged[0]), "%s", key);
        pc_log_line("[opt] %s=%d (%s)", key, value, source);
    }
    return value;
}

namespace {
// Thread handles by role, saved when each thread registers, so the menu can move them later.
Handle g_roleHandles[5] = {}; // game, FIFO, render, shader worker (unused), audio
const char* const kAffinityNames[] = {"baseline", "main", "split", "tuned", "game"};

// Puts one registered thread where the current policy wants it. Called from the thread itself at start and
// from the menu (any thread) when the policy changes, so the handle is never the CUR_THREAD pseudo-handle.
// `live` is true when called from the menu: "baseline" then has to actively restore the default mask
// instead of leaving the thread alone. Every call logs a THREAD line with the result.
void place_thread(int role, Handle handle, const char* name, bool live) {
    u64 allowed = 0, actual = 0;
    s32 preferred = -1;
    const Result info_rc = svcGetInfo(&allowed, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
    int cores[32], count = 0;
    for (int i = 0; i < 32; ++i)
        if (allowed & (1ull << i))
            cores[count++] = i;
    int target = -1;
    u64 mask = 0;
    // lo/mid/hi are the lowest, second and highest allowed cores (0, 1, 2 for a normal application).
    if (R_SUCCEEDED(info_rc) && count >= 3) {
        const int lo = cores[0], mid = cores[1], hi = cores[count - 1];
        switch (affinity) {
        case Affinity::Baseline: // untouched at start (any core, ideal -1, as logged); live: back to that
            target = live ? -2 : -1;
            mask = allowed;
            break;
        case Affinity::Main: // the game thread on the lowest core, the rest anywhere
            target = role == 0 ? lo : -2;
            mask = role == 0 ? 1ull << lo : allowed;
            break;
        case Affinity::Split: // one core each: game, FIFO, render (audio with render)
            target = role == 0 ? lo : role == 1 ? mid : hi;
            mask = 1ull << target;
            break;
        case Affinity::Game: // the game thread alone on the middle core, everything else on the lowest
            target = role == 0 ? mid : lo;
            mask = 1ull << target;
            break;
        case Affinity::Tuned: // KartPadNX: the game thread owns the middle core, the rest share the others
            if (role == 0) {
                target = mid;
                mask = 1ull << mid;
            } else {
                target = role == 1 ? lo : hi;
                mask = allowed & ~(1ull << mid);
            }
            break;
        }
    }
    // target -1: leave the thread alone; -2: ideal core -1 (no preference, as threads start) with the mask.
    const Result set_rc = target == -1 ? 0
                          : target == -2 ? svcSetThreadCoreMask(handle, -1, mask)
                                         : svcSetThreadCoreMask(handle, target, mask);
    const Result mask_rc = svcGetThreadCoreMask(&preferred, &actual, handle);
    s32 priority = 0;
    const Result priority_rc = svcGetThreadPriority(&priority, handle);
    pc_log_line(
        "THREAD %s policy %s allowed 0x%llx requested %d actual 0x%llx preferred %d priority %d rc %x/%x/%x/%x",
        name, affinity_name, (unsigned long long)allowed, target, (unsigned long long)actual,
        preferred, priority, info_rc, set_rc, mask_rc, priority_rc);
}
const char* const kRoleNames[] = {"Main thread", "Aurora FIFO processor", "Aurora render worker", "",
                                  "SFA audio"};
} // namespace

// Maps a thread name to its role, saves its handle, sets the audio thread's priority and places it.
extern "C" void melee_nx_perf_thread(const char* name) {
    int role;
    if (std::strcmp(name, "Main thread") == 0)
        role = 0;
    else if (std::strcmp(name, "Aurora FIFO processor") == 0)
        role = 1;
    else if (std::strcmp(name, "Aurora render worker") == 0)
        role = 2;
    else if (std::strcmp(name, "SFA audio") == 0)
        role = 4;
    else
        return;
    const Handle handle = threadGetCurHandle();
    g_roleHandles[role] = handle;
    if (role == 4)
        svcSetThreadPriority(handle, 0x2B); // above the game thread (0x2C): mixing is short and must be on time
    place_thread(role, handle, name, false);
    melee_nx_prof_register(role);
}

// For Aurora's pipeline worker and pipeline cache writer: lower priority than the game (0x2E)
// and kept off the game thread's core unless the policy is baseline.
extern "C" void melee_nx_perf_background_thread(const char* name) {
    const Handle handle = threadGetCurHandle();
    const Result priority_rc = svcSetThreadPriority(handle, 0x2E);
    u64 allowed = 0;
    Result mask_rc = svcGetInfo(&allowed, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
    int cores[32], count = 0;
    for (int i = 0; i < 32; ++i)
        if (allowed & (1ull << i))
            cores[count++] = i;
    if (R_SUCCEEDED(mask_rc) && count >= 3 && affinity != Affinity::Baseline)
        mask_rc = svcSetThreadCoreMask(handle, cores[count - 1], allowed & ~(1ull << cores[1]));
    s32 preferred = -1, priority = 0;
    u64 actual = 0;
    svcGetThreadCoreMask(&preferred, &actual, handle);
    svcGetThreadPriority(&priority, handle);
    pc_log_line("THREAD %s background: actual 0x%llx preferred %d priority %d rc %x/%x", name,
                (unsigned long long)actual, preferred, priority, priority_rc, mask_rc);
}

// Current policy as a number (the order of the Affinity enum) and its name, for the menu.
extern "C" int melee_nx_perf_affinity(void) { return static_cast<int>(affinity); }

extern "C" const char* melee_nx_perf_affinity_name(int policy) {
    return policy >= 0 && policy < 5 ? kAffinityNames[policy] : "?";
}

// From the - menu: switch the policy now and move every thread already running.
extern "C" void melee_nx_perf_set_affinity(int policy) {
    if (policy < 0 || policy >= 5 || policy == static_cast<int>(affinity))
        return;
    affinity = static_cast<Affinity>(policy);
    affinity_name = kAffinityNames[policy];
    for (int role = 0; role < 5; ++role)
        if (g_roleHandles[role] != 0)
            place_thread(role, g_roleHandles[role], kRoleNames[role], true);
}

extern "C" uint64_t melee_nx_perf_now(void) { return armTicksToNs(armGetSystemTick()); }

// Adds one timed stage to the current window.
extern "C" void melee_nx_perf_stage(MeleePerfStage stage, uint64_t elapsed_ns) {
    stages[stage] += elapsed_ns;
    ++calls[stage];
}

extern "C" void melee_nx_perf_frame(uint64_t elapsed_ns) {
    ++frames;
    window_ns += elapsed_ns;
    if (elapsed_ns > worst_ns)
        worst_ns = elapsed_ns;
    if (elapsed_ns > 1000000000ull / 60)
        ++late;
    const size_t bin = elapsed_ns / BinNs;
    if (bin < bins.size())
        ++bins[bin];
    else
        ++overflow;
    if (window_ns < 2000000000ull)
        return;

    pc_log_line(
        "FRAMES n %u span_ms %.1f fps %.2f p50_le_ms %.2f p95_le_ms %.2f p99_le_ms %.2f worst_ms %.2f late60 %u overflow %u",
        frames, window_ns / 1e6, frames * 1e9 / window_ns, percentile(50), percentile(95),
        percentile(99), worst_ns / 1e6, late, overflow);
    pc_log_line(
        "STAGES fifo_ms %.3f texture_ms %.3f finish_ms %.3f ui_ms %.3f dispatch_ms %.3f alarms_ms %.3f retrace_ms %.3f",
        average(MELEE_PERF_FIFO), average(MELEE_PERF_TEXTURE), average(MELEE_PERF_FINISH),
        average(MELEE_PERF_UI), average(MELEE_PERF_DISPATCH), average(MELEE_PERF_ALARMS),
        average(MELEE_PERF_RETRACE));
    bins.fill(0);
    stages.fill(0);
    calls.fill(0);
    frames = overflow = late = 0;
    window_ns = worst_ns = 0;
}
