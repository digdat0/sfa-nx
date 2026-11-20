// Per-pass GPU timing, active while the - menu's Profiler is on.
//
// Every kEveryFrames frames one frame is sampled: the command buffer writes a GPU timestamp
// (dkCmdBufReportCounter) at the start and end of every render pass and before the present. When the
// frame's fence has signalled, the stamps are read back and logged as "gpu time #N" lines: the total from
// the first pass to the present, the time inside passes versus between them, and every interval of
// 0.25 ms or more with its pass label, target and draw count. Only one frame is in flight at a time, so
// sampling never stalls the CPU.
#include "backend_internal.h"

extern "C" int sfaProfEnabled(void);

namespace kd {
namespace {

// Up to 1024 stamps per sampled frame; kTickNs converts Maxwell timestamp ticks (the 19.2 MHz counter
// reported in 625/384 ns units) to nanoseconds.
constexpr uint32_t kMarks = 1024;
constexpr uint64_t kEveryFrames = 120;
constexpr double kTickNs = 625.0 / 384.0;

enum class Kind : uint8_t { PassBegin, PassEnd, Present };

// CPU-side description of one stamp; the 16-byte GPU report at g_block + i * 16 holds its time.
struct Mark {
  Kind kind;
  uint32_t width, height;
  uint64_t draws;
  char label[40];
  char target[40];
};

// Idle: waiting for the next sample frame. Sampling: stamps are being recorded this frame. Pending: the
// frame was submitted and its stamps are read once its fence signals.
enum class Phase { Idle, Sampling, Pending };

swdk::Block g_block;
Mark g_marks[kMarks];
uint32_t g_count = 0;
Phase g_phase = Phase::Idle;
uint64_t g_nextFrame = 0;
uint64_t g_sampleFrame = 0;
DkFence g_fence{};
uint32_t g_samples = 0;

void copy_text(char (&out)[40], const char *text) {
  std::snprintf(out, sizeof out, "%s", text ? text : "");
}

// Timestamp of stamp i (second word of the 16-byte counter report), read from CPU-visible memory.
uint64_t stamp(uint32_t i) {
  return reinterpret_cast<volatile const uint64_t *>(g_block.cpu)[i * 2 + 1];
}

// Logs a summary line and one line per interval worth showing (see the top of the file).
void report() {
  if (g_count < 2)
    return;
  const uint64_t first = stamp(0);
  const double spanMs = double(stamp(g_count - 1) - first) * kTickNs / 1e6;
  double passMs = 0, gapMs = 0;
  uint32_t passes = 0;
  uint64_t draws = 0;
  for (uint32_t i = 1; i < g_count; ++i) {
    const double ms = double(stamp(i) - stamp(i - 1)) * kTickNs / 1e6;
    if (g_marks[i].kind == Kind::PassEnd) {
      passMs += ms;
      ++passes;
      draws += g_marks[i].draws;
    } else {
      gapMs += ms;
    }
  }
  ++g_samples;
  swdk::dklog("gpu time #%u frame %llu: %.2f ms from first pass to present; %u passes %.2f ms (%llu draws), between "
              "passes %.2f ms",
              g_samples, (unsigned long long)g_sampleFrame, spanMs, passes, passMs, (unsigned long long)draws, gapMs);
  for (uint32_t i = 1; i < g_count; ++i) {
    const double ms = double(stamp(i) - stamp(i - 1)) * kTickNs / 1e6;
    const Mark &m = g_marks[i];
    if (ms < 0.25)
      continue;
    if (m.kind == Kind::PassEnd) {
      swdk::dklog("gpu time #%u   %6.2f ms pass %u \"%s\" -> %s %ux%u, %llu draws", g_samples, ms, i, m.label,
                  m.target, m.width, m.height, (unsigned long long)m.draws);
    } else {
      swdk::dklog("gpu time #%u   %6.2f ms before %s %u (after \"%s\")", g_samples, ms,
                  m.kind == Kind::Present ? "present" : "pass", i, g_marks[i - 1].label);
    }
  }
}

// Records a stamp if this frame is being sampled. Also finishes a pending sample and decides when the
// next one starts (only at a pass begin, so a sampled frame starts with a full pass).
Mark *next_mark(Kind kind) {
  if (g_phase == Phase::Pending && dkFenceWait(&g_fence, 0) == DkResult_Success) {
    report();
    g_phase = Phase::Idle;
    g_nextFrame = swdk::R.frame + kEveryFrames;
  }
  if (g_phase == Phase::Idle) {
    if (kind != Kind::PassBegin || !sfaProfEnabled() || swdk::R.frame < g_nextFrame)
      return nullptr;
    if (!g_block) {
      g_block = swdk::block_create(kMarks * 16, "GPU pass timing");
      if (!g_block)
        return nullptr;
    }
    g_phase = Phase::Sampling;
    g_sampleFrame = swdk::R.frame;
    g_count = 0;
  }
  if (g_phase != Phase::Sampling || g_count >= kMarks - 1 && kind != Kind::Present)
    return nullptr;
  Mark &m = g_marks[g_count];
  m = {};
  m.kind = kind;
  dkCmdBufReportCounter(swdk::R.cmd, DkCounter_Timestamp, g_block.gpu + g_count * 16);
  ++g_count;
  return &m;
}

}  // namespace

// Hooks called from the command replay (backend_commands.cpp, backend_present.cpp). gpu_timing_sampling
// tells end_pass_native whether the extra barrier for exact pass boundaries is needed this frame.
bool gpu_timing_sampling() { return g_phase == Phase::Sampling; }

void gpu_timing_pass_begin() { next_mark(Kind::PassBegin); }

void gpu_timing_pass_end(const char *label, const char *target, uint32_t width, uint32_t height, uint64_t draws) {
  if (Mark *m = next_mark(Kind::PassEnd)) {
    copy_text(m->label, label);
    copy_text(m->target, target);
    m->width = width;
    m->height = height;
    m->draws = draws;
  }
}

// Last stamp of a sampled frame: a full barrier first so all passes have finished, then the fence the
// result is read back after.
void gpu_timing_present() {
  if (g_phase == Phase::Sampling) {
    barrier();
    if (Mark *m = next_mark(Kind::Present))
      copy_text(m->label, "present");
    dkCmdBufSignalFence(swdk::R.cmd, &g_fence, true);
    g_phase = Phase::Pending;
  } else {
    next_mark(Kind::Present);
  }
}

}  // namespace kd
