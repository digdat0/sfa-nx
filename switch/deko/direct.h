// SFA-NX direct renderer (phase 1, step 1). With the − menu's "Renderer: Direct", Aurora's render
// worker records each render pass and each GX or clear draw straight into the native command list
// of the frame's encoder (one closure per draw, its pipeline, bind groups and index buffer resolved
// once). The WebGPU path it replaces went through Dawn's render pass (labels, usage tracking, heap
// allocations) and five to eight wrapped wgpu calls per draw, each taking the adapter lock and
// looking its objects up again. Replay is the same as the WebGPU path's (kd::draw).
//
// Used from extern/aurora/lib/gfx/encoding.cpp, on the render worker only. A pass with anything
// else in it (custom draws, debug markers) takes the WebGPU path, so both can run in one frame.
#pragma once

#include <cstddef>
#include <cstdint>
#include <webgpu/webgpu.h>

extern "C" {
void aurora_set_direct_renderer(bool on);
bool aurora_direct_renderer(void);
}

namespace kd::direct {

// One render pass: colour and optional depth target, and whether each is cleared or loaded.
struct PassDesc {
  WGPUTextureView color = nullptr;
  WGPUTextureView depth = nullptr;
  bool clearColor = false;
  float clearValue[4]{};
  bool clearDepth = false;
  float depthClearValue = 1.f;
  const char* label = "";
};

// One GX draw as Aurora's gx::render would issue it: the pipeline (plus an optional depth prepass
// pipeline drawn first with the same state), the texture group, the offset of its uniforms in the
// pass's uniform buffer, its index range and immediate data, and the blend constant for destination
// alpha.
struct GxDraw {
  WGPURenderPipeline main = nullptr;
  WGPURenderPipeline prepass = nullptr;
  WGPUBindGroup textures = nullptr;  // null: keep the texture group bound before (as gx::render)
  uint32_t uniformOffset = 0;
  uint64_t indexOffset = 0;
  const void* immediates = nullptr;
  uint32_t immediateBytes = 0;
  uint32_t vtxCount = 0, indexCount = 0, instanceCount = 1;
  bool blendConstant = false;
  float blendAlpha = 0.f;
};

// A clear drawn as a full-screen triangle (Aurora's clear::render) with its viewport and scissor.
struct ClearDraw {
  WGPURenderPipeline pipeline = nullptr;
  float color[4]{};
  float viewport[6]{};  // x, y, w, h, min depth, max depth
  uint32_t scissor[4]{};
};

// The pass's shared groups: 0 static, 1 uniforms (dynamic offset per draw), 2 the empty texture
// group, and the index buffer.
struct PassResources {
  WGPUBindGroup staticGroup = nullptr;
  WGPUBindGroup uniformGroup = nullptr;
  WGPUBindGroup emptyTextures = nullptr;
  WGPUBuffer indexBuffer = nullptr;
};

void begin_pass(WGPUCommandEncoder encoder, const PassDesc& desc, const PassResources& res);
void set_viewport(float x, float y, float w, float h, float minDepth, float maxDepth);
void set_scissor(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
void gx_draw(const GxDraw& draw);
void clear_draw(const ClearDraw& draw);
void end_pass();

}  // namespace kd::direct
