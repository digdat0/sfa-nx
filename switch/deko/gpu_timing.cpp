// Per-pass GPU timing, active while the - menu's Profiler is on.
//
// Every kEveryFrames frames one frame is sampled: the command buffer writes a GPU timestamp
// (dkCmdBufReportCounter) at the start and end of every render pass and before the present. When the
// frame's fence has signalled, the stamps are read back and logged as "gpu time #N" lines: the total from
// the first pass to the present, the time inside passes versus between them, and every interval of
// 0.25 ms or more with its pass label, target and draw count. Only one frame is in flight at a time, so
// sampling never stalls the CPU.
//
// SFA-NX: every kDrawEvery-th sampled frame is also a draw probe frame. Each draw is bracketed by a
// primitives barrier (the GPU finishes all work before it) and an end-of-pipe timestamp on both sides, so
// the difference is that draw's cost alone. The report ("gpu draws #N" lines) lists the kTopDraws most
// expensive draws with what identifies them (pass, shaders, size, blend/depth state, viewport, first
// texture) and the kTopShaders fragment shaders with the most time. The barriers slow the probe frame and
// inflate its pass times, so its "gpu time" summary is tagged "(draw probe)".
#include "backend_internal.h"

#include <algorithm> // SFA-NX: std::sort (draw probe report)
#include <cstring> // SFA-NX: strncmp (always-listed passes in report())

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

// SFA-NX: draw probe (see the top of the file). kDraws draws per probe frame at most, two stamps each.
constexpr uint32_t kDrawEvery = 5;   // every 5th sample: one probe frame per 600 frames (10 s at 60 fps)
constexpr uint32_t kDraws = 2048;
constexpr uint32_t kTopDraws = 20;
constexpr uint32_t kTopShaders = 8;

// One probed draw: what identifies it, copied when recorded (the texture may be gone by the report).
struct DrawRec {
  GpuDrawInfo info;     // info.tex is cleared: use the copied fields below
  uint32_t passMark;    // index of its pass's PassBegin mark (the PassEnd after it has the label)
  uint32_t texW, texH, texFormat;
  char tex[32];
};

swdk::Block g_drawBlock;        // 2 x 16-byte counter reports per draw: before, after
DrawRec g_drawRecs[kDraws];
uint32_t g_drawCount = 0;
bool g_drawMode = false;        // this sample is a probe frame
bool g_drawOpen = false;        // a draw's first stamp is written, its second not yet
uint32_t g_started = 0;         // samples started (picks the probe frames)
uint32_t g_lastPassBegin = 0;   // mark index of the current pass's PassBegin

// Draw stamp i (0 before, 1 after, per draw) of the probe frame.
uint64_t draw_stamp(uint32_t i) {
  return reinterpret_cast<volatile const uint64_t *>(g_drawBlock.cpu)[i * 2 + 1];
}

void copy_text(char (&out)[40], const char *text) {
  std::snprintf(out, sizeof out, "%s", text ? text : "");
}

// Timestamp of stamp i (second word of the 16-byte counter report), read from CPU-visible memory.
uint64_t stamp(uint32_t i) {
  return reinterpret_cast<volatile const uint64_t *>(g_block.cpu)[i * 2 + 1];
}

// SFA-NX: the draw probe's report: a summary, the most expensive draws, then time per fragment shader.
void report_draws() {
  static double ms[kDraws];
  static uint16_t order[kDraws];
  double total = 0;
  for (uint32_t i = 0; i < g_drawCount; ++i) {
    const uint64_t a = draw_stamp(i * 2), b = draw_stamp(i * 2 + 1);
    ms[i] = b > a ? double(b - a) * kTickNs / 1e6 : 0.0;
    total += ms[i];
    order[i] = uint16_t(i);
  }
  std::sort(order, order + g_drawCount, [](uint16_t x, uint16_t y) { return ms[x] > ms[y]; });
  swdk::dklog("gpu draws #%u frame %llu: %u draws timed one at a time, %.2f ms in all (%s)", g_samples,
              (unsigned long long)g_sampleFrame, g_drawCount, total,
              g_drawCount >= kDraws ? "draw limit reached, later draws not timed" : "every draw");
  for (uint32_t k = 0; k < g_drawCount && k < kTopDraws; ++k) {
    const DrawRec &r = g_drawRecs[order[k]];
    const GpuDrawInfo &d = r.info;
    const uint32_t end = r.passMark + 1;
    const char *pass = end < g_count && g_marks[end].kind == Kind::PassEnd ? g_marks[end].label : "?";
    swdk::dklog("gpu draws #%u  %6.3f ms  draw %u in pass %u \"%s\": %u %s x%u, %s, z %s/%s, viewport %ux%u, "
                "fs %016llx vs %016llx, tex %s %ux%u fmt %u",
                g_samples, ms[order[k]], unsigned(order[k]), end, pass, d.count, d.indexed ? "indices" : "vertices",
                d.instances, d.blend ? "blended" : "opaque",
                d.depthTest ? "test" : "no test", d.depthWrite ? "write" : "no write", d.viewportW, d.viewportH,
                (unsigned long long)d.fsHash, (unsigned long long)d.vsHash, r.tex[0] ? r.tex : "-", r.texW, r.texH,
                r.texFormat);
  }
  // Time per fragment shader: unique hashes in first-seen order, then the largest kTopShaders.
  struct Shader {
    uint64_t fs;
    uint32_t draws;
    double ms;
  };
  static Shader shaders[256];
  uint32_t n = 0;
  for (uint32_t i = 0; i < g_drawCount; ++i) {
    const uint64_t fs = g_drawRecs[i].info.fsHash;
    uint32_t j = 0;
    while (j < n && shaders[j].fs != fs)
      ++j;
    if (j == n) {
      if (n == 256)
        continue; // table full: rare, the shader is left out of this summary
      shaders[n++] = {fs, 0, 0.0};
    }
    ++shaders[j].draws;
    shaders[j].ms += ms[i];
  }
  std::sort(shaders, shaders + n, [](const Shader &x, const Shader &y) { return x.ms > y.ms; });
  for (uint32_t k = 0; k < n && k < kTopShaders; ++k)
    swdk::dklog("gpu draws #%u  fs %016llx: %u draws, %.2f ms", g_samples, (unsigned long long)shaders[k].fs,
                shaders[k].draws, shaders[k].ms);
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
  // SFA-NX: a probe frame's times include its per-draw waits (tagged so they are not compared with others)
  swdk::dklog("gpu time #%u frame %llu: %.2f ms from first pass to present; %u passes %.2f ms (%llu draws), between "
              "passes %.2f ms%s",
              g_samples, (unsigned long long)g_sampleFrame, spanMs, passes, passMs, (unsigned long long)draws, gapMs,
              g_drawMode ? " (draw probe)" : "");
  for (uint32_t i = 1; i < g_count; ++i) {
    const double ms = double(stamp(i) - stamp(i - 1)) * kTickNs / 1e6;
    const Mark &m = g_marks[i];
    // SFA-NX: EFB copy conversions ("TexCopyConv <format>", the split B8 shadow blur) and the ImGui pass are
    // listed at any cost, so their timing can be compared between builds even when they are cheap
    const bool alwaysShown = m.kind == Kind::PassEnd && (std::strncmp(m.label, "TexCopyConv", 11) == 0 ||
                                                         std::strncmp(m.label, "ImGui", 5) == 0);
    if (ms < 0.25 && !alwaysShown)
      continue;
    if (m.kind == Kind::PassEnd) {
      swdk::dklog("gpu time #%u   %6.2f ms pass %u \"%s\" -> %s %ux%u, %llu draws", g_samples, ms, i, m.label,
                  m.target, m.width, m.height, (unsigned long long)m.draws);
    } else {
      swdk::dklog("gpu time #%u   %6.2f ms before %s %u (after \"%s\")", g_samples, ms,
                  m.kind == Kind::Present ? "present" : "pass", i, g_marks[i - 1].label);
    }
  }
  if (g_drawMode) // SFA-NX: a probe frame's per-draw lines follow its pass lines
    report_draws();
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
    // SFA-NX: every kDrawEvery-th sample is a draw probe frame (its stamp block made on first use)
    g_drawMode = ++g_started % kDrawEvery == 0;
    g_drawCount = 0;
    g_drawOpen = false;
    if (g_drawMode && !g_drawBlock) {
      g_drawBlock = swdk::block_create(kDraws * 2 * 16, "GPU draw probe");
      g_drawMode = bool(g_drawBlock);
    }
  }
  if (g_phase != Phase::Sampling || g_count >= kMarks - 1 && kind != Kind::Present)
    return nullptr;
  if (kind == Kind::PassBegin) // SFA-NX: the draw probe files each draw under this pass
    g_lastPassBegin = g_count;
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

// SFA-NX: draw probe hooks (kd::draw). Begin: wait for all earlier GPU work, then stamp; end: wait for the
// draw, stamp, and keep what identifies it. Both do nothing outside a probe frame or past kDraws draws.
bool gpu_timing_draw_sampling() { return g_phase == Phase::Sampling && g_drawMode; }

void gpu_timing_draw_begin() {
  if (!gpu_timing_draw_sampling() || g_drawCount >= kDraws)
    return;
  dkCmdBufBarrier(swdk::R.cmd, DkBarrier_Primitives, 0);
  dkCmdBufReportCounter(swdk::R.cmd, DkCounter_Timestamp, g_drawBlock.gpu + g_drawCount * 2 * 16);
  g_drawOpen = true;
}

void gpu_timing_draw_end(const GpuDrawInfo &info) {
  if (!g_drawOpen)
    return;
  g_drawOpen = false;
  dkCmdBufBarrier(swdk::R.cmd, DkBarrier_Primitives, 0);
  dkCmdBufReportCounter(swdk::R.cmd, DkCounter_Timestamp, g_drawBlock.gpu + (g_drawCount * 2 + 1) * 16);
  DrawRec &r = g_drawRecs[g_drawCount++];
  r.info = info;
  r.info.tex = nullptr;
  r.passMark = g_lastPassBegin;
  r.texW = info.tex ? info.tex->width : 0;
  r.texH = info.tex ? info.tex->height : 0;
  r.texFormat = info.tex ? unsigned(info.tex->wformat) : 0;
  std::snprintf(r.tex, sizeof r.tex, "%s", info.tex ? info.tex->label.c_str() : "");
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
