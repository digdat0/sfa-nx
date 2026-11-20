#include <dolphin/os.h>
#include <SDL3/SDL_mutex.h>
#include <aurora/dvd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shim_log.h"

static u32 sSoundMode = 1;
static u32 sProgressiveMode;
static u8 sLanguage;

/* "Interrupts off" is a real lock once the audio mixer runs on its own thread (port/src/ai_shim.c): on the
   GameCube the audio DMA interrupt could never run in the middle of a section the game protected with
   OSDisableInterrupts, and the interrupt handler itself ran to the end without the game running. So:
   - a game-side thread takes sIrqMutex when it disables and releases it when it enables again;
   - the audio thread holds sIrqMutex for a whole pump (fhIrqEnterHandler/fhIrqLeaveHandler), and inside
     that, Disable/Enable/Restore only track the level (the handler stays atomic, as on hardware).
   Each thread has its own level. While nothing else runs game code, the lock is never contended. */
#if defined(_MSC_VER)
#define FH_THREAD_LOCAL __declspec(thread)
#else
#define FH_THREAD_LOCAL _Thread_local
#endif
static SDL_Mutex* sIrqMutex;
static FH_THREAD_LOCAL BOOL tDisabled; /* this thread's interrupt level is "off" */
static FH_THREAD_LOCAL BOOL tLocked;   /* this thread holds sIrqMutex */
static FH_THREAD_LOCAL BOOL tHandler;  /* the audio thread inside a pump */

/* MusyX's own lock is this same mutex (musyx_output.c), so the two can never be taken in opposite orders. */
SDL_Mutex* fhIrqMutex(void) {
  if (sIrqMutex == NULL) {
    sIrqMutex = SDL_CreateMutex(); /* recursive; first created on the game thread, before the audio thread */
  }
  return sIrqMutex;
}

/* Enter "interrupts off" for this thread: the first level change takes the lock (not inside the handler). */
static void irqOff(void) {
  if (!tDisabled) {
    tDisabled = 1;
    if (!tHandler) {
      if (fhIrqMutex() != NULL) {
        SDL_LockMutex(sIrqMutex);
        tLocked = 1;
      }
    }
  }
}

/* Leave "interrupts off": release the lock if this thread took it. */
static void irqOn(void) {
  if (tDisabled) {
    tDisabled = 0;
    if (tLocked) {
      tLocked = 0;
      SDL_UnlockMutex(sIrqMutex);
    }
  }
}

/* Interrupts would fire here: deliver finished DVD reads (game threads only). */
static void irqDeliver(void) {
  if (!tHandler) {
    irqOff();
    aurora_dvd_process_callbacks();
  }
}

BOOL OSDisableInterrupts(void) {
  BOOL prev = !tDisabled;
  irqOff();
  return prev;
}

BOOL OSEnableInterrupts(void) {
  BOOL prev = !tDisabled;
  irqDeliver();
  irqOn();
  return prev;
}

BOOL OSRestoreInterrupts(BOOL level) {
  BOOL prev = !tDisabled;
  if (level) {
    irqDeliver();
    irqOn();
  } else {
    irqOff();
  }
  return prev;
}

/* Audio thread: begin / end one "interrupt handler" run (one pump), holding the lock throughout. */
void fhIrqEnterHandler(void) {
  if (fhIrqMutex() != NULL) {
    SDL_LockMutex(sIrqMutex);
  }
  tHandler = 1;
  tDisabled = 1;
}

void fhIrqLeaveHandler(void) {
  tDisabled = 0;
  tHandler = 0;
  if (sIrqMutex != NULL) {
    SDL_UnlockMutex(sIrqMutex);
  }
}

/* For a game-thread wait that lets "interrupts" run (a frame wait inside a disabled section, OSSleepThread):
   drop the lock, returning whether it was held; fhIrqResume takes it back. */
int fhIrqSuspend(void) {
  if (tHandler || !tDisabled) {
    return 0;
  }
  tDisabled = 0;
  if (tLocked) {
    tLocked = 0;
    SDL_UnlockMutex(sIrqMutex);
  }
  return 1;
}

void fhIrqResume(int held) {
  if (held && !tDisabled) {
    irqOff();
  }
}

void OSPanic(const char* file, int line, const char* msg, ...) {
  fprintf(stderr, "[foxhollow] OSPanic %s:%d: %s\n", file, line, msg);
  abort();
}

u32 OSGetSoundMode(void) { return sSoundMode; }
void OSSetSoundMode(u32 mode) { sSoundMode = mode; }
u32 OSGetProgressiveMode(void) { return sProgressiveMode; }
void OSSetProgressiveMode(u32 mode) { sProgressiveMode = mode; }
u8 OSGetLanguage(void) { return sLanguage; }
void OSSetLanguage(u8 language) { sLanguage = language; }
u32 OSGetResetCode(void) { return 0; }
BOOL OSGetResetButtonState(void) { return 0; }

static OSErrorHandler sErrorHandlers[17];

OSErrorHandler OSSetErrorHandler(OSError error, OSErrorHandler handler) {
  OSErrorHandler prev = 0;
  if (error < 17) {
    prev = sErrorHandlers[error];
    sErrorHandlers[error] = handler;
  }
  return prev;
}
void OSSetSaveRegion(void* start, void* end) { (void)start; (void)end; }

void OSResetSystem(int reset, u32 resetCode, BOOL forceMenu) {
  (void)reset; (void)resetCode; (void)forceMenu;
  fprintf(stderr, "[foxhollow] OSResetSystem requested; exiting\n");
  exit(0);
}
