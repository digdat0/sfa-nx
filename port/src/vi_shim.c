#include <dolphin/types.h>
#include <dolphin/os.h>
#include <dolphin/vi.h>
#include <aurora/aurora.h>
#include <aurora/dvd.h>
#include <aurora/event.h>
#include <SDL3/SDL_timer.h>
#include <stdlib.h>
#include "foxhollow_autosave.h"
#include "foxhollow_compat.h"
#include "foxhollow_config.h"
#include "foxhollow_mods.h"
#include "foxhollow_quit.h"
#include "foxhollow_splash.h"
#include "shim_log.h"

#ifdef __SWITCH__
/* Restart support for shutdown_and_exit: switch/src/updater_switch.cpp (relaunch requested) and
   switch/src/clang_tls_switch.c (end every thread, then return to hbloader). */
int sfaRelaunchPending(void);
int kartpad_end_threads(void);
void aurora_switch_dk_release_all(void); /* switch/deko/device.cpp: frees GPU memory, queue, device */
void sfaSwitchMenuReleaseServices(void); /* switch/src/switch_menu.cpp: closes apm and clkrst */
unsigned int romfsUnmount(const char* name); /* libnx; romfsExit() is a macro for romfsUnmount("romfs") */
__attribute__((noreturn)) void kartpad_exit_to_loader(void);
#endif

typedef void (*VIRetraceCallback)(u32 retraceCount);

#if defined(FOXHOLLOW_DEBUG_SHORTCUTS)
void fhDebugOverlayDraw(void);
void fhGalleryDrawOverlay(void);
#endif
#if defined(FOXHOLLOW_DEBUG_INPUT_TIMING)
void fhDebugOverlayFrameBegin(void);
void fhDebugOverlayFramePresented(void);
#endif
void fhGalleryUpdate(void);

static VIRetraceCallback sPreRetraceCallback;
static VIRetraceCallback sPostRetraceCallback;
static void* sNextFrameBuffer;
static u32 sRetraceCount;
static int sFrameOpen;
static int sBlack;
static Uint64 sNextRetraceNs;

void fhGXCompleteFrame(void);
#ifdef __SWITCH__
void sfaSwitchMenuFrame(void); /* switch/src/switch_menu.cpp: - menu, FPS counter, profiler, shader tour */
void sfaProfMark(int id);      /* switch/src/prof_switch.cpp: frame phase timing */
#define SFA_PROF_MARK(id) sfaProfMark(id)
/* switch/src/vsync_switch.cpp: waits for the display's vsync; 0 means use the timer below instead. */
int sfaVsyncWait(void);
#else
#define SFA_PROF_MARK(id) ((void)0)
#endif

enum { VI_RETRACE_DEFAULT_HZ = 60 };

static void wait_for_retrace_deadline(void) {
  const int limit = fhConfigFrameLimit();
  Uint64 periodNs;
  Uint64 now;

  if (limit == 0) {
    sNextRetraceNs = 0;
    return;
  }
#ifdef __SWITCH__
  if (limit == 60 && sfaVsyncWait()) {
    sNextRetraceNs = 0;
    return;
  }
#endif

  periodNs = SDL_NS_PER_SECOND / (Uint64)(limit > 0 ? limit : VI_RETRACE_DEFAULT_HZ);
  now = SDL_GetTicksNS();

  if (sNextRetraceNs == 0) {
    sNextRetraceNs = now + periodNs;
  } else if (now >= sNextRetraceNs + periodNs) {
    /* Do not run several retraces back-to-back after a long host stall. */
    sNextRetraceNs = now + periodNs;
  }

  const Uint64 deadline = sNextRetraceNs;
  sNextRetraceNs += periodNs;
  if (now < deadline) {
    SDL_DelayPrecise(deadline - now);
  }
}

static void shutdown_and_exit(void) {
  /* Drop the "interrupts off" lock (the audio thread needs it to finish its pump), then stop that thread
     before anything it touches is torn down. */
  (void)fhIrqSuspend();
  fhAIShutdown();
  fhAutosaveShutdown();
  fhModsShutdown();
  aurora_dvd_close();
  aurora_shutdown();
#ifdef __SWITCH__
  /* The menu's Restart: once every thread has ended, return to hbloader so it starts the NRO again. With a
     thread still running that would execute the unloaded image, so then end the process as before. */
  if (sfaRelaunchPending()) {
    fhMovieShutdown();
    if (kartpad_end_threads() == 0) {
      /* No thread can render any more: give the GPU memory back, or hbloader can't map the next NRO
         over heap pages still locked to the GPU (hbl aborts with 2001-0106). */
      aurora_switch_dk_release_all();
      /* Services and files left open would outlive this NRO in hbloader's process (see the function). */
      sfaSwitchMenuReleaseServices();
      romfsUnmount("romfs"); /* main_sfa.cpp mounted it; its NRO file handle would leak */
      kartpad_exit_to_loader();
    }
  }
#endif
  exit(0);
}

static void pump_events(void) {
  const AuroraEvent* event = aurora_update();
  int quitting = fhQuitRequested();

  while (event != NULL && event->type != AURORA_NONE) {
    if (event->type == AURORA_EXIT) {
      quitting = 1;
    }
    ++event;
  }
  if (quitting) {
    shutdown_and_exit();
  }
}

void foxhollowFramePumpInit(void) {
  pump_events();
  sFrameOpen = aurora_begin_frame();
}

void VIWaitForRetrace(void) {
  SFA_PROF_MARK(1);
  /* The game may call this with "interrupts off" (waitNextFrame does). Holding the lock through the end of
     the frame, the present and the wait blocked the audio thread for up to 63 ms, so it is dropped here and
     taken back once the next frame has begun. */
  const int irqHeld = fhIrqSuspend();
  if (sFrameOpen) {
    fhModsDraw();
#if defined(FOXHOLLOW_DEBUG_SHORTCUTS)
    fhDebugOverlayDraw();
    fhGalleryDrawOverlay();
#endif
#ifdef __SWITCH__
    sfaSwitchMenuFrame();
#endif
    if (sBlack) {
      fhDrawBlackScreen();
    }
    fhGXCompleteFrame();
    aurora_end_frame();
    SFA_PROF_MARK(2);
#if defined(FOXHOLLOW_DEBUG_INPUT_TIMING)
    fhDebugOverlayFramePresented();
#endif
    sFrameOpen = 0;
  }
  fhModsUpdate();
  fhGalleryUpdate();
  fhAutosaveUpdate();
  /* The audio thread may mix while this thread waits, even when it waits with interrupts off. */
  wait_for_retrace_deadline();
  SFA_PROF_MARK(3);
  pump_events();
  while (!aurora_begin_frame()) {
    pump_events();
  }
  sFrameOpen = 1;
  fhIrqResume(irqHeld);
  SFA_PROF_MARK(0);
#if defined(FOXHOLLOW_DEBUG_INPUT_TIMING)
  fhDebugOverlayFrameBegin();
#endif
  fhAIPump();

  sRetraceCount++;
  {
    /* The retrace callbacks were interrupt handlers on the GameCube: run them with interrupts off. */
    BOOL level = OSDisableInterrupts();
    if (sPreRetraceCallback) {
      sPreRetraceCallback(sRetraceCount);
    }
    if (sPostRetraceCallback) {
      sPostRetraceCallback(sRetraceCount);
    }
    OSRestoreInterrupts(level);
  }
}

VIRetraceCallback VISetPreRetraceCallback(VIRetraceCallback cb) {
  VIRetraceCallback prev = sPreRetraceCallback;
  sPreRetraceCallback = cb;
  return prev;
}

VIRetraceCallback VISetPostRetraceCallback(VIRetraceCallback cb) {
  VIRetraceCallback prev = sPostRetraceCallback;
  sPostRetraceCallback = cb;
  return prev;
}

void VISetNextFrameBuffer(void* fb) { sNextFrameBuffer = fb; }
void* VIGetNextFrameBuffer(void) { return sNextFrameBuffer; }
void VISetBlack(BOOL black) { sBlack = black ? 1 : 0; }
u32 VIGetRetraceCount(void) { return sRetraceCount; }
u32 VIGetNextField(void) { return 0; }
u32 VIGetDTVStatus(void) { return 1; }
