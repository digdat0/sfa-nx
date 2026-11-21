#include <dolphin/ai.h>
#include <dolphin/dvd.h>
#include <dolphin/os.h>
#include <SDL3/SDL.h>
#include <stdio.h>

#include "foxhollow_compat.h"

#define AI_DMA_SAMPLE_RATE 32028
#define AI_STREAM_SAMPLE_RATE 48043
#define AI_STREAM_BLOCK_SIZE 32
#define AI_STREAM_BLOCK_FRAMES 28
/* How much mixed audio is kept queued ahead of the speaker. 40 ms normally; after the audio thread had to wait
   longer than the queue holds (the game thread kept the "interrupts off" lock while it merged file tables or ran
   DVD callbacks: up to 68 ms on hardware) or the queue ran dry, it grows to cover that wait, up to 120 ms, and
   shrinks back 10 ms at a time once 10 s pass without another. A deeper queue only adds latency, never gaps.
   Only the thread that pumps (the audio thread, or the game thread without it) reads or changes it. */
#define AI_OUTPUT_QUEUE_MS 40
#define AI_OUTPUT_QUEUE_MAX_MS 120
static int sQueueMs = AI_OUTPUT_QUEUE_MS;
static Uint64 sQueueGrownNs;
#define AI_DMA_QUEUE_BYTES ((AI_DMA_SAMPLE_RATE * 4 * sQueueMs) / 1000)
#define AI_STREAM_QUEUE_BYTES ((AI_STREAM_SAMPLE_RATE * 4 * sQueueMs) / 1000)

/* Called before each threaded pump with how long it waited for the lock and whether the queue ran dry. */
static void aiAdaptQueue(int waitUs, int underrun) {
  const Uint64 now = SDL_GetTicksNS();
  int need = sQueueMs;
  if (underrun) {
    need = sQueueMs + 20;
  }
  if (waitUs / 1000 + 10 > need) {
    need = waitUs / 1000 + 10; /* the wait, plus one pump period of margin and some */
  }
  if (need > AI_OUTPUT_QUEUE_MAX_MS) {
    need = AI_OUTPUT_QUEUE_MAX_MS;
  }
  if (need > sQueueMs) {
    sQueueMs = need;
    sQueueGrownNs = now;
  } else if (sQueueMs > AI_OUTPUT_QUEUE_MS && now - sQueueGrownNs > 10ull * 1000000000ull) {
    sQueueMs -= 10;
    if (sQueueMs < AI_OUTPUT_QUEUE_MS) {
      sQueueMs = AI_OUTPUT_QUEUE_MS;
    }
    sQueueGrownNs = now;
  }
}

static AIDCallback sDmaCallback;
static AISCallback sStreamCallback;
static uintptr_t sDmaStartAddr;
static u32 sDmaLength;
static u32 sDspSampleRate;
static u32 sStreamPlayState;
static u32 sStreamVolLeft;
static u32 sStreamVolRight;
static u32 sDmaRunning;
static u32 sStreamSampleCount;
static u32 sStreamTrigger;
#if defined(FOXHOLLOW_DEBUG_SHORTCUTS)
static u32 sMuted = 1;
static u32 sMuteKeyWasDown;
#endif
static SDL_AudioStream* sOutputStream;
static SDL_AudioStream* sStreamOutput;
static u8* sStreamData;
static u32 sStreamDataLength;
static u32 sStreamDataOffset;
static s32 sStreamHistLeft1;
static s32 sStreamHistLeft2;
static s32 sStreamHistRight1;
static s32 sStreamHistRight2;

/* Audio thread: fills the SDL queues every few ms instead of once per game frame in VIWaitForRetrace, so
   the MusyX mix (salCtrlDsp, ~1.3 ms a frame on Switch) leaves the game thread and the sound no longer
   stutters when the frame rate drops. Each pump runs as the GameCube's audio interrupt did: holding the
   "interrupts off" lock (os_shim.c), so it never overlaps a section the game protected. Off: the game
   thread pumps once per frame, as before. */
#define AI_THREAD_PERIOD_NS (4 * 1000000ull)
static SDL_Thread* sThread;
static void aiStartThread(void);
static SDL_AtomicInt sThreaded = {1};
static SDL_AtomicInt sThreadStop;
#if defined(__SWITCH__)
void melee_nx_perf_thread(const char* name); /* switch/src/perf_switch.cpp: core and priority */
#endif

#if defined(FOXHOLLOW_DEBUG_SHORTCUTS)
static void aiSetMuted(u32 muted) {
  float gain = muted != 0 ? 0.0f : 1.0f;

  sMuted = muted;
  if (sOutputStream != NULL) {
    SDL_SetAudioStreamGain(sOutputStream, gain);
  }
  if (sStreamOutput != NULL) {
    SDL_SetAudioStreamGain(sStreamOutput, gain);
  }
  fprintf(stderr, "[foxhollow] audio %s\n", muted != 0 ? "muted" : "unmuted");
}

static void aiUpdateMuteToggle(void) {
  int keyCount;
  const bool* keys = SDL_GetKeyboardState(&keyCount);
  u32 muteKeyDown = keyCount > SDL_SCANCODE_M && keys[SDL_SCANCODE_M];

  if (muteKeyDown != 0 && sMuteKeyWasDown == 0 && !fhKeyboardCaptured()) {
    aiSetMuted(sMuted == 0);
  }
  sMuteKeyWasDown = muteKeyDown;
}
#endif

#if defined(__SWITCH__)
/* On Switch the SDL audio devices are resumed once when opened and never paused again: pausing and resuming
   the Switch device blocked inside SDL's device lock (sdl-dusklight-sfa-nx.patch fixes the worst case), and
   the main and audio threads both waited on it. Clearing the stream is enough to silence it. */
#define aiPauseDevice(stream) ((void)(stream))
#define aiResumeDevice(stream) ((void)(stream))
#define aiStartOpenedDevice(stream) SDL_ResumeAudioStreamDevice(stream)
#else
#define aiPauseDevice(stream) SDL_PauseAudioStreamDevice(stream)
#define aiResumeDevice(stream) SDL_ResumeAudioStreamDevice(stream)
#define aiStartOpenedDevice(stream) ((void)(stream))
#endif

static void aiOpenOutput(void) {
  SDL_AudioSpec spec;

  if (sOutputStream != NULL) {
    return;
  }
  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    fprintf(stderr, "[foxhollow] SDL audio init failed: %s\n", SDL_GetError());
    return;
  }
  SDL_zero(spec);
  spec.format = SDL_AUDIO_S16;
  spec.channels = 2;
  spec.freq = AI_DMA_SAMPLE_RATE;
  sOutputStream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
  if (sOutputStream == NULL) {
    fprintf(stderr, "[foxhollow] SDL audio output failed: %s\n", SDL_GetError());
  }
  if (sOutputStream != NULL) {
    aiStartOpenedDevice(sOutputStream);
  }
#if defined(FOXHOLLOW_DEBUG_SHORTCUTS)
  else {
    SDL_SetAudioStreamGain(sOutputStream, sMuted != 0 ? 0.0f : 1.0f);
  }
#endif
}

static void aiOpenStreamOutput(void) {
  SDL_AudioSpec spec;

  if (sStreamOutput != NULL) {
    return;
  }
  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    fprintf(stderr, "[foxhollow] SDL stream audio init failed: %s\n", SDL_GetError());
    return;
  }
  SDL_zero(spec);
  spec.format = SDL_AUDIO_S16;
  spec.channels = 2;
  spec.freq = AI_STREAM_SAMPLE_RATE;
  sStreamOutput = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
  if (sStreamOutput == NULL) {
    fprintf(stderr, "[foxhollow] SDL stream audio output failed: %s\n", SDL_GetError());
  }
  if (sStreamOutput != NULL) {
    aiStartOpenedDevice(sStreamOutput);
  }
#if defined(FOXHOLLOW_DEBUG_SHORTCUTS)
  else {
    SDL_SetAudioStreamGain(sStreamOutput, sMuted != 0 ? 0.0f : 1.0f);
  }
#endif
}

static s16 aiDecodeStreamSample(s32 bits, s32 predictorScale, s32* hist1, s32* hist2) {
  s32 hist;
  s32 sample;

  switch (predictorScale >> 4) {
  case 1:
    hist = *hist1 * 0x3c;
    break;
  case 2:
    hist = (*hist1 * 0x73) - (*hist2 * 0x34);
    break;
  case 3:
    hist = (*hist1 * 0x62) - (*hist2 * 0x37);
    break;
  default:
    hist = 0;
    break;
  }
  hist = SDL_clamp((hist + 0x20) >> 6, -0x200000, 0x1fffff);
  sample = (((s16)(bits << 12) >> (predictorScale & 0xf)) << 6) + hist;
  *hist2 = *hist1;
  *hist1 = sample;
  return (s16)SDL_clamp(sample >> 6, -0x8000, 0x7fff);
}

static void aiPumpStream(void) {
  int queued;

  if (sStreamPlayState != AI_STREAM_START || sStreamData == NULL) {
    return;
  }

  queued = sStreamOutput != NULL ? SDL_GetAudioStreamQueued(sStreamOutput) : 0;
  while (sStreamOutput != NULL && queued < AI_STREAM_QUEUE_BYTES &&
         sStreamDataOffset + AI_STREAM_BLOCK_SIZE <= sStreamDataLength) {
    const u8* block = sStreamData + sStreamDataOffset;
    s16 pcm[AI_STREAM_BLOCK_FRAMES * 2];
    int i;

    for (i = 0; i < AI_STREAM_BLOCK_FRAMES; i++) {
      const u8 sample = block[i + 4];
      s32 left = aiDecodeStreamSample(sample & 0xf, block[0], &sStreamHistLeft1, &sStreamHistLeft2);
      s32 right = aiDecodeStreamSample(sample >> 4, block[1], &sStreamHistRight1, &sStreamHistRight2);
      pcm[i * 2] = (s16)((left * (s32)sStreamVolLeft) / 255);
      pcm[i * 2 + 1] = (s16)((right * (s32)sStreamVolRight) / 255);
    }
    if (sStreamOutput != NULL) {
      SDL_PutAudioStreamData(sStreamOutput, pcm, sizeof(pcm));
      queued += sizeof(pcm);
    }
    sStreamDataOffset += AI_STREAM_BLOCK_SIZE;
    sStreamSampleCount += AI_STREAM_BLOCK_FRAMES;
    if (sStreamCallback != NULL && sStreamTrigger != 0 && sStreamSampleCount >= sStreamTrigger) {
      sStreamCallback(sStreamSampleCount);
    }
  }

  if (sStreamDataOffset + AI_STREAM_BLOCK_SIZE > sStreamDataLength) {
    sStreamPlayState = AI_STREAM_STOP;
  }
}

void AIInit(u8* stack) { (void)stack; }
void AIReset(void) { AIStopDMA(); }

int fhAIPrepareStream(void* fileInfoPtr) {
  DVDFileInfo* fileInfo = fileInfoPtr;
  u8* data;
  s32 bytesRead;

  if (fileInfo == NULL || fileInfo->length == 0 || (fileInfo->length & (AI_STREAM_BLOCK_SIZE - 1)) != 0) {
    return 0;
  }
  data = SDL_aligned_alloc(32, fileInfo->length);
  if (data == NULL) {
    fprintf(stderr, "[foxhollow] Unable to allocate %u bytes for stream audio\n", fileInfo->length);
    return 0;
  }
  bytesRead = DVDReadPrio(fileInfo, data, (s32)fileInfo->length, 0, 2);
  if (bytesRead != (s32)fileInfo->length) {
    fprintf(stderr, "[foxhollow] Unable to read stream audio (%d/%u bytes)\n", bytesRead,
            fileInfo->length);
    SDL_aligned_free(data);
    return 0;
  }

  BOOL level = OSDisableInterrupts();
  if (sStreamData != NULL) {
    SDL_aligned_free(sStreamData);
  }
  sStreamData = data;
  sStreamDataLength = fileInfo->length;
  sStreamDataOffset = 0;
  sStreamSampleCount = 0;
  sStreamHistLeft1 = 0;
  sStreamHistLeft2 = 0;
  sStreamHistRight1 = 0;
  sStreamHistRight2 = 0;
  sStreamPlayState = AI_STREAM_STOP;
  aiOpenStreamOutput();
  if (sStreamOutput != NULL) {
    aiPauseDevice(sStreamOutput);
    SDL_ClearAudioStream(sStreamOutput);
  }
  OSRestoreInterrupts(level);
  return 1;
}

AIDCallback AIRegisterDMACallback(AIDCallback callback) {
  BOOL level = OSDisableInterrupts();
  AIDCallback previous = sDmaCallback;
  sDmaCallback = callback;
  OSRestoreInterrupts(level);
  return previous;
}

/* The AI register calls below take the "interrupts off" lock because the audio thread reads the same
   state during a pump, just as the GameCube's audio interrupt could fire between two game instructions. */
void AIInitDMA(uintptr_t startAddr, u32 length) {
  BOOL level = OSDisableInterrupts();
  sDmaStartAddr = startAddr;
  sDmaLength = length;
  OSRestoreInterrupts(level);
}

void AIStartDMA(void) {
  BOOL level = OSDisableInterrupts();
  aiOpenOutput();
  sDmaRunning = 1;
  if (sOutputStream != NULL) {
    aiResumeDevice(sOutputStream);
  }
  OSRestoreInterrupts(level);
  aiStartThread();
}

void AIStopDMA(void) {
  BOOL level = OSDisableInterrupts();
  sDmaRunning = 0;
  if (sOutputStream != NULL) {
    aiPauseDevice(sOutputStream);
    SDL_ClearAudioStream(sOutputStream);
  }
  OSRestoreInterrupts(level);
}

/* One pump: top up the streamed-audio queue, then call the game's DMA callback (MusyX mixes the next block)
   until the output queue holds sQueueMs of sound. */
static void aiPumpAll(void) {
  int dmaQueued = sOutputStream != NULL ? SDL_GetAudioStreamQueued(sOutputStream) : 0;

  aiPumpStream();
  while (sDmaRunning != 0 && sOutputStream != NULL && dmaQueued < AI_DMA_QUEUE_BYTES) {
    AIDCallback callback;

    if (sOutputStream != NULL && sDmaStartAddr != 0 && sDmaLength != 0) {
      SDL_PutAudioStreamData(sOutputStream, (const void*)sDmaStartAddr, (int)sDmaLength);
      dmaQueued += sDmaLength;
    }
    callback = sDmaCallback;
    if (callback != NULL) {
      callback();
    }
  }
}

/* Crackle diagnosis (written to the profiler's reports): pumps that found the output queue empty
   (an underrun: the speaker had nothing to play), pumps that found it under 10 ms, and the longest wait
   for the "interrupts off" lock, which the game thread may hold. Reset when read. */
static SDL_AtomicInt sStatUnderruns;
static SDL_AtomicInt sStatLow;
static SDL_AtomicInt sStatMaxWaitUs;

void fhAIStats(int* underruns, int* low, int* maxWaitUs) {
  *underruns = SDL_SetAtomicInt(&sStatUnderruns, 0);
  *low = SDL_SetAtomicInt(&sStatLow, 0);
  *maxWaitUs = SDL_SetAtomicInt(&sStatMaxWaitUs, 0);
}

/* The audio thread: every 4 ms, take the lock as an interrupt handler, record the statistics, adapt the
   queue depth and pump. Its core and priority are set by perf_switch.cpp on Switch. */
static int aiThreadMain(void* unused) {
  (void)unused;
#if defined(__SWITCH__)
  melee_nx_perf_thread("SFA audio");
#endif
  while (SDL_GetAtomicInt(&sThreadStop) == 0) {
    if (SDL_GetAtomicInt(&sThreaded) != 0) {
      const Uint64 start = SDL_GetTicksNS();
      fhIrqEnterHandler();
      const int waitUs = (int)((SDL_GetTicksNS() - start) / 1000);
      if (waitUs > SDL_GetAtomicInt(&sStatMaxWaitUs)) {
        SDL_SetAtomicInt(&sStatMaxWaitUs, waitUs);
      }
      int underrun = 0;
      if (sDmaRunning != 0 && sOutputStream != NULL) {
        const int queued = SDL_GetAudioStreamQueued(sOutputStream);
        if (queued == 0) {
          SDL_AddAtomicInt(&sStatUnderruns, 1);
          underrun = 1;
        } else if (queued < AI_DMA_QUEUE_BYTES / 4) {
          SDL_AddAtomicInt(&sStatLow, 1);
        }
        aiAdaptQueue(waitUs, underrun);
      }
      aiPumpAll();
      fhIrqLeaveHandler();
    }
    SDL_DelayNS(AI_THREAD_PERIOD_NS);
  }
  return 0;
}

/* Starts the audio thread with the first DMA; if it cannot start, mixing stays on the game thread. */
static void aiStartThread(void) {
  if (sThread == NULL && SDL_GetAtomicInt(&sThreaded) != 0) {
    sThread = SDL_CreateThread(aiThreadMain, "SFA audio", NULL);
    if (sThread == NULL) {
      fprintf(stderr, "[foxhollow] audio thread failed: %s; mixing on the game thread\n", SDL_GetError());
      SDL_SetAtomicInt(&sThreaded, 0);
    }
  }
}

/* The - menu's "Audio thread" setting; turning it on starts the thread if output is already open. */
int fhAIThreaded(void) { return SDL_GetAtomicInt(&sThreaded); }

void fhAISetThreaded(int on) {
  SDL_SetAtomicInt(&sThreaded, on != 0);
  if (on != 0 && sOutputStream != NULL) {
    aiStartThread();
  }
}

/* Stops the audio thread and waits for it to end. Called before shutdown: the thread otherwise keeps
   polling sOutputStream after aurora_shutdown/SDL_Quit freed it (the crash on Restart/exit). The caller
   must not hold the "interrupts off" lock, or the thread could wait on it forever. */
void fhAIShutdown(void) {
  SDL_SetAtomicInt(&sThreadStop, 1);
  if (sThread != NULL) {
    SDL_WaitThread(sThread, NULL);
    sThread = NULL;
  }
}

/* Called once per game frame from VIWaitForRetrace: only pumps when the audio thread is not doing it. */
void fhAIPump(void) {
#if defined(FOXHOLLOW_DEBUG_SHORTCUTS)
  aiUpdateMuteToggle();
#endif
  if (SDL_GetAtomicInt(&sThreaded) != 0 && sThread != NULL) {
    return;
  }
  aiPumpAll();
}

uintptr_t fhAIGetDMAStartAddr(void) { return sDmaStartAddr; }
u32 AIGetDMAStartAddr(void) { return (u32)sDmaStartAddr; }
u32 AIGetDMALength(void) { return sDmaLength; }
AIDCallback AIGetDMACallback(void) { return sDmaCallback; }
BOOL AIGetDMAEnableFlag(void) { return sDmaRunning != 0; }
u32 AIGetDMABytesLeft(void) { return sDmaLength; }
BOOL AICheckInit(void) { return TRUE; }
void AISetDSPSampleRate(u32 rate) { sDspSampleRate = rate; }
u32 AIGetDSPSampleRate(void) { return sDspSampleRate; }
AISCallback AIRegisterStreamCallback(AISCallback callback) {
  AISCallback previous = sStreamCallback;
  sStreamCallback = callback;
  return previous;
}
u32 AIGetStreamSampleCount(void) { return sStreamSampleCount; }
void AIResetStreamSampleCount(void) { sStreamSampleCount = 0; }
void AISetStreamTrigger(u32 trigger) { sStreamTrigger = trigger; }
u32 AIGetStreamTrigger(void) { return sStreamTrigger; }
void AISetStreamPlayState(u32 state) {
  BOOL level = OSDisableInterrupts();
  sStreamPlayState = state;
  if (sStreamOutput != NULL) {
    if (state == AI_STREAM_START) {
      aiResumeDevice(sStreamOutput);
    } else {
      aiPauseDevice(sStreamOutput);
      SDL_ClearAudioStream(sStreamOutput);
    }
  }
  OSRestoreInterrupts(level);
}
u32 AIGetStreamPlayState(void) { return sStreamPlayState; }
void AISetStreamSampleRate(u32 rate) { (void)rate; }
u32 AIGetStreamSampleRate(void) { return AI_SAMPLERATE_48KHZ; }
void AISetStreamVolLeft(u8 vol) { sStreamVolLeft = vol; }
void AISetStreamVolRight(u8 vol) { sStreamVolRight = vol; }
u8 AIGetStreamVolLeft(void) { return (u8)sStreamVolLeft; }
u8 AIGetStreamVolRight(void) { return (u8)sStreamVolRight; }
