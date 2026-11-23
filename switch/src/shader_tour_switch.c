// Shader warm-up tour (the - menu's "Shader tour"). Warps the player through every destination of the
// game's warp table (WARPTAB.bin, the same records warpToMap reads), and at each one turns the player a
// quarter at a time while the renderer compiles what the map draws. It moves on once the shader compiler
// and Aurora's pipeline queue have been idle for a while (or a time limit passes). Everything compiled
// lands in the normal caches (cache/deko/dksh_local.bin, cache/pipeline_cache_deko.db), which
// switch/builder/seed-caches.py merges into the seed (switch/seed/, packed into sfa-shaderdb.bin).
//
// Routes (picked on the menu line with Left/Right): route 1 stops where each warp lands. Routes 2 and 3
// stop somewhere else in the same map, so a second and third tour compile what route 1 never saw. After
// the warp, route 2 teleports the player to the loaded object farthest from the arrival point; route 3
// goes there too and then on to the object farthest from both. On routes 2 and 3 a map with no object
// far enough away is skipped, since route 1 already covered it.
//
// Warp loops: some destinations land the player on a warp pad that sends them on at once. Warps 93 and 94
// are the two ends of one pair: on hardware 2026-11-20 the game reloaded between them every few frames
// and the tour never moved on. A stop only counts as arrived once the player has stayed loaded for
// kSettleFrames in a row. A player that vanishes kMaxDrops times means a loop, and that warp is skipped
// now and in every later tour (listed in kSkipPath).
//
// Capture mode (sfaShaderTourStartHere, switch_menu.ini shader_capture= for the Ryujinx save tour,
// switch/tools/ryujinx-save-tour.py): no warps. It dwells where the loaded save put the player, then hops
// to up to N far objects in the same map and dwells at each, then reports done so the caller can quit.
//
// Runs on the game thread from VIWaitForRetrace (switch_menu.cpp). One "[tour]" line per stop in
// stdout.log. Built with the game's include dirs (CMakeLists.txt).
#include <stdio.h>
#include <string.h>

#include "main/asset_load.h"
#include "main/mldf_fileid.h"
#include "main/objanim_internal.h"
#include "main/dll/player.h"
#include "main/dll/player_state.h"
#include "main/obj_list.h"
#include "sys/objects.h"

// Game functions and flags (decomp names): start a warp, warp in progress, map unload in progress, and
// which asset files are loaded (logged at the start).
extern void warpToMap(int idx, s8 transType);
extern u8 gWarpRequested;
extern u8 gGameLoopFullMapUnloadPending;
extern int getLoadedFileFlags(int slot);

// Renderer activity: each returns how much work is queued (non-zero = busy) and sets a running total of
// finished work, so a stop can tell when nothing new has been compiled for a while.
extern int swdk_shader_activity(unsigned* compiled);           // switch/deko/shaders.cpp
extern unsigned aurora_pipeline_activity(unsigned* created);   // extern/aurora/lib/gfx/pipeline_cache.cpp

// WARPTAB.bin holds 128 records of 16 bytes; unused ones are all zero.
enum { kWarpCount = 128 };
// Generous on purpose: the map streams in and the compiler catches up for several seconds after a warp
// (hardware 2026-11-19: stops of a few seconds moved on before the area had drawn).
enum { kSettleFrames = 3 * 60, kMinFrames = 16 * 60, kQuietFrames = 5 * 60, kMaxFrames = 120 * 60, kLoadFrames = 90 * 60 };
// Per stop: wait kSettleFrames after the player appears, stay at least kMinFrames, move on after
// kQuietFrames without compiles or at kMaxFrames; give up on a warp that never loads after kLoadFrames.
// The player turns 90 degrees every kTurnFrames, four times.
enum { kTurnFrames = 3 * 60 };
// Routes 2 and 3: a hop target must be at least kMinHop units (horizontally) from every spot already
// visited in this map. Objects more than kMaxHop from the player are ignored (bad or out-of-map
// positions). The player is placed kHopLift above the object so it does not start inside the floor.
#define kMinHop 400.0f
#define kMaxHop 30000.0f
#define kHopLift 10.0f
enum { kRouteCount = 3 };
// Times the player may vanish (reload) at one stop before the warp counts as a warp loop.
enum { kMaxDrops = 3 };

// kLoading: warp requested, waiting for the map and player. kHop (routes 2 and 3): teleported within the
// map, waiting for that area to stream in. kDwell: at the stop, turning and waiting for the compiler to
// go quiet.
typedef enum { kIdle, kLoading, kHop, kDwell } TourState;

static TourState sState = kIdle;
static int sNext;      // next warp index to try
static int sCurrent = -1;
static int sStops, sTotal;
static int sFrames, sQuiet, sTurns;
static int sRoute = 1;  // 1..kRouteCount
static int sHopsLeft;   // hops still to make at this stop
// Frames the player has been loaded without a break, and how often it vanished, at this stop.
static int sPresent, sDrops;
// Spots visited in this map (the arrival, then each hop) as x and z.
static float sSpots[kRouteCount][2];
static int sSpotCount;
// Compile counters: last frame's totals, the totals when the stop began, and the sums for the whole tour.
static unsigned sLastCompiled, sLastCreated, sStartCompiled, sStartCreated, sTourCompiled, sTourCreated;
// Capture mode: active, hops still allowed after the current dwell, and set once it has finished.
static int sHere, sHereHops, sHereDone;

// Where the next tour starts, as "<warp> <route>": the warp after the last finished stop, saved after
// every stop, so a tour that ends early (crash, power) resumes there from the - menu. The file is
// deleted only when it would say warp 0 on route 1 (the defaults).
static const char* const kResumePath = "sdmc:/switch/SFA-NX/config/shader_tour_next";
static int sResume = -1;  // -1: not read yet

// Reads the resume file once. Older builds wrote only the warp, which means route 1.
static void read_resume(void) {
    if (sResume >= 0) {
        return;
    }
    sResume = 0;
    FILE* f = fopen(kResumePath, "r");
    if (f != NULL) {
        int v = 0, r = 1;
        const int n = fscanf(f, "%d %d", &v, &r);
        if (n >= 1 && v > 0 && v < kWarpCount) {
            sResume = v;
        }
        if (n == 2 && r >= 1 && r <= kRouteCount) {
            sRoute = r;
        }
        fclose(f);
    }
}

// Warps found to loop, one index per line; every tour skips them. Delete the file to retry them.
static const char* const kSkipPath = "sdmc:/switch/SFA-NX/config/shader_tour_skip";
static u8 sSkip[kWarpCount];
static int sSkipRead;

// Reads the skip list once.
static void read_skip(void) {
    if (sSkipRead) {
        return;
    }
    sSkipRead = 1;
    FILE* f = fopen(kSkipPath, "r");
    if (f != NULL) {
        int v;
        while (fscanf(f, "%d", &v) == 1) {
            if (v >= 0 && v < kWarpCount) {
                sSkip[v] = 1;
            }
        }
        fclose(f);
    }
}

// Adds a warp to the skip list, in memory and in the file.
static void add_skip(int idx) {
    if (idx < 0 || idx >= kWarpCount || sSkip[idx]) {
        return;
    }
    sSkip[idx] = 1;
    FILE* f = fopen(kSkipPath, "a");
    if (f != NULL) {
        fprintf(f, "%d\n", idx);
        fclose(f);
    }
}

// Writes the resume point and route, or deletes the file when both are the defaults.
static void write_resume(void) {
    if (sResume == 0 && sRoute == 1) {
        remove(kResumePath);
        return;
    }
    FILE* f = fopen(kResumePath, "w");
    if (f != NULL) {
        fprintf(f, "%d %d\n", sResume, sRoute);
        fclose(f);
    }
}

// Warp the next tour starts from (0 = the beginning).
int sfaShaderTourResumeWarp(void) {
    read_resume();
    return sResume;
}

// Saves (or with 0, clears) the resume point.
void sfaShaderTourSetResumeWarp(int warp) {
    read_resume();
    sResume = warp > 0 && warp < kWarpCount ? warp : 0;
    write_resume();
}

// The selected route (1..3).
int sfaShaderTourRoute(void) {
    read_resume();
    return sRoute;
}

// Selects a route (wrapped into 1..3). A newly picked route starts from the first warp.
void sfaShaderTourSetRoute(int route) {
    read_resume();
    sRoute = ((route - 1) % kRouteCount + kRouteCount) % kRouteCount + 1;
    sResume = 0;
    write_resume();
}

// A warp record is used when its first 12 bytes (position and map) are not all zero and it is not on
// the skip list.
static int warp_valid(int idx) {
    read_skip();
    if (sSkip[idx]) {
        return 0;
    }
    u8 rec[16];
    getTabEntry(rec, MLDF_FILEID_WARPTAB_BIN, idx << 4, 16);
    static const u8 zero[12];
    return memcmp(rec, zero, 12) != 0;
}

// Reads both renderer counters; busy when shaders or pipelines are still queued.
static void counters(unsigned* compiled, unsigned* created, int* busy) {
    const int shaders = swdk_shader_activity(compiled);
    const unsigned pipes = aurora_pipeline_activity(created);
    *busy = shaders > 0 || pipes > 0;
}

// Squared horizontal distance from (x, z) to the nearest spot visited in this map.
static float nearest_spot_sq(float x, float z) {
    float best = -1.0f;
    for (int i = 0; i < sSpotCount; ++i) {
        const float dx = x - sSpots[i][0], dz = z - sSpots[i][1];
        const float d = dx * dx + dz * dz;
        if (best < 0.0f || d < best) {
            best = d;
        }
    }
    return best;
}

// Teleports the player to the loaded object farthest from every spot visited so far. The object must
// have no parent (not carried or attached) and lie between kMinHop and kMaxHop away. Returns 0 when no
// object qualifies.
static int hop(GameObject* player) {
    int start = 0, count = 0;
    GameObject** objs = ObjList_GetObjects(&start, &count);
    GameObject* best = NULL;
    float bestSq = kMinHop * kMinHop;
    for (int i = start; objs != NULL && i < count; ++i) {
        GameObject* o = objs[i];
        if (o == NULL || o == player || o->anim.parent != NULL) {
            continue;
        }
        const float x = o->anim.worldPosX, z = o->anim.worldPosZ;
        const float px = x - player->anim.worldPosX, pz = z - player->anim.worldPosZ;
        if (px * px + pz * pz > kMaxHop * kMaxHop) {
            continue;
        }
        const float d = nearest_spot_sq(x, z);
        if (d > bestSq) {
            bestSq = d;
            best = o;
        }
    }
    if (best == NULL) {
        return 0;
    }
    Vec3f pos;
    pos.x = best->anim.worldPosX;
    pos.y = best->anim.worldPosY + kHopLift;
    pos.z = best->anim.worldPosZ;
    playerTeleport(player, &pos, NULL, 0);
    if (sSpotCount < kRouteCount) {
        sSpots[sSpotCount][0] = pos.x;
        sSpots[sSpotCount][1] = pos.z;
        ++sSpotCount;
    }
    printf("[tour] warp %d: hop to (%.0f, %.0f, %.0f), %.0f units from the nearest earlier spot\n", sCurrent,
           pos.x, pos.y, pos.z, __builtin_sqrtf(bestSq));
    return 1;
}

// Starts the dwell (turning, waiting for the compiler) at the player's current spot.
static void begin_dwell(void) {
    int busy;
    counters(&sStartCompiled, &sStartCreated, &busy);
    sLastCompiled = sStartCompiled;
    sLastCreated = sStartCreated;
    sState = kDwell;
    sFrames = sQuiet = sTurns = 0;
}

// Next used warp index at or after sNext, or -1 when the table is done.
static int next_stop(void) {
    while (sNext < kWarpCount) {
        const int idx = sNext++;
        if (warp_valid(idx)) {
            return idx;
        }
    }
    return -1;
}

static void next(void);

// Warps to the next stop, or ends the tour (and clears the resume point) when none are left.
static void go(void) {
    sCurrent = next_stop();
    if (sCurrent < 0) {
        printf("[tour] route %d done: %d stops, %u shaders and %u pipelines compiled\n", sRoute, sStops,
               sTourCompiled, sTourCreated);
        sfaShaderTourSetResumeWarp(0);
        sState = kIdle;
        return;
    }
    warpToMap(sCurrent, 0);
    sState = kLoading;
    sFrames = sPresent = sDrops = 0;
}

// Tracks whether the player stays loaded and returns 1 when the player is loaded this frame. A player
// that vanishes kMaxDrops times means a warp loop: the warp goes on the skip list, the tour moves on,
// and *skipped is set.
static int track_player(GameObject* player, int* skipped) {
    *skipped = 0;
    if (player == NULL || gWarpRequested != 0 || gGameLoopFullMapUnloadPending != 0) {
        if (sPresent > 0 && ++sDrops >= kMaxDrops) {
            printf("[tour] warp %d: the player keeps getting warped away (warp loop), skipped from now on\n",
                   sCurrent);
            add_skip(sCurrent);
            next();
            *skipped = 1;
        }
        sPresent = 0;
        return 0;
    }
    ++sPresent;
    return 1;
}

// Counts a stop as finished, saves the resume point and warps on. In capture mode there is no next
// warp: the capture ends instead (a vanished player or a failed hop also lands here).
static void next(void) {
    if (sHere) {
        printf("[capture] done: %u shaders and %u pipelines compiled\n", sTourCompiled, sTourCreated);
        sHere = 0;
        sHereDone = 1;
        sState = kIdle;
        return;
    }
    ++sStops;
    sfaShaderTourSetResumeWarp(sNext);
    go();
}

// A save is loaded and no warp is running: the tour can start.
int sfaShaderTourHasPlayer(void) { return Obj_GetPlayerObject() != NULL && gWarpRequested == 0; }

// 1 started (or already running), 0 refused: no player yet (title screen, file select).
int sfaShaderTourStart(int first) {
    if (sState != kIdle) {
        return 1;
    }
    if (!sfaShaderTourHasPlayer()) {
        printf("[tour] no player: load a save first\n");
        return 0;
    }
    read_resume();
    sTotal = 0;
    for (int i = 0; i < kWarpCount; ++i) {
        sTotal += warp_valid(i);
    }
    sNext = first > 0 && first < kWarpCount ? first : 0;
    sStops = 0;
    for (int i = 0; i < sNext; ++i) {
        sStops += warp_valid(i);
    }
    sTourCompiled = sTourCreated = 0;
    printf("[tour] route %d, start at warp %d: %d destinations, load flags 0x%x\n", sRoute, sNext, sTotal,
           getLoadedFileFlags(0));
    go();
    return 1;
}

// Capture mode: dwell at the player's spot, then hop to up to `hops` far objects in this map and dwell
// at each. 1 started, 0 refused (no player). The warp table, resume point and skip list are not touched.
int sfaShaderTourStartHere(int hops) {
    if (sState != kIdle) {
        return 1;
    }
    GameObject* player = Obj_GetPlayerObject();
    if (player == NULL || gWarpRequested != 0) {
        return 0;
    }
    sHere = 1;
    sHereDone = 0;
    sHereHops = hops < 0 ? 0 : hops > kRouteCount - 1 ? kRouteCount - 1 : hops;
    sCurrent = -1;
    sTourCompiled = sTourCreated = 0;
    sSpots[0][0] = player->anim.worldPosX;
    sSpots[0][1] = player->anim.worldPosZ;
    sSpotCount = 1;
    sPresent = kSettleFrames;  // the caller already waited for the player to settle
    sDrops = 0;
    printf("[capture] start at (%.0f, %.0f, %.0f), up to %d hops\n", player->anim.worldPosX,
           player->anim.worldPosY, player->anim.worldPosZ, sHereHops);
    begin_dwell();
    return 1;
}

// 1 once a capture has finished (its caller quits the app).
int sfaShaderTourHereDone(void) { return sHereDone; }

// Stops after the current stop (the resume point keeps the next one).
void sfaShaderTourStop(void) {
    if (sState != kIdle) {
        printf("[tour] stopped at warp %d\n", sCurrent);
        sState = kIdle;
    }
}

int sfaShaderTourActive(void) { return sState != kIdle; }

// "Route 2: 12/76 (warp 14)" for the menu.
void sfaShaderTourStatus(char* out, int size) {
    if (sState == kIdle) {
        snprintf(out, size, "%s", "");
    } else {
        snprintf(out, size, "Route %d: %d/%d (warp %d)", sRoute, sStops + 1, sTotal, sCurrent);
    }
}

// Called every frame from the menu; advances the state machine described at the top of the file.
void sfaShaderTourFrame(void) {
    if (sState == kIdle) {
        return;
    }
    ++sFrames;
    GameObject* player = Obj_GetPlayerObject();
    if (sState == kLoading) {
        int skipped;
        if (!track_player(player, &skipped)) {
            if (!skipped && sFrames > kLoadFrames) {
                printf("[tour] warp %d: no player after %d s, skipped\n", sCurrent, kLoadFrames / 60);
                next();
            }
            return;
        }
        // Arrived only once the player has stayed loaded for kSettleFrames in a row.
        if (sPresent < kSettleFrames) {
            return;
        }
        // Arrived. Route 1 dwells here; routes 2 and 3 remember the arrival point and hop on.
        sSpots[0][0] = player->anim.worldPosX;
        sSpots[0][1] = player->anim.worldPosZ;
        sSpotCount = 1;
        sHopsLeft = sRoute - 1;
        sQuiet = 0;
        sDrops = 0;  // reloads after a hop are counted apart from the arrival's
        sState = kHop;
        sFrames = kSettleFrames;  // already settled: no second wait at the arrival point
    }
    if (sState == kHop) {
        // Wait for the hop's area to stream in. A fall into the void can reload the player: wait again.
        // sQuiet (unused until the dwell) counts all frames spent here, so a reload that never ends is
        // skipped like a warp that never loads.
        if (++sQuiet > kLoadFrames) {
            printf("[tour] warp %d: no player after a hop, skipped\n", sCurrent);
            next();
            return;
        }
        int skipped;
        if (!track_player(player, &skipped)) {
            sFrames = 0;  // wait the full settle time again once the player is back
            return;
        }
        if (sFrames < kSettleFrames) {
            return;
        }
        if (sHopsLeft > 0) {
            --sHopsLeft;
            if (hop(player)) {
                sFrames = 0;
                return;
            }
            if (sSpotCount == 1) {
                // No far spot at all: route 1 already covered this map.
                printf("[tour] warp %d: no spot %.0f+ units from the arrival, skipped\n", sCurrent, kMinHop);
                next();
                return;
            }
            sHopsLeft = 0;  // the first hop worked but there is no second spot: dwell where we are
        }
        begin_dwell();
        return;
    }
    // kDwell: turn a quarter every kTurnFrames so the camera sees all around; wait for the compiler.
    if (player != NULL && sFrames % kTurnFrames == 0 && sTurns < 4) {
        // The player code copies PlayerState.targetYaw into anim.rotX every frame, so turn that too.
        const s16 yaw = (s16)(player->anim.rotX + 0x4000);
        PlayerState* ps = (PlayerState*)player->extra;
        if (ps != NULL) {
            ps->targetYaw = yaw;
            ps->yaw = yaw;
        }
        player->anim.rotX = yaw;
        ++sTurns;
    }
    unsigned compiled, created;
    int busy;
    counters(&compiled, &created, &busy);
    if (busy || compiled != sLastCompiled || created != sLastCreated) {
        sQuiet = 0;
    } else {
        ++sQuiet;
    }
    sLastCompiled = compiled;
    sLastCreated = created;
    const int done = sFrames >= kMinFrames && sTurns >= 4 && sQuiet >= kQuietFrames;
    if (done || sFrames >= kMaxFrames) {
        const unsigned dc = compiled - sStartCompiled, dp = created - sStartCreated;
        sTourCompiled += dc;
        sTourCreated += dp;
        if (sHere) {
            printf("[capture] spot %d: %u shaders, %u pipelines, %.1f s%s\n", sSpotCount, dc, dp, sFrames / 60.0,
                   done ? "" : " (time limit)");
            // Hop on while hops are left and a far enough object exists; the hop's area streams in during
            // kHop (sHopsLeft 0: straight to the dwell once settled).
            if (sHereHops > 0 && player != NULL) {
                --sHereHops;
                if (hop(player)) {
                    sState = kHop;
                    sHopsLeft = 0;
                    sFrames = sQuiet = 0;
                    return;
                }
            }
            next();
            return;
        }
        printf("[tour] route %d, %d/%d warp %d: %u shaders, %u pipelines, %.1f s%s\n", sRoute, sStops + 1,
               sTotal, sCurrent, dc, dp, sFrames / 60.0, done ? "" : " (time limit)");
        next();
    }
}
