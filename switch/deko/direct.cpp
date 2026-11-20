// SFA-NX direct renderer, phase 1 step 1: see direct.h. Records into the frame encoder's native
// command list (kd::Commands) in the order Aurora encodes, so passes recorded here and passes
// recorded through the WebGPU entry points interleave correctly within one encoder.
#include "direct.h"

#include "backend_internal.h"

#include <atomic>

namespace {
// The - menu's Renderer setting; on by default. Read by encoding.cpp for each pass.
std::atomic_bool g_direct{true};

// The pass being recorded (render worker only).
struct DirectPass {
  std::shared_ptr<kd::Commands> commands;
  std::shared_ptr<kd::Group> uniforms;
  std::shared_ptr<kd::Buffer> index;
  wgpu::TextureView target;
  std::string label;
} g_pass;

// The adapter's record for a bind group handle, or null if the handle is null or already released.
std::shared_ptr<kd::Group> group_or_null(WGPUBindGroup h) {
  if (!h)
    return nullptr;
  auto it = kd::groups.find(h);
  return it == kd::groups.end() ? nullptr : it->second;
}
}  // namespace

extern "C" void aurora_set_direct_renderer(bool on) { g_direct.store(on, std::memory_order_relaxed); }
extern "C" bool aurora_direct_renderer(void) { return g_direct.load(std::memory_order_relaxed); }

namespace kd::direct {

// Starts recording a pass into the encoder's command list. The shared_ptrs captured here keep the
// targets, uniform buffer and index buffer alive until the frame is replayed.
void begin_pass(WGPUCommandEncoder encoder, const PassDesc& desc, const PassResources& res) {
  Lock l(mutex);
  if (g_pass.commands)
    swdk::fatal("direct: pass begun inside a pass");
  g_pass.commands = required(encoders, encoder, __func__);
  g_pass.uniforms = group_or_null(res.uniformGroup);
  auto ib = buffers.find(res.indexBuffer);
  g_pass.index = ib == buffers.end() ? nullptr : ib->second;
  g_pass.target = keep(desc.color);
  g_pass.label = desc.label ? desc.label : "";
  auto depth = desc.depth ? keep(desc.depth) : wgpu::TextureView{};
  const std::array<float, 4> c{desc.clearValue[0], desc.clearValue[1], desc.clearValue[2], desc.clearValue[3]};
  g_pass.commands->emplace_back([view = g_pass.target, depth, c, clear = desc.clearColor,
                                 clearDepth = desc.clearDepth && desc.depth, z = desc.depthClearValue] {
    begin_pass_native(view, depth, clear, c, clearDepth, z);
  });
  // encoding.cpp render_pass: group 0 and the empty texture group for the whole pass.
  g_pass.commands->emplace_back([s = group_or_null(res.staticGroup), e = group_or_null(res.emptyTextures)] {
    state.groups[0] = {s, {}};
    state.groups[2] = {e, {}};
  });
}

void set_viewport(float x, float y, float w, float h, float minDepth, float maxDepth) {
  g_pass.commands->emplace_back([v = DkViewport{x, y, w, h, minDepth, maxDepth}] {
    state.viewport = v;
    dkCmdBufSetViewports(swdk::R.cmd, 0, &v, 1);
  });
}

void set_scissor(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
  g_pass.commands->emplace_back([s = DkScissor{x, y, w, h}] {
    state.scissor = s;
    dkCmdBufSetScissors(swdk::R.cmd, 0, &s, 1);
  });
}

// Records one GX draw. Objects are looked up once here, under `mutex`; the recorded closure then
// replays without any lookup. A draw whose pipeline or textures were released in the meantime is
// dropped, the same as a draw whose pipeline is still compiling.
void gx_draw(const GxDraw& d) {
  if (d.immediateBytes > kImmediateBytes)
    swdk::fatal("direct: immediate data %u bytes (limit %u)", d.immediateBytes, kImmediateBytes);
  std::shared_ptr<Pipeline> main, pre;
  std::shared_ptr<Group> tex;
  {
    Lock l(mutex);
    auto it = pipelines.find(d.main);
    if (it == pipelines.end())
      return;  // released since Aurora resolved it: skip, as for a pipeline still compiling
    main = it->second;
    if (d.prepass) {
      auto pit = pipelines.find(d.prepass);
      if (pit == pipelines.end())
        return;
      pre = pit->second;
    }
    if (d.textures && !(tex = group_or_null(d.textures)))
      return;
  }
  std::array<uint8_t, kImmediateBytes> imm{};
  if (d.immediateBytes)
    std::memcpy(imm.data(), d.immediates, d.immediateBytes);
  g_pass.commands->emplace_back([main = std::move(main), pre = std::move(pre), tex = std::move(tex),
                                 uniforms = g_pass.uniforms, index = g_pass.index, imm,
                                 immBytes = d.immediateBytes, uniformOffset = d.uniformOffset,
                                 indexOffset = d.indexOffset, vtx = d.vtxCount, idx = d.indexCount,
                                 instances = d.instanceCount, blend = d.blendConstant, alpha = d.blendAlpha] {
    // Same order and effect as gx::render through the WebGPU entry points.
    std::memcpy(state.immediates.data(), imm.data(), immBytes);
    state.immediatesDirty = true;
    auto& u = state.groups[1];
    if (u.group != uniforms)
      u.group = uniforms;
    u.offsets[0] = uniformOffset;
    if (tex)
      state.groups[2] = {tex, {}};
    if (state.index != index)
      state.index = index;
    state.indexOffset = indexOffset;
    state.indexFormat = DkIdxFormat_Uint16;
    if (blend)
      dkCmdBufSetBlendConst(swdk::R.cmd, 0.f, 0.f, 0.f, alpha);
    const auto issue = [&] {
      if (idx == 0)
        draw(false, vtx, instances, 0, 0, 0);
      else
        draw(true, idx, instances, 0, 0, 0);
    };
    if (pre) {
      state.pipeline = pre;
      issue();
    }
    state.pipeline = main;
    issue();
  });
}

// Records a clear draw (see ClearDraw).
void clear_draw(const ClearDraw& d) {
  std::shared_ptr<Pipeline> p;
  {
    Lock l(mutex);
    auto it = pipelines.find(d.pipeline);
    if (it == pipelines.end())
      return;
    p = it->second;
  }
  g_pass.commands->emplace_back([p = std::move(p), c = std::array<float, 4>{d.color[0], d.color[1], d.color[2], d.color[3]},
                                 v = DkViewport{d.viewport[0], d.viewport[1], d.viewport[2], d.viewport[3],
                                                d.viewport[4], d.viewport[5]},
                                 s = DkScissor{d.scissor[0], d.scissor[1], d.scissor[2], d.scissor[3]}] {
    // clear::render: pipeline, blend constant, full-target viewport at the clear depth, scissor, 3 vertices.
    state.pipeline = p;
    dkCmdBufSetBlendConst(swdk::R.cmd, c[0], c[1], c[2], c[3]);
    state.viewport = v;
    dkCmdBufSetViewports(swdk::R.cmd, 0, &v, 1);
    state.scissor = s;
    dkCmdBufSetScissors(swdk::R.cmd, 0, &s, 1);
    draw(false, 3, 1, 0, 0, 0);
  });
}

// Records the end of the pass (which also stamps GPU timing) and forgets the pass.
void end_pass() {
  g_pass.commands->emplace_back(
      [target = std::move(g_pass.target), label = std::move(g_pass.label)] { end_pass_native(target, label); });
  Lock l(mutex);  // the shared_ptrs below are also referenced from maps other threads change
  g_pass = {};
}

}  // namespace kd::direct
