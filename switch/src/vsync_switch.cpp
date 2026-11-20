// Display setup and vsync pacing.
//
// libnx normally creates the default window in its own __nx_win_init. This file replaces that function
// (and __nx_win_exit / nwindowGetDefault) so the port keeps the ViDisplay handle, which it needs to get the
// display's vsync event. The window is fixed at 1280x720; the system scales it when docked.
//
// sfaVsyncWait is called from VIWaitForRetrace (port/src/vi_shim.c) when the game runs at 60 fps. Waiting
// on the real vsync event instead of a sleep timer keeps each game frame aligned with a display refresh,
// which removed the uneven frame pacing a CPU timer gave (the timer drifts against the display clock).
#include <switch.h>

extern "C" void pc_log_line(const char* fmt, ...);
extern "C" int sfaVsyncPacing(void);

namespace {

// g_state: 0 = vsync event not fetched yet, 1 = ready, -1 = unavailable (never retried) or closed.
// g_timeouts counts waits that hit the 50 ms limit.
ViDisplay g_display;
ViLayer g_layer;
NWindow g_window;
Event g_vsync;
int g_state;
unsigned g_timeouts;

} // namespace

// Same steps as libnx's own __nx_win_init (display, layer, scaling, NWindow), keeping the handles here.
// Any failure aborts with LibnxError_BadGfxInit, like libnx does.
extern "C" void __nx_win_init(void) {
    Result rc = viInitialize(ViServiceType_Default);
    if (R_SUCCEEDED(rc)) {
        rc = viOpenDefaultDisplay(&g_display);
        if (R_SUCCEEDED(rc)) {
            rc = viCreateLayer(&g_display, &g_layer);
            if (R_SUCCEEDED(rc)) {
                rc = viSetLayerScalingMode(&g_layer, ViScalingMode_FitToLayer);
                if (R_SUCCEEDED(rc)) {
                    rc = nwindowCreateFromLayer(&g_window, &g_layer);
                    if (R_SUCCEEDED(rc)) {
                        nwindowSetDimensions(&g_window, 1280, 720);
                    }
                }
                if (R_FAILED(rc)) {
                    viCloseLayer(&g_layer);
                }
            }
            if (R_FAILED(rc)) {
                viCloseDisplay(&g_display);
            }
        }
        if (R_FAILED(rc)) {
            viExit();
        }
    }
    if (R_FAILED(rc)) {
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_BadGfxInit));
    }
}

// Undoes __nx_win_init at exit; also releases the vsync event if it was fetched.
extern "C" void __nx_win_exit(void) {
    if (g_state > 0) {
        eventClose(&g_vsync);
        g_state = -1;
    }
    nwindowClose(&g_window);
    viCloseLayer(&g_layer);
    viCloseDisplay(&g_display);
    viExit();
}

// The window deko3d presents to (used by the renderer's swapchain).
extern "C" NWindow* nwindowGetDefault(void) { return &g_window; }

// Blocks until the next vsync. Returns 1 if it waited, 0 if the caller should fall back to its own
// timer: pacing is turned off in the menu, the event is unavailable, or the wait timed out.
extern "C" int sfaVsyncWait(void) {
    if (!sfaVsyncPacing() || g_state < 0) {
        return 0;
    }
    if (g_state == 0) {
        const Result rc = viGetDisplayVsyncEvent(&g_display, &g_vsync);
        g_state = R_SUCCEEDED(rc) ? 1 : -1;
        pc_log_line("[sfa-nx] vsync event rc 0x%x", rc);
        if (g_state < 0) {
            return 0;
        }
    }
    if (R_FAILED(eventWait(&g_vsync, 50000000ull))) {
        if (++g_timeouts == 1 || g_timeouts % 100 == 0) {
            pc_log_line("[sfa-nx] vsync wait timed out (%u)", g_timeouts);
        }
        return 0;
    }
    return 1;
}
