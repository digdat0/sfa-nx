// SFA-NX's - (minus) menu, drawn with Aurora's Dear ImGui. Two tabs, switched with L / R:
//   Game:  Show FPS, Invert look, HD textures, Button mapping, Updates.
//   Debug: (only with debug_menu=1 in switch_menu.ini, off by default) the profiler, thread placement (perf_switch.cpp policies, switched live), the audio thread
//          (port/src/ai_shim.c), frame overlap / frame step / pacing, the renderer, GPU clock and GPU
//          options, the shader tour and clearing the shader cache.
// Some settings apply at once; compression, Z-cull, BC1 textures and HD textures are read by the renderer
// at startup and show "(restart)" until then.
// Drawn at the end of every game frame (port/src/vi_shim.c, VIWaitForRetrace). While it is open the game sees
// no pad input (switch/src/input_trace_switch.c's PADRead wrapper calls sfaSwitchMenuFilterPad). Settings are
// kept in sdmc:/switch/SFA-NX/config/switch_menu.ini (the profiler always starts off).
//
// Button mapping: one Switch control per GameCube button (port 1). The defaults are whatever Aurora gives
// the controller (read when it connects); changes are written to Aurora's mapping table, and the ini only
// holds a "map=" line once something was changed. GameCube L/R keep their analog value when bound to ZL/ZR.
// SDL reports ZL/ZR as axes, which Aurora's button table cannot hold, so digital buttons bound to ZL/ZR are
// set in sfaSwitchMenuFilterPad. The page also lights each GameCube button and shows the sticks, triggers and
// raw Switch buttons live, to check every input.
#include <aurora/imgui.h>
#include <dolphin/pad.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <switch.h>

extern "C" void sfaProfSetEnabled(int on);     // prof_switch.cpp
extern "C" int sfaProfEnabled(void);
extern "C" void sfaProfSections(double* avgMs, double* maxMs, unsigned long long* samples);
extern "C" int melee_nx_perf_affinity(void);                 // perf_switch.cpp
extern "C" const char* melee_nx_perf_affinity_name(int policy);
extern "C" void melee_nx_perf_set_affinity(int policy);
extern "C" int fhAIThreaded(void);                            // port/src/ai_shim.c
extern "C" void fhAISetThreaded(int on);
extern "C" void aurora_set_frame_overlap(bool value);          // extern/aurora/lib/aurora.cpp
extern "C" bool aurora_frame_overlap(void);
extern "C" void aurora_set_direct_renderer(bool on);          // switch/deko/direct.cpp
extern "C" bool aurora_direct_renderer(void);
extern "C" int sfaShaderTourStart(int first);                 // switch/src/shader_tour_switch.c
extern "C" void sfaShaderTourStop(void);
extern "C" int sfaShaderTourActive(void);
extern "C" void sfaShaderTourStatus(char* out, int size);
extern "C" void sfaShaderTourFrame(void);
extern "C" int sfaShaderTourHasPlayer(void);
extern "C" int sfaShaderTourResumeWarp(void);
extern "C" void sfaShaderTourSetResumeWarp(int warp);
extern "C" int sfaShaderTourRoute(void);
extern "C" void sfaShaderTourSetRoute(int route);
extern "C" int sfaDiscIsPal(void);                            // switch/src/main_sfa.cpp

namespace {

constexpr const char* kSettingsPath = "sdmc:/switch/SFA-NX/config/switch_menu.ini";

// Current settings. The sBoot* copies hold the values the running session started with, so the menu can
// show "(restart)" for settings that only apply at the next start.
bool sShowFps = false;
bool sInvertLook = false;
bool sLoaded;
bool sGpuBoost = false;
int sCompression = 0;
bool sZcull = false;
bool sTexBc1 = false;
bool sHdTextures = true;
bool sBootHdTextures = true;
// Game text language as the game numbers it (0 English, 1 French, 2 German, 3 Italian, 5 Spanish; 4 is
// Japanese, which only the Japanese disc uses). main_sfa.cpp passes it to the game at start as
// FOXHOLLOW_LANGUAGE, only for the PAL disc, the one that has the other languages' text.
int sLanguage = 0;
int sBootLanguage = 0;
constexpr int kLanguages[] = {0, 1, 2, 3, 5};
constexpr const char* kLanguageNames[] = {"English", "French", "German", "Italian", "", "Spanish"};
int sHdTextureMb = 512;
int sBootCompression = 0;
bool sBootZcull = false;
bool sBootTexBc1 = false;

// Frame step: advance game time by whole 60 Hz frames (on) or by the measured frame time (off).
// Pacing: wait for the display's vsync (on, vsync_switch.cpp) or a CPU timer (off).
bool sWholeSteps = true;
bool sVsyncPace = true;

// Menu state: open, current tab and row, and last frame's button states (to detect new presses).
bool sOpen;
int sTab;
// The Debug tab is for testing builds: hidden unless switch_menu.ini has debug_menu=1 (never on by default,
// so released builds show only the Game tab). Its settings in the ini still apply when it is hidden.
bool sShowDebug;
int sSelected;
bool sPrevDown[SDL_GAMEPAD_BUTTON_COUNT];

// FPS counter: frames counted over half-second windows.
Uint64 sFpsStartNs;
int sFpsFrames;
float sFps;

// Every menu row; the tabs below list which rows each tab shows, in order.
enum Item {
    kItemFps,
    kItemInvertLook,
    kItemProfiler,
    kItemThreads,
    kItemAudio,
    kItemOverlap,
    kItemFrameStep,
    kItemPacing,
    kItemRenderer,
    kItemGpuClock,
    kItemCompression,
    kItemZcull,
    kItemTexBc1,
    kItemHdTextures,
    kItemLanguage,
    kItemMapping,
    kItemTour,
    kItemShaderCache,
    kItemUpdate,
    kItemClose,
    kItemCount
};

constexpr int kGameItems[] = {kItemFps,     kItemInvertLook, kItemHdTextures, kItemLanguage,
                              kItemMapping, kItemUpdate,     kItemClose};
constexpr int kDebugItems[] = {kItemProfiler,    kItemThreads, kItemAudio, kItemOverlap,     kItemFrameStep,
                               kItemPacing,      kItemRenderer, kItemGpuClock, kItemCompression, kItemZcull,
                               kItemTexBc1,      kItemTour,    kItemShaderCache, kItemClose};
constexpr const char* kTabNames[2] = {"Game", "Debug"};

extern "C" void sfaUpdaterStatus(char* out, size_t size);
extern "C" void sfaUpdaterActivate(void);

// The rows of one tab and how many there are.
const int* tab_items(int tab, int* count) {
    *count = tab == 0 ? static_cast<int>(sizeof(kGameItems) / sizeof(kGameItems[0]))
                      : static_cast<int>(sizeof(kDebugItems) / sizeof(kDebugItems[0]));
    return tab == 0 ? kGameItems : kDebugItems;
}

// Threads: the placement policies offered, in menu order (perf_switch.cpp numbers: 3 tuned, 4 game,
// 2 split, 0 baseline). Applied at once to the running threads; saved as threads=<number>.
constexpr int kPolicies[] = {3, 4, 2, 0};
constexpr int kPolicyCount = 4;

// Read by switch/deko/shaders.cpp at start: moves dksh_local.bin aside and compiles the shaders again.
constexpr const char* kClearShadersPath = "sdmc:/switch/SFA-NX/config/clear_shader_cache";
// True when the clear-shader-cache flag file exists.
bool shader_clear_pending() {
    FILE* f = std::fopen(kClearShadersPath, "r");
    if (f != nullptr) {
        std::fclose(f);
    }
    return f != nullptr;
}
bool sClearShaders; // the flag file exists
// Test runs (Ryujinx) only: switch_menu.ini shader_tour=<first warp> starts the tour once the player has
// existed for a few seconds. Read, never written.
int sTourAutoStart = -1;
int sTourAutoFrames;
bool sTourRefused;  // the last start had no player (shown on the menu line)

// ---- button mapping -------------------------------------------------------------------------------------

// A Switch control: an SDL gamepad button (0..), ZL/ZR (kTrigger + SDL axis), or nothing.
constexpr int kNone = -1;
constexpr int kTrigger = 100;

struct GcButton {
    const char* name;
    PADButton bit;
};
constexpr GcButton kGc[PAD_BUTTON_COUNT] = {
    {"A", PAD_BUTTON_A},          {"B", PAD_BUTTON_B},           {"X", PAD_BUTTON_X},
    {"Y", PAD_BUTTON_Y},          {"Z", PAD_TRIGGER_Z},          {"L", PAD_TRIGGER_L},
    {"R", PAD_TRIGGER_R},         {"Start", PAD_BUTTON_START},   {"D-Pad Up", PAD_BUTTON_UP},
    {"D-Pad Down", PAD_BUTTON_DOWN}, {"D-Pad Left", PAD_BUTTON_LEFT}, {"D-Pad Right", PAD_BUTTON_RIGHT},
};
enum MapRow { kRowReset = PAD_BUTTON_COUNT, kRowBack, kRowCount };

bool sMapPage;            // the menu shows the mapping page
int sMapSelected;
int sCapture = -1;        // GameCube button waiting for a Switch control, or -1
bool sCaptureArmed;       // everything was released since the capture started
int sSources[PAD_BUTTON_COUNT];
int sDefaults[PAD_BUTTON_COUNT];
bool sCustom;             // sSources differs from the defaults (saved as "map=")
bool sLoadedCustom;       // the ini had a map= line, applied when the controller appears
SDL_Gamepad* sMapPad;     // controller the mapping was applied to
PADStatus sLastStatus;    // port 1 as PADRead returned it (before the menu clears it)

// The SDL controller Aurora assigned to GameCube port 1, or null.
SDL_Gamepad* port0_gamepad() {
    const s32 index = PADGetIndexForPort(0);
    return index >= 0 ? PADGetSDLGamepadForIndex(static_cast<u32>(index)) : nullptr;
}

// GameCube L and R are also analog triggers, so they have an axis mapping as well as a button mapping.
bool is_trigger_bit(PADButton bit) { return bit == PAD_TRIGGER_L || bit == PAD_TRIGGER_R; }

// Accepts the values a saved "map=" line may contain (- is reserved for the menu itself).
bool valid_source(int s) {
    return s == kNone || (s >= 0 && s < SDL_GAMEPAD_BUTTON_COUNT && s != SDL_GAMEPAD_BUTTON_BACK) ||
           s == kTrigger + SDL_GAMEPAD_AXIS_LEFT_TRIGGER || s == kTrigger + SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
}

// Aurora's current table for port 1 as one source per GameCube button.
bool read_aurora_sources(int out[PAD_BUTTON_COUNT]) {
    u32 nb = 0, na = 0;
    PADButtonMapping* buttons = PADGetButtonMappings(0, &nb);
    PADAxisMapping* axes = PADGetAxisMappings(0, &na);
    if (buttons == nullptr || axes == nullptr) {
        return false;
    }
    for (int i = 0; i < PAD_BUTTON_COUNT; ++i) {
        out[i] = kNone;
        for (u32 j = 0; j < nb; ++j) {
            if (buttons[j].padButton == kGc[i].bit && buttons[j].nativeButton < SDL_GAMEPAD_BUTTON_COUNT) {
                out[i] = static_cast<int>(buttons[j].nativeButton);
            }
        }
        if (out[i] == kNone && is_trigger_bit(kGc[i].bit)) {
            const PADAxis axis = kGc[i].bit == PAD_TRIGGER_L ? PAD_AXIS_TRIGGER_L : PAD_AXIS_TRIGGER_R;
            for (u32 j = 0; j < na; ++j) {
                if (axes[j].padAxis != axis) {
                    continue;
                }
                if (axes[j].nativeAxis.nativeAxis >= 0) {
                    out[i] = kTrigger + axes[j].nativeAxis.nativeAxis;
                } else if (axes[j].nativeButton >= 0) {
                    out[i] = axes[j].nativeButton;
                }
            }
        }
    }
    return true;
}

// Writes sSources into Aurora's mapping table for port 1. For L/R, a ZL/ZR source becomes the analog axis
// (so the game sees how far the trigger is pressed) and a button source drives the axis fully.
void apply_sources() {
    for (int i = 0; i < PAD_BUTTON_COUNT; ++i) {
        const int s = sSources[i];
        const bool button = s >= 0 && s < kTrigger;
        PADSetButtonMapping(0, PADButtonMapping{button ? static_cast<u32>(s) : PAD_NATIVE_BUTTON_INVALID, kGc[i].bit});
        if (is_trigger_bit(kGc[i].bit)) {
            PADAxisMapping axis{};
            axis.padAxis = kGc[i].bit == PAD_TRIGGER_L ? PAD_AXIS_TRIGGER_L : PAD_AXIS_TRIGGER_R;
            axis.nativeAxis.sign = AXIS_SIGN_POSITIVE;
            axis.nativeAxis.nativeAxis = s >= kTrigger ? s - kTrigger : SDL_GAMEPAD_AXIS_INVALID;
            axis.nativeButton = button ? s : SDL_GAMEPAD_BUTTON_INVALID;
            PADSetAxisMapping(0, axis);
        }
    }
}

// Once per frame: when a (new) controller is on port 1, take its defaults and apply the saved mapping.
void sync_mapping() {
    SDL_Gamepad* pad = port0_gamepad();
    if (pad == sMapPad) {
        return;
    }
    sMapPad = pad;
    if (pad == nullptr || !read_aurora_sources(sDefaults)) {
        return;
    }
    if (sLoadedCustom) {
        apply_sources();
        sCustom = std::memcmp(sSources, sDefaults, sizeof(sSources)) != 0;
    } else {
        std::memcpy(sSources, sDefaults, sizeof(sSources));
    }
    std::printf("[sfa-nx] button mapping: %s\n", sCustom ? "custom" : "defaults");
}

// Display name of a Switch control for the mapping page.
const char* switch_name(int s) {
    static char buf[32];
    if (s == kNone) {
        return "(none)";
    }
    if (s == kTrigger + SDL_GAMEPAD_AXIS_LEFT_TRIGGER) {
        return "ZL";
    }
    if (s == kTrigger + SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
        return "ZR";
    }
    const auto b = static_cast<SDL_GamepadButton>(s);
    switch (b) {
    case SDL_GAMEPAD_BUTTON_SOUTH:
    case SDL_GAMEPAD_BUTTON_EAST:
    case SDL_GAMEPAD_BUTTON_WEST:
    case SDL_GAMEPAD_BUTTON_NORTH: {
        // the letter printed on the button (SDL's positions follow the Xbox layout)
        const SDL_GamepadButtonLabel label = sMapPad != nullptr ? SDL_GetGamepadButtonLabel(sMapPad, b)
                                                                : SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN;
        switch (label) {
        case SDL_GAMEPAD_BUTTON_LABEL_A: return "A";
        case SDL_GAMEPAD_BUTTON_LABEL_B: return "B";
        case SDL_GAMEPAD_BUTTON_LABEL_X: return "X";
        case SDL_GAMEPAD_BUTTON_LABEL_Y: return "Y";
        default: break;
        }
        static const char* const pos[4] = {"Bottom face", "Right face", "Left face", "Top face"};
        return pos[b - SDL_GAMEPAD_BUTTON_SOUTH];
    }
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return "L";
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return "R";
    case SDL_GAMEPAD_BUTTON_START: return "+";
    case SDL_GAMEPAD_BUTTON_BACK: return "-";
    case SDL_GAMEPAD_BUTTON_GUIDE: return "Home";
    case SDL_GAMEPAD_BUTTON_MISC1: return "Capture";
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return "Left stick press";
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return "Right stick press";
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return "D-Pad Up";
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return "D-Pad Down";
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return "D-Pad Left";
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return "D-Pad Right";
    default: break;
    }
    const char* sdl = SDL_GetGamepadStringForButton(b);
    std::snprintf(buf, sizeof(buf), "%s", sdl != nullptr ? sdl : "?");
    return buf;
}

// True when ZL/ZR is pressed further than `frac` of its travel.
bool trigger_down(SDL_Gamepad* pad, SDL_GamepadAxis axis, float frac) {
    return pad != nullptr && SDL_GetGamepadAxis(pad, axis) > static_cast<Sint16>(32767 * frac);
}

void save_settings();

void assign(int gc, int source) {
    // a Switch control drives one GameCube button: the one that had it takes this button's old control
    for (int j = 0; j < PAD_BUTTON_COUNT; ++j) {
        if (j != gc && sSources[j] == source && source != kNone) {
            sSources[j] = sSources[gc];
        }
    }
    sSources[gc] = source;
    sCustom = std::memcmp(sSources, sDefaults, sizeof(sSources)) != 0;
    sLoadedCustom = sCustom;
    apply_sources();
    save_settings();
    std::printf("[sfa-nx] button mapping: GC %s = %s\n", kGc[gc].name, switch_name(source));
}

// Capture mode: the first Switch control pressed after everything was let go (- cancels).
void update_capture(SDL_Gamepad* pad, const bool down[], const bool pressed[]) {
    if (pressed[SDL_GAMEPAD_BUTTON_BACK]) {
        sCapture = -1;
        return;
    }
    const bool zl = trigger_down(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 0.6f);
    const bool zr = trigger_down(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0.6f);
    if (!sCaptureArmed) {
        bool any = zl || zr || trigger_down(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 0.3f) ||
                   trigger_down(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0.3f);
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b) {
            any = any || down[b];
        }
        sCaptureArmed = !any;
        return;
    }
    int source = kNone;
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT && source == kNone; ++b) {
        if (pressed[b] && b != SDL_GAMEPAD_BUTTON_GUIDE) {
            source = b;
        }
    }
    if (source == kNone && zl) {
        source = kTrigger + SDL_GAMEPAD_AXIS_LEFT_TRIGGER;
    }
    if (source == kNone && zr) {
        source = kTrigger + SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
    }
    if (source != kNone) {
        assign(sCapture, source);
        sCapture = -1;
    }
}

// ---- GPU clock --------------------------------------------------------------------------------------------

// GPU clock: apm performance configuration 0x92220008 is one of the console's own handheld profiles (CPU
// 1020 MHz, GPU 460.8 MHz, memory 1331.2 MHz), not an overclock; homebrew normally gets the 307.2 MHz
// default. 0x00020004 (GPU 384 MHz) is the fallback if the first is refused. The original configuration is
// saved so turning the option off restores it.
constexpr u32 kGpuBoostConf = 0x92220008;
constexpr u32 kGpuBoostFallback = 0x00020004;
bool sApmReady;
bool sApmSaved;
u32 sApmOriginal;

// Current clock of one module (CPU, GPU, memory) in MHz, or 0 if clkrst is unavailable (as in Ryujinx).
u32 clock_mhz(PcvModuleId module) {
    ClkrstSession session;
    u32 hz = 0;
    if (R_SUCCEEDED(clkrstOpenSession(&session, module, 3))) {
        clkrstGetClockRate(&session, &hz);
        clkrstCloseSession(&session);
    }
    return hz / 1000000;
}

// Logs the real clocks and the active apm configuration, to confirm a clock change took effect.
void log_clocks(const char* when) {
    static bool clkrstReady = R_SUCCEEDED(clkrstInitialize());
    ApmPerformanceMode mode = ApmPerformanceMode_Invalid;
    u32 conf = 0;
    if (sApmReady) {
        apmGetPerformanceMode(&mode);
        apmGetPerformanceConfiguration(mode, &conf);
    }
    if (clkrstReady) {
        std::printf("[sfa-nx] clocks %s: CPU %u MHz, GPU %u MHz, EMC %u MHz, mode %d, configuration 0x%08X\n", when,
                    clock_mhz(PcvModuleId_CpuBus), clock_mhz(PcvModuleId_GPU), clock_mhz(PcvModuleId_EMC), (int)mode,
                    conf);
    } else {
        std::printf("[sfa-nx] clocks %s: clkrst unavailable, mode %d, configuration 0x%08X\n", when, (int)mode, conf);
    }
}

// Applies the GPU clock setting (called at startup and when the option changes).
void apply_gpu_clock() {
    if (!sApmReady) {
        sApmReady = R_SUCCEEDED(apmInitialize());
    }
    if (sApmReady && !sApmSaved) {
        sApmSaved = R_SUCCEEDED(apmGetPerformanceConfiguration(ApmPerformanceMode_Normal, &sApmOriginal));
    }
    if (sApmReady && sApmSaved) {
        if (sGpuBoost) {
            if (R_FAILED(apmSetPerformanceConfiguration(ApmPerformanceMode_Normal, kGpuBoostConf))) {
                apmSetPerformanceConfiguration(ApmPerformanceMode_Normal, kGpuBoostFallback);
            }
        } else {
            apmSetPerformanceConfiguration(ApmPerformanceMode_Normal, sApmOriginal);
        }
    }
    log_clocks(sGpuBoost ? "(GPU clock 460 MHz)" : "(GPU clock system)");
}

// One integer "key=value" from switch_menu.ini (the last one wins); used by the renderer and Aurora to
// read the at-next-start settings through sfaSwitchMenuIniValue.
int ini_value(const char* key, int def) {
    FILE* f = std::fopen(kSettingsPath, "r");
    if (f == nullptr) {
        return def;
    }
    char line[256];
    char format[64];
    std::snprintf(format, sizeof(format), "%s=%%d", key);
    int value = def;
    int v;
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        if (std::sscanf(line, format, &v) == 1) {
            value = v;
        }
    }
    std::fclose(f);
    return value;
}

// ---- settings -------------------------------------------------------------------------------------------

// Reads switch_menu.ini on the first menu frame and applies each setting. Unknown lines are ignored.
void load_settings() {
    sLoaded = true;
    sClearShaders = shader_clear_pending();
    FILE* f = std::fopen(kSettingsPath, "r");
    if (f == nullptr) {
        return;
    }
    char line[256];
    int value;
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        int m[PAD_BUTTON_COUNT];
        if (std::sscanf(line, "fps=%d", &value) == 1) {
            sShowFps = value != 0;
        } else if (std::sscanf(line, "invert_look=%d", &value) == 1) {
            sInvertLook = value != 0;
        } else if (std::sscanf(line, "threads=%d", &value) == 1) {
            for (int policy : kPolicies) {
                if (policy == value) {
                    melee_nx_perf_set_affinity(value);
                }
            }
        } else if (std::sscanf(line, "audio_thread=%d", &value) == 1) {
            fhAISetThreaded(value != 0);
        } else if (std::sscanf(line, "frame_overlap=%d", &value) == 1) {
            aurora_set_frame_overlap(value != 0);
        } else if (std::sscanf(line, "pacing=%d", &value) == 1) {
            sVsyncPace = value != 0;
        } else if (std::sscanf(line, "frame_step=%d", &value) == 1) {
            sWholeSteps = value != 0;
        } else if (std::sscanf(line, "renderer=%d", &value) == 1) {
            aurora_set_direct_renderer(value != 0);
        } else if (std::sscanf(line, "gpu_clock=%d", &value) == 1) {
            sGpuBoost = value != 0;
        } else if (std::sscanf(line, "compression=%d", &value) == 1) {
            sCompression = value < 0 ? 0 : value > 2 ? 2 : value;
        } else if (std::sscanf(line, "zcull=%d", &value) == 1) {
            sZcull = value != 0;
        } else if (std::sscanf(line, "texture_bc1=%d", &value) == 1) {
            sTexBc1 = value != 0;
        } else if (std::sscanf(line, "hd_textures=%d", &value) == 1) {
            sHdTextures = value != 0;
        } else if (std::sscanf(line, "language=%d", &value) == 1) {
            sLanguage = value >= 0 && value <= 5 && value != 4 ? value : 0;
        } else if (std::sscanf(line, "hd_texture_mb=%d", &value) == 1) {
            sHdTextureMb = value < 64 ? 64 : value > 1536 ? 1536 : value;
        } else if (std::sscanf(line, "shader_tour=%d", &value) == 1) {
            sTourAutoStart = value >= 0 ? value : -1;
        } else if (std::sscanf(line, "debug_menu=%d", &value) == 1) {
            sShowDebug = value != 0;  // shows the Debug tab (testing only)
        } else if (std::sscanf(line, "profiler=%d", &value) == 1) {
            // Test runs (Ryujinx) only: read, never written; the menu's Profiler item is not saved.
            if (value != 0) {
                sfaProfSetEnabled(1);
            }
        } else if (std::sscanf(line, "map=%d %d %d %d %d %d %d %d %d %d %d %d", &m[0], &m[1], &m[2], &m[3], &m[4],
                               &m[5], &m[6], &m[7], &m[8], &m[9], &m[10], &m[11]) == PAD_BUTTON_COUNT) {
            bool ok = true;
            for (int v : m) {
                ok = ok && valid_source(v);
            }
            if (ok) {
                std::memcpy(sSources, m, sizeof(sSources));
                sLoadedCustom = true;
            }
        }
    }
    std::fclose(f);
}

// Remembers the values this session started with and applies the GPU clock.
void load_settings_done() {
    sBootCompression = sCompression;
    sBootZcull = sZcull;
    sBootTexBc1 = sTexBc1;
    sBootHdTextures = sHdTextures;
    sBootLanguage = sLanguage;
    apply_gpu_clock();
}

// Rewrites switch_menu.ini with every setting, with # comment lines explaining the values for anyone
// editing it by hand. Test-only keys (shader_tour, profiler) are never written.
void save_settings() {
    FILE* f = std::fopen(kSettingsPath, "w");
    if (f == nullptr) {
        return;
    }
    std::fprintf(f, "fps=%d\ninvert_look=%d\n", sShowFps ? 1 : 0, sInvertLook ? 1 : 0);
    std::fprintf(f, "# threads: 3 tuned, 4 game, 2 split, 0 baseline\nthreads=%d\naudio_thread=%d\n",
                 melee_nx_perf_affinity(), fhAIThreaded());
    std::fprintf(f, "frame_overlap=%d\n", aurora_frame_overlap() ? 1 : 0);
    std::fprintf(f, "# frame_step: 1 whole frames, 0 measured\nframe_step=%d\n", sWholeSteps ? 1 : 0);
    std::fprintf(f, "# pacing: 1 display vsync, 0 timer\npacing=%d\n", sVsyncPace ? 1 : 0);
    std::fprintf(f, "# renderer: 1 direct, 0 dawn\nrenderer=%d\n", aurora_direct_renderer() ? 1 : 0);
    std::fprintf(f, "# gpu_clock: 1 460 MHz handheld, 0 system\ngpu_clock=%d\n", sGpuBoost ? 1 : 0);
    std::fprintf(f, "# compression: 0 off, 1 colour, 2 colour and depth (at next start)\ncompression=%d\n",
                 sCompression);
    std::fprintf(f, "zcull=%d\ntexture_bc1=%d\n", sZcull ? 1 : 0, sTexBc1 ? 1 : 0);
    std::fprintf(f, "# hd_textures: 1 load mods/*/textures (at next start); hd_texture_mb: their cache in MiB\n"
                    "hd_textures=%d\nhd_texture_mb=%d\n", sHdTextures ? 1 : 0, sHdTextureMb);
    std::fprintf(f, "# language (PAL disc, at next start): 0 English, 1 French, 2 German, 3 Italian, 5 Spanish\n"
                    "language=%d\n", sLanguage);
    // Kept only when enabled, so a normal ini never mentions the testing tab.
    if (sShowDebug) {
        std::fprintf(f, "# debug_menu: 1 shows the Debug tab\ndebug_menu=1\n");
    }
    if (sCustom) {
        std::fprintf(f, "# GameCube A B X Y Z L R Start Up Down Left Right: SDL button, 104 ZL, 105 ZR, -1 none\nmap=");
        for (int i = 0; i < PAD_BUTTON_COUNT; ++i) {
            std::fprintf(f, i == 0 ? "%d" : " %d", sSources[i]);
        }
        std::fprintf(f, "\n");
    }
    std::fclose(f);
}

// ---- menu -----------------------------------------------------------------------------------------------

// Any connected controller; the menu listens to all of them through the first one SDL reports.
SDL_Gamepad* first_gamepad() {
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    SDL_Gamepad* pad = nullptr;
    for (int i = 0; i < count && pad == nullptr; ++i) {
        pad = SDL_GetGamepadFromID(ids[i]);
    }
    SDL_free(ids);
    return pad;
}

// The button printed with this letter (SDL's positions follow the Xbox layout).
SDL_GamepadButton button_labelled(SDL_Gamepad* pad, SDL_GamepadButtonLabel label, SDL_GamepadButton fallback) {
    for (SDL_GamepadButton b : {SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST,
                                SDL_GAMEPAD_BUTTON_NORTH}) {
        if (SDL_GetGamepadButtonLabel(pad, b) == label) {
            return b;
        }
    }
    return fallback;
}

// Changes one setting (direction -1 for D-Pad Left, otherwise forward) and saves the ini.
void toggle(int item, int direction) {
    if (item == kItemFps) {
        sShowFps = !sShowFps;
    } else if (item == kItemInvertLook) {
        sInvertLook = !sInvertLook;
    } else if (item == kItemProfiler) {
        sfaProfSetEnabled(!sfaProfEnabled());
        return; // not saved: the profiler always starts off
    } else if (item == kItemThreads) {
        int index = 0;
        for (int i = 0; i < kPolicyCount; ++i) {
            if (kPolicies[i] == melee_nx_perf_affinity()) {
                index = i;
            }
        }
        const int next = kPolicies[(index + (direction < 0 ? kPolicyCount - 1 : 1)) % kPolicyCount];
        melee_nx_perf_set_affinity(next);
        std::printf("[sfa-nx] threads: %s\n", melee_nx_perf_affinity_name(next));
    } else if (item == kItemAudio) {
        fhAISetThreaded(!fhAIThreaded());
        std::printf("[sfa-nx] audio thread: %s\n", fhAIThreaded() ? "on" : "off");
    } else if (item == kItemRenderer) {
        aurora_set_direct_renderer(!aurora_direct_renderer());
        std::printf("[sfa-nx] renderer: %s\n", aurora_direct_renderer() ? "direct" : "dawn");
    } else if (item == kItemOverlap) {
        aurora_set_frame_overlap(!aurora_frame_overlap());
        std::printf("[sfa-nx] frame overlap: %s\n", aurora_frame_overlap() ? "on" : "off");
    } else if (item == kItemFrameStep) {
        sWholeSteps = !sWholeSteps;
        std::printf("[sfa-nx] frame step: %s\n", sWholeSteps ? "whole frames" : "measured");
    } else if (item == kItemPacing) {
        sVsyncPace = !sVsyncPace;
        std::printf("[sfa-nx] pacing: %s\n", sVsyncPace ? "display vsync" : "timer");
    } else if (item == kItemGpuClock) {
        sGpuBoost = !sGpuBoost;
        apply_gpu_clock();
    } else if (item == kItemCompression) {
        sCompression = (sCompression + (direction < 0 ? 2 : 1)) % 3;
    } else if (item == kItemZcull) {
        sZcull = !sZcull;
    } else if (item == kItemTexBc1) {
        sTexBc1 = !sTexBc1;
    } else if (item == kItemHdTextures) {
        sHdTextures = !sHdTextures;
    } else if (item == kItemLanguage) {
        // Step through kLanguages (A and Right forward, Left back).
        constexpr int n = sizeof(kLanguages) / sizeof(kLanguages[0]);
        int at = 0;
        while (at < n && kLanguages[at] != sLanguage) {
            ++at;
        }
        sLanguage = kLanguages[(at + (direction < 0 ? n - 1 : 1)) % n];
    } else if (item == kItemShaderCache) {
        if (sClearShaders) {
            sClearShaders = std::remove(kClearShadersPath) != 0;
        } else if (FILE* f = std::fopen(kClearShadersPath, "w")) {
            std::fputs("1\n", f);
            std::fclose(f);
            sClearShaders = true;
        }
        return;
    } else if (item == kItemTour) {
        if (sfaShaderTourActive()) {
            sfaShaderTourStop();
        } else {
            if (direction == 0) {
                sTourRefused = sfaShaderTourStart(sfaShaderTourResumeWarp()) == 0;
            } else {
                // Left/Right: pick route 1, 2 or 3 (where each map's stop lands); it starts from the first warp
                sfaShaderTourSetRoute(sfaShaderTourRoute() + direction);
                return;
            }
            sOpen = sTourRefused;  // close the menu to watch the tour
        }
        return;
    } else if (item == kItemMapping) {
        sMapPage = true;
        sMapSelected = 0;
        return;
    }
    save_settings();
}

// Mapping page input: D-Pad chooses a row, A starts capturing a new control (or resets / goes back), B
// returns to the menu.
void update_map_page(bool a, bool b, const bool pressed[]) {
    if (b) {
        sMapPage = false;
    } else if (pressed[SDL_GAMEPAD_BUTTON_DPAD_UP]) {
        sMapSelected = (sMapSelected + kRowCount - 1) % kRowCount;
    } else if (pressed[SDL_GAMEPAD_BUTTON_DPAD_DOWN]) {
        sMapSelected = (sMapSelected + 1) % kRowCount;
    } else if (a) {
        if (sMapSelected == kRowBack) {
            sMapPage = false;
        } else if (sMapSelected == kRowReset) {
            std::memcpy(sSources, sDefaults, sizeof(sSources));
            sCustom = false;
            sLoadedCustom = false;
            apply_sources();
            save_settings();
            std::printf("[sfa-nx] button mapping: reset to defaults\n");
        } else if (sMapPad != nullptr) {
            sCapture = sMapSelected;
            sCaptureArmed = false;
        }
    }
}

// Menu input, read straight from SDL every frame (the game's own pad input is blanked while the menu is
// open). - toggles the menu; while a capture runs, all input goes to the capture.
void update_input() {
    SDL_Gamepad* pad = first_gamepad();
    if (pad == nullptr) {
        return;
    }
    bool down[SDL_GAMEPAD_BUTTON_COUNT];
    bool pressed[SDL_GAMEPAD_BUTTON_COUNT];
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b) {
        down[b] = SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(b));
        pressed[b] = down[b] && !sPrevDown[b];
        sPrevDown[b] = down[b];
    }
    if (sOpen && sCapture >= 0) {
        update_capture(sMapPad != nullptr ? sMapPad : pad, down, pressed);
        return;
    }
    // A and B by the letters printed on the buttons, whatever position SDL gives them.
    const SDL_GamepadButton a = button_labelled(pad, SDL_GAMEPAD_BUTTON_LABEL_A, SDL_GAMEPAD_BUTTON_EAST);
    const SDL_GamepadButton b = button_labelled(pad, SDL_GAMEPAD_BUTTON_LABEL_B, SDL_GAMEPAD_BUTTON_SOUTH);

    if (pressed[SDL_GAMEPAD_BUTTON_BACK]) {
        sOpen = !sOpen;
        sSelected = 0;
        sMapPage = false;
        return;
    }
    if (!sOpen) {
        return;
    }
    if (sMapPage) {
        update_map_page(pressed[a], pressed[b], pressed);
        return;
    }
    int count = 0;
    const int* items = tab_items(sTab, &count);
    const int item = items[sSelected < count ? sSelected : 0];
    if (pressed[b]) {
        sOpen = false;
    } else if (sShowDebug &&  // L / R switch tabs only when the Debug tab is shown
               (pressed[SDL_GAMEPAD_BUTTON_LEFT_SHOULDER] || pressed[SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER])) {
        sTab = 1 - sTab;
        sSelected = 0;
    } else if (pressed[SDL_GAMEPAD_BUTTON_DPAD_UP]) {
        sSelected = (sSelected + count - 1) % count;
    } else if (pressed[SDL_GAMEPAD_BUTTON_DPAD_DOWN]) {
        sSelected = (sSelected + 1) % count;
    } else if (pressed[a] || pressed[SDL_GAMEPAD_BUTTON_DPAD_LEFT] || pressed[SDL_GAMEPAD_BUTTON_DPAD_RIGHT]) {
        if (item == kItemClose) {
            sOpen = !pressed[a];
        } else if (item == kItemUpdate) {
            if (pressed[a]) {
                sfaUpdaterActivate();
            }
        } else if (item == kItemTour) {
            // A: start / resume / stop; Left/Right: previous / next route (restarts from warp 0)
            toggle(item, pressed[a] ? 0 : pressed[SDL_GAMEPAD_BUTTON_DPAD_LEFT] ? -1 : 1);
        } else if (item != kItemMapping || pressed[a]) {
            toggle(item, pressed[SDL_GAMEPAD_BUTTON_DPAD_LEFT] ? -1 : 1);
        }
    }
}

// Counts presented frames and updates the FPS value every half second.
void update_fps() {
    const Uint64 now = SDL_GetTicksNS();
    if (sFpsStartNs == 0) {
        sFpsStartNs = now;
    }
    ++sFpsFrames;
    if (now - sFpsStartNs >= SDL_NS_PER_SECOND / 2) {
        sFps = static_cast<float>(sFpsFrames) * 1e9f / static_cast<float>(now - sFpsStartNs);
        sFpsFrames = 0;
        sFpsStartNs = now;
    }
}

// FPS counter in the top-left corner on a translucent box.
void draw_fps() {
    char text[32];
    std::snprintf(text, sizeof(text), "%.0f FPS", sFps);
    ImDrawList* list = ImGui::GetForegroundDrawList();
    const float size = 28.0f;
    const ImVec2 pos(16.0f, 12.0f);
    const ImVec2 extent = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    list->AddRectFilled(ImVec2(pos.x - 6, pos.y - 3), ImVec2(pos.x + extent.x + 6, pos.y + extent.y + 3),
                        IM_COL32(0, 0, 0, 150), 6.0f);
    list->AddText(ImGui::GetFont(), size, pos, IM_COL32(255, 255, 255, 255), text);
}

// Profiler overlay under the FPS counter: frame phases over the last half second, avg / max in ms.
void draw_profiler(float top) {
    double avg[4], mx[4];
    unsigned long long samples = 0;
    sfaProfSections(avg, mx, &samples);
    static const char* const names[4] = {"game", "present", "wait", "acquire"};
    char lines[6][64];
    std::snprintf(lines[0], sizeof(lines[0]), "PROFILER  %llu samples", samples);
    for (int i = 0; i < 4; ++i) {
        std::snprintf(lines[i + 1], sizeof(lines[i + 1]), "%-8s %5.1f  max %5.1f ms", names[i], avg[i], mx[i]);
    }
    std::snprintf(lines[5], sizeof(lines[5]), "frame    %5.1f ms", avg[0] + avg[1] + avg[2] + avg[3]);
    ImDrawList* list = ImGui::GetForegroundDrawList();
    const float size = 22.0f;
    const ImVec2 pos(16.0f, top);
    float width = 0.0f;
    for (const auto& line : lines) {
        const float w = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0.0f, line).x;
        width = w > width ? w : width;
    }
    list->AddRectFilled(ImVec2(pos.x - 6, pos.y - 3), ImVec2(pos.x + width + 6, pos.y + size * 6 + 3),
                        IM_COL32(0, 0, 0, 150), 6.0f);
    for (int i = 0; i < 6; ++i) {
        list->AddText(ImGui::GetFont(), size, ImVec2(pos.x, pos.y + size * i),
                      i == 0 ? IM_COL32(255, 205, 64, 255) : IM_COL32(255, 255, 255, 255), lines[i]);
    }
}

// Menu colours: normal rows, the selected row, and a pressed button on the mapping page.
const ImVec4 kWhite(1.0f, 1.0f, 1.0f, 1.0f);
const ImVec4 kGold(1.0f, 0.8f, 0.25f, 1.0f);
const ImVec4 kGreen(0.35f, 1.0f, 0.45f, 1.0f);

// Appends a control name to the "Switch held" line.
void append_name(char* raw, size_t size, const char* name) {
    std::strncat(raw, name, size - std::strlen(raw) - 1);
    std::strncat(raw, "  ", size - std::strlen(raw) - 1);
}

// Mapping page: one row per GameCube button with its Switch control, "PRESSED" while the game would see
// it, "*" when it differs from the default; below, the sticks, triggers and raw Switch buttons held.
// `scale` is the font scale (the main menu's, see draw_menu); the hint line is smaller, as on the menu.
void draw_map_page(float scale) {
    ImGui::SetWindowFontScale(scale);
    ImGui::TextUnformatted("Button mapping  (GameCube = Switch)");
    ImGui::Separator();
    if (sMapPad == nullptr) {
        ImGui::TextColored(kGold, "No controller on port 1");
    }
    const u16 held = sLastStatus.err == PAD_ERR_NONE ? sLastStatus.button : 0;
    for (int i = 0; i < kRowCount; ++i) {
        const bool sel = i == sMapSelected;
        const ImVec4 color = sel ? kGold : kWhite;
        if (i >= PAD_BUTTON_COUNT) {
            ImGui::TextColored(color, "%s %s", sel ? ">" : " ", i == kRowReset ? "Reset to defaults" : "Back");
            continue;
        }
        const char* value = i == sCapture ? "press a button... (- cancels)" : switch_name(sSources[i]);
        ImGui::TextColored(color, "%s %-12s %-20s", sel ? ">" : " ", kGc[i].name, value);
        ImGui::SameLine();
        if ((held & kGc[i].bit) != 0) {
            ImGui::TextColored(kGreen, "PRESSED");
        } else {
            ImGui::TextUnformatted(sSources[i] != sDefaults[i] ? "*" : " ");
        }
    }
    ImGui::Separator();
    const PADStatus& s = sLastStatus;
    ImGui::Text("Stick %4d %4d   C-Stick %4d %4d   L %3d   R %3d", s.stickX, s.stickY, s.substickX, s.substickY,
                s.triggerLeft, s.triggerRight);
    char raw[256] = "";
    if (SDL_Gamepad* pad = sMapPad) {
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b) {
            if (SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(b))) {
                append_name(raw, sizeof(raw), switch_name(b));
            }
        }
        for (int t : {kTrigger + SDL_GAMEPAD_AXIS_LEFT_TRIGGER, kTrigger + SDL_GAMEPAD_AXIS_RIGHT_TRIGGER}) {
            if (trigger_down(pad, static_cast<SDL_GamepadAxis>(t - kTrigger), 0.3f)) {
                append_name(raw, sizeof(raw), switch_name(t));
            }
        }
    }
    ImGui::Text("Switch held: %s", raw);
    ImGui::SetWindowFontScale(scale * 0.7f);
    ImGui::TextDisabled("D-Pad: choose   A: remap   B: back   * = changed");
}

// The menu window, centred. The font is scaled so the longer Debug tab fits the screen height, and the
// Game tab is padded to the same height so the window does not jump when switching tabs.
void draw_menu() {
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.85f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    ImGui::Begin("SFA-NX menu", nullptr, flags);
    // The tallest tab sets the font scale and height: Debug when shown, otherwise Game.
    int debugCount = 0;
    tab_items(sShowDebug ? 1 : 0, &debugCount);
    ImGui::SetWindowFontScale(1.0f);
    const float base = ImGui::GetFontSize();
    const float spacing = ImGui::GetStyle().ItemSpacing.y;
    // The scale at which `n` lines fill 90% of the screen height.
    auto fit = [&](float n) { return (display.y * 0.9f - n * spacing) / (n * base); };
    const float scale = std::min(2.8f, fit(static_cast<float>(debugCount + 3)));
    if (sMapPage) {
        // Same font and size as the menu, made smaller only when the page's rows would not fit the screen:
        // title, maybe "No controller", one row per button plus Reset and Back, sticks, held, and the hint.
        const float mapLines = static_cast<float>(kRowCount + (sMapPad == nullptr ? 5 : 4));
        draw_map_page(std::min(scale, fit(mapLines)));
        ImGui::End();
        return;
    }
    ImGui::SetWindowFontScale(scale);
    ImGui::Dummy(ImVec2(ImGui::CalcTextSize("> Render compression     Resume at warp 76  < restart >").x, 0.0f));
    ImGui::TextUnformatted("SFA-NX");
    // Tab strip only when there is a second tab to switch to.
    if (sShowDebug) {
        ImGui::SameLine();
        ImGui::TextDisabled("  L");
        for (int t = 0; t < 2; ++t) {
            ImGui::SameLine();
            if (t == sTab) {
                ImGui::TextColored(kGold, "[%s]", kTabNames[t]);
            } else {
                ImGui::TextDisabled(" %s ", kTabNames[t]);
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("R");
    }
    ImGui::Separator();
    const char* labels[kItemCount] = {"Show FPS",      "Invert look",   "Profiler",
                                      "Threads",       "Audio thread",         "Frame overlap", "Frame step",
                                      "Pacing",        "Renderer",
                                      "GPU clock",     "Render compression",   "Z-cull",        "Texture BC1",   "HD textures",
                                      "Language",      "Button mapping", "Shader tour",         "Clear shader cache", "Updates",
                                      "Close"};
    const bool values[kItemCount] = {sShowFps, sInvertLook, sfaProfEnabled() != 0, false,
                                     fhAIThreaded() != 0, aurora_frame_overlap(), sWholeSteps, false, false,
                                     false, false, sZcull,
                                     sTexBc1, sHdTextures, false, false, false, false, false, false};
    int count = 0;
    const int* items = tab_items(sTab, &count);
    for (int row = 0; row < count; ++row) {
        const int i = items[row];
        const bool sel = row == sSelected;
        const ImVec4 color = sel ? kGold : kWhite;
        if (i == kItemClose) {
            ImGui::TextColored(color, "%s %s", sel ? ">" : " ", labels[i]);
        } else if (i == kItemMapping) {
            ImGui::TextColored(color, "%s %-21s %s", sel ? ">" : " ", labels[i], sCustom ? "Custom" : "");
        } else if (i == kItemThreads) {
            ImGui::TextColored(color, "%s %-21s < %s >", sel ? ">" : " ", labels[i],
                               melee_nx_perf_affinity_name(melee_nx_perf_affinity()));
        } else if (i == kItemFrameStep) {
            ImGui::TextColored(color, "%s %-21s %s", sel ? ">" : " ", labels[i],
                               sWholeSteps ? "Whole frames" : "Measured");
        } else if (i == kItemPacing) {
            ImGui::TextColored(color, "%s %-21s %s", sel ? ">" : " ", labels[i],
                               sVsyncPace ? "Display vsync" : "Timer");
        } else if (i == kItemRenderer) {
            ImGui::TextColored(color, "%s %-21s %s", sel ? ">" : " ", labels[i],
                               aurora_direct_renderer() ? "Direct" : "Dawn");
        } else if (i == kItemTour) {
            char status[48];
            if (sfaShaderTourActive()) {
                sfaShaderTourStatus(status, sizeof(status));
            } else if (sTourRefused) {
                std::snprintf(status, sizeof(status), "Load a save first");
            } else if (sfaShaderTourResumeWarp() > 0) {
                std::snprintf(status, sizeof(status), "< Route %d > resume at warp %d", sfaShaderTourRoute(),
                              sfaShaderTourResumeWarp());
            } else {
                std::snprintf(status, sizeof(status), "< Route %d > start", sfaShaderTourRoute());
            }
            ImGui::TextColored(color, "%s %-21s %s", sel ? ">" : " ", labels[i], status);
        } else if (i == kItemGpuClock) {
            ImGui::TextColored(color, "%s %-21s %s", sel ? ">" : " ", labels[i],
                               sGpuBoost ? "460 MHz (handheld)" : "System");
        } else if (i == kItemCompression) {
            static const char* const kNames[3] = {"Off", "Colour", "Colour + depth"};
            ImGui::TextColored(color, "%s %-21s < %s >%s", sel ? ">" : " ", labels[i],
                               kNames[sCompression], sCompression != sBootCompression ? "  (restart)" : "");
        } else if (i == kItemZcull || i == kItemTexBc1 || i == kItemHdTextures) {
            const bool pending = i == kItemZcull    ? sZcull != sBootZcull
                                 : i == kItemTexBc1 ? sTexBc1 != sBootTexBc1
                                                    : sHdTextures != sBootHdTextures;
            ImGui::TextColored(color, "%s %-21s %s%s", sel ? ">" : " ", labels[i], values[i] ? "On" : "Off",
                               pending ? "  (restart)" : "");
        } else if (i == kItemLanguage) {
            // Only the PAL disc carries the other languages; other discs always run in their own.
            if (sfaDiscIsPal()) {
                ImGui::TextColored(color, "%s %-21s < %s >%s", sel ? ">" : " ", labels[i], kLanguageNames[sLanguage],
                                   sLanguage != sBootLanguage ? "  (restart)" : "");
            } else {
                ImGui::TextColored(color, "%s %-21s %s", sel ? ">" : " ", labels[i], "PAL disc only");
            }
        } else if (i == kItemUpdate) {
            char status[34];
            sfaUpdaterStatus(status, sizeof(status));
            ImGui::TextColored(color, "%s %-21s %s", sel ? ">" : " ", labels[i], status);
        } else if (i == kItemShaderCache) {
            ImGui::TextColored(color, "%s %-21s %s", sel ? ">" : " ", labels[i],
                               sClearShaders ? "At next start" : "");
        } else {
            ImGui::TextColored(color, "%s %-21s %s", sel ? ">" : " ", labels[i], values[i] ? "On" : "Off");
        }
    }
    for (int row = count; row < debugCount; ++row) {
        ImGui::TextUnformatted(" ");
    }
    ImGui::Separator();
    ImGui::SetWindowFontScale(scale * 0.7f);
    // The L / R hint only applies when the Debug tab is shown.
    ImGui::TextDisabled(sShowDebug ? "L / R: tab   D-Pad: choose   A: change   B / -: close"
                                   : "D-Pad: choose   A: change   B / -: close");
    ImGui::End();
}

} // namespace

// Called once per frame from VIWaitForRetrace (port/src/vi_shim.c), inside Aurora's ImGui frame. Loads the
// settings on the first call, then handles input, the shader tour and the overlays.
extern "C" void sfaSwitchMenuFrame(void) {
    if (!sLoaded) {
        load_settings();
        load_settings_done();
    }
    sync_mapping();
    update_input();
    if (sTourAutoStart >= 0) {
        sTourAutoFrames = sfaShaderTourHasPlayer() ? sTourAutoFrames + 1 : 0;
        if (sTourAutoFrames >= 5 * 60 && sfaShaderTourStart(sTourAutoStart) != 0) {
            sTourAutoStart = -1;
        }
    }
    sfaShaderTourFrame();
    update_fps();
    if (sShowFps) {
        draw_fps();
    }
    if (sfaProfEnabled()) {
        draw_profiler(sShowFps ? 52.0f : 12.0f);
    }
    if (sOpen) {
        draw_menu();
    }
}

// Read by the game's camera and flight code (player.c, the Arwing and cloud-run objects, camera DLL 68)
// to flip the vertical look direction.
extern "C" int sfaSwitchInvertLook(void) { return sInvertLook ? 1 : 0; }

// Accessors for other modules: ini values, frame step mode (game/src/main/pi_videoinit.c) and vsync pacing (vsync_switch.cpp).
extern "C" int sfaSwitchMenuIniValue(const char* key, int def) { return ini_value(key, def); }
extern "C" int sfaWholeFrameSteps(void) { return sWholeSteps ? 1 : 0; }
extern "C" int sfaVsyncPacing(void) { return sVsyncPace ? 1 : 0; }

// Called by the PADRead wrapper with PADRead's PADStatus[4]: adds port 1's GameCube buttons that are bound to
// ZL/ZR, keeps port 1 for the mapping page's tester, and clears all input while the menu is open (error kept).
extern "C" void sfaSwitchMenuFilterPad(void* status) {
    PADStatus* pads = static_cast<PADStatus*>(status);
    if (pads[0].err == PAD_ERR_NONE && sMapPad != nullptr) {
        for (int i = 0; i < PAD_BUTTON_COUNT; ++i) {
            if (sSources[i] >= kTrigger &&
                trigger_down(sMapPad, static_cast<SDL_GamepadAxis>(sSources[i] - kTrigger), 0.5f)) {
                pads[0].button |= kGc[i].bit;
            }
        }
    }
    // Test runs only (shader_tour= in switch_menu.ini): Ryujinx has no one to press buttons, so tap A
    // through the title and file select until the save's player exists.
    if (sTourAutoStart >= 0 && !sfaShaderTourHasPlayer()) {
        static unsigned tap;
        if (++tap > 20 * 60 && tap % 90 < 6) {
            pads[0].err = PAD_ERR_NONE;
            pads[0].button |= PAD_BUTTON_A;
        }
    }
    sLastStatus = pads[0];
    if (!sOpen) {
        return;
    }
    for (int i = 0; i < PAD_CHANMAX; ++i) {
        const s8 err = pads[i].err;
        std::memset(&pads[i], 0, sizeof(PADStatus));
        pads[i].err = err;
    }
}
