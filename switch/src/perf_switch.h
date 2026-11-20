/* Performance settings and telemetry for the Switch layer (perf_switch.cpp, prof_switch.cpp).
   Tuning knobs come from sdmc:/switch/SFA-NX/config/perf.cfg, one "key value" per line; the file is
   optional and every knob has a default. The melee_nx_ names come from Melee-NX, where this layer started. */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Loaded once before Aurora starts its workers. Missing file selects baseline. */
void melee_nx_perf_init(void);
/* Called by a named thread when it starts (Aurora's thread naming hook). Known threads get their core
   placement and priority here and are registered with the profiler; other names are ignored. */
void melee_nx_perf_thread(const char* name);
/* Thread placement policy: 0 baseline, 1 main, 2 split, 3 tuned, 4 game. Setting it moves every
   registered thread (game, FIFO, render, audio) at once. */
int melee_nx_perf_affinity(void);
const char* melee_nx_perf_affinity_name(int policy);
void melee_nx_perf_set_affinity(int policy);
/* A numeric perf.cfg value (decimal or 0x hex), or `fallback` when the key is absent. */
int melee_nx_perf_int(const char* key, int fallback);
/* Like melee_nx_perf_int for an optimization switch: `opt_safe 1` turns it off unless the file sets
   it, and the first call per key logs "[opt] key=value (source)". */
int melee_nx_opt(const char* key, int fallback);
/* Sampling profiler (prof_switch.cpp, perf.cfg prof_seconds). Roles: 0 game, 1 FIFO, 2 render,
   3 shader compile. Register from the thread itself once it is placed. */
void melee_nx_prof_init(void);
void melee_nx_prof_register(int role);
/* Monotonic time in nanoseconds from the ARM system counter. */
uint64_t melee_nx_perf_now(void);
/* Parts of a game frame timed separately; averaged per 2 s window in the STAGES log line. */
enum MeleePerfStage {
    MELEE_PERF_FIFO,
    MELEE_PERF_TEXTURE,
    MELEE_PERF_FINISH,
    MELEE_PERF_UI,
    MELEE_PERF_DISPATCH,
    MELEE_PERF_ALARMS,
    MELEE_PERF_RETRACE,
    MELEE_PERF_STAGE_COUNT
};
/* All timing calls below belong to the game thread, never a render callback. */
void melee_nx_perf_stage(enum MeleePerfStage stage, uint64_t elapsed_ns);
/* Records one frame's time. Every 2 s logs a FRAMES line (fps, percentiles, worst, frames over 16.7 ms)
   and a STAGES line, then starts a new window. */
void melee_nx_perf_frame(uint64_t elapsed_ns);

#ifdef __cplusplus
}
#endif
