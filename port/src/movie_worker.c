/* Background decoding for the game's THP movies (the intro and cutscene videos).
   The game decodes each movie frame from a VI retrace callback, which runs with "interrupts off"; a ~10 ms
   decode there held the lock the audio thread needs, and the sound stuttered during movies. dll_3e.c now
   hands the decode job to this worker (fhMovieDecodeStart) and checks later whether it finished
   (fhMovieDecodeBusy). One job at a time; if the thread cannot be created the job runs inline. */
#include <SDL3/SDL.h>

#include <dolphin/types.h>

#include "foxhollow_compat.h"

#if defined(__SWITCH__)
#include <switch.h>
#endif

/* sBusy is set while a job runs; sJob is the job, called with 1 on the worker (0 when run inline). */
static SDL_Thread* sThread;
static SDL_Semaphore* sStart;
static SDL_AtomicInt sBusy;
static int (*sJob)(int);
static int sFailed;

/* Worker loop: runs on core 2 at priority 0x2D (just below the game thread) on Switch. */
static int movieWorkerMain(void* unused) {
  (void)unused;
#if defined(__SWITCH__)
  svcSetThreadCoreMask(CUR_THREAD_HANDLE, 2, 0x7);
  svcSetThreadPriority(CUR_THREAD_HANDLE, 0x2D);
#endif
  for (;;) {
    SDL_WaitSemaphore(sStart);
    if (sJob != NULL) {
      sJob(1);
    }
    SDL_SetAtomicInt(&sBusy, 0);
  }
  return 0;
}

/* Creates the worker on first use; remembers a failure so it is not retried every frame. */
static int movieWorkerReady(void) {
  if (sThread != NULL) {
    return 1;
  }
  if (sFailed) {
    return 0;
  }
  sStart = SDL_CreateSemaphore(0);
  if (sStart != NULL) {
    sThread = SDL_CreateThread(movieWorkerMain, "SFA movie", NULL);
  }
  if (sThread == NULL) {
    sFailed = 1;
    return 0;
  }
  SDL_DetachThread(sThread);
  return 1;
}

/* Starts a decode job; returns 0 (nothing started) while the previous one is still running. */
int fhMovieDecodeStart(int (*job)(int)) {
  if (SDL_GetAtomicInt(&sBusy) != 0) {
    return 0;
  }
  if (!movieWorkerReady()) {
    job(0);
    return 1;
  }
  sJob = job;
  SDL_SetAtomicInt(&sBusy, 1);
  SDL_SignalSemaphore(sStart);
  return 1;
}

int fhMovieDecodeBusy(void) { return SDL_GetAtomicInt(&sBusy) != 0; }

/* Waits for a running job (before the movie is stopped or its buffers change), letting the audio thread run
   meanwhile by dropping the "interrupts off" lock. */
void fhMovieDecodeWait(void) {
  int held;
  if (SDL_GetAtomicInt(&sBusy) == 0) {
    return;
  }
  held = fhIrqSuspend();
  while (SDL_GetAtomicInt(&sBusy) != 0) {
    SDL_DelayNS(200000);
  }
  fhIrqResume(held);
}
