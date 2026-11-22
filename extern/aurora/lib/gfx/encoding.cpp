#include "encoding.hpp"

#include "frame.hpp"

#include "clear.hpp"
#include "depth_peek.hpp"
#include "pipeline_cache.hpp"
#include "tex_copy_conv.hpp"
#include "tex_palette_conv.hpp"
#include "../gx/gx.hpp"
#include "../gx/pipeline.hpp"
#include "../gx/viewport_depth.hpp"
#ifdef AURORA_ENABLE_RMLUI
#include "../rmlui/pipeline.hpp"
#endif
#include "../webgpu/gpu.hpp"
#include "../webgpu/gpu_prof.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <tracy/Tracy.hpp>

#ifdef AURORA_GFX_DEKO3D
#include "direct.h" // switch/deko
#include "resource_cache.hpp"
#endif

namespace aurora::gfx {
using namespace detail;
using webgpu::g_device;
using webgpu::g_queue;

namespace {
constexpr Module Log{"aurora::gfx"};
WGPURenderPipeline g_currentPipeline = nullptr;

void apply_viewport(const wgpu::RenderPassEncoder& pass, const Viewport& vp) {
  const auto depth = gx::viewport_depth(vp.znear, vp.zfar, gx::UseReversedZ);
  pass.SetViewport(vp.left, vp.top, vp.width, vp.height, depth.min, depth.max);
}

void apply_scissor(const wgpu::RenderPassEncoder& pass, const ClipRect& sc, const wgpu::Extent3D& size) {
  const auto x = std::clamp(static_cast<uint32_t>(sc.x), 0u, size.width);
  const auto y = std::clamp(static_cast<uint32_t>(sc.y), 0u, size.height);
  const auto w = std::clamp(static_cast<uint32_t>(sc.width), 0u, size.width - x);
  const auto h = std::clamp(static_cast<uint32_t>(sc.height), 0u, size.height - y);
  pass.SetScissorRect(x, y, w, h);
}

DrawContext make_draw_context(const RenderPass& passInfo) {
  auto& res = resources();
  return {
      .device = g_device,
      .queue = g_queue,
      .vertexBuffer = res.vertexBuffer,
      .indexBuffer = res.indexBuffer,
      .uniformBuffer = res.uniformBuffer,
      .storageBuffer = res.storageBuffer,
      .layout = passInfo.target_layout(),
  };
}

void render_custom_draw(const CustomDrawCommand& draw, const wgpu::RenderPassEncoder& pass,
                        const RenderPass& passInfo) {
  const auto drawType = find_runtime_draw_type(draw.type);
  if (!drawType) {
    // Unregistered between record and replay; the command is a no-op.
    return;
  }

  const auto context = make_draw_context(passInfo);
  drawType->draw(context, pass, draw.payload.data(), draw.payloadSize, drawType->userdata);
}

void execute_encoder_task(wgpu::CommandEncoder& cmd, FramePacket& frame, const EncoderTask& task) {
  const auto taskType = find_runtime_encoder_task_type(task.type);
  if (!taskType) {
    // Unregistered between record and encode; the task is a no-op.
    return;
  }

  auto& res = resources();
  const EncoderTaskContext context{
      .device = g_device,
      .queue = g_queue,
      .vertexBuffer = res.vertexBuffer,
      .indexBuffer = res.indexBuffer,
      .uniformBuffer = res.uniformBuffer,
      .storageBuffer = res.storageBuffer,
  };
  taskType->callback(context, cmd, task.payload.data(), task.payloadSize, taskType->userdata);
  if (taskType->afterSubmit != nullptr) {
    const auto payload = task.payload;
    const auto payloadSize = task.payloadSize;
    const auto callback = taskType->afterSubmit;
    const auto userdata = taskType->userdata;
    frame.afterSubmitCallbacks.emplace_back([payload, payloadSize, callback, userdata] {
      const EncoderTaskCompletionContext completionContext{
          .device = g_device,
          .queue = g_queue,
      };
      callback(completionContext, payload.data(), payloadSize, userdata);
    });
  }
}

void render_pass(const wgpu::RenderPassEncoder& pass, FramePacket& frame, RenderPass& passInfo) {
  ZoneScoped;
  g_currentPipeline = nullptr;
#ifdef AURORA_GFX_DEBUG_GROUPS
  std::vector<std::string> lastDebugGroupStack;
#endif
  Viewport currentViewport{};
  ClipRect currentScissor{};
  bool hasViewport = false;
  bool hasScissor = false;

  // Bind bind group for the whole pass
  pass.SetBindGroup(0, resources().staticBindGroup);
  pass.SetBindGroup(2, gx::g_emptyTextureBindGroup);

  for (auto& cmd : passInfo.commands) {
#ifdef AURORA_GFX_DEBUG_GROUPS
    {
      size_t firstDiff = lastDebugGroupStack.size();
      for (size_t i = 0; i < lastDebugGroupStack.size(); ++i) {
        if (i >= cmd.debugGroupStack.size() || cmd.debugGroupStack[i] != lastDebugGroupStack[i]) {
          firstDiff = i;
          break;
        }
      }
      for (size_t i = firstDiff; i < lastDebugGroupStack.size(); ++i) {
        pass.PopDebugGroup();
      }
      for (size_t i = firstDiff; i < cmd.debugGroupStack.size(); ++i) {
        pass.PushDebugGroup(cmd.debugGroupStack[i].c_str());
      }
      lastDebugGroupStack = cmd.debugGroupStack;
    }
#endif
    switch (cmd.type) {
    case CommandType::SetViewport: {
      const auto& vp = cmd.data.setViewport;
      apply_viewport(pass, vp);
      currentViewport = vp;
      hasViewport = true;
    } break;
    case CommandType::SetScissor: {
      const auto& sc = cmd.data.setScissor;
      apply_scissor(pass, sc, passInfo.colorAttachments[SceneColorAttachmentIndex].size);
      currentScissor = sc;
      hasScissor = true;
    } break;
    case CommandType::Draw: {
      auto& draw = cmd.data.draw;
      if (draw.encoder != nullptr) {
        draw.encoder(draw.payload.data(), pass, passInfo);
      }
    } break;
    case CommandType::CustomDraw: {
      render_custom_draw(cmd.data.customDraw, pass, passInfo);
      g_currentPipeline = nullptr;
      pass.SetBindGroup(0, resources().staticBindGroup);
      pass.SetBindGroup(2, gx::g_emptyTextureBindGroup);
      if (hasViewport) {
        apply_viewport(pass, currentViewport);
      }
      if (hasScissor) {
        apply_scissor(pass, currentScissor, passInfo.colorAttachments[SceneColorAttachmentIndex].size);
      }
    } break;
    case CommandType::DebugMarker: {
#if defined(AURORA_GFX_DEBUG_GROUPS)
      pass.InsertDebugMarker(wgpu::StringView(frame.debugMarkers[cmd.data.debugMarkerIndex]));
#endif
    } break;
    }
  }

#ifdef AURORA_GFX_DEBUG_GROUPS
  for (size_t i = 0; i < lastDebugGroupStack.size(); ++i) {
    pass.PopDebugGroup();
  }
#endif
}

#ifdef AURORA_GFX_DEKO3D
// SFA-NX direct renderer (switch/deko/direct.h): a pass of GX draws, clear draws, viewports and
// scissors goes straight to the native command list; anything else takes the WebGPU path.
bool direct_eligible(const RenderPass& passInfo) {
  if (passInfo.colorAttachmentCount == 0 || !passInfo.colorAttachments[0].view) {
    return false;
  }
  const auto gxEncoder = gx_draw_encoder();
  const auto clearEncoder = clear_draw_encoder();
  for (const auto& cmd : passInfo.commands) {
    switch (cmd.type) {
    case CommandType::SetViewport:
    case CommandType::SetScissor:
      break;
    case CommandType::Draw:
      if (cmd.data.draw.encoder != gxEncoder && cmd.data.draw.encoder != clearEncoder &&
          cmd.data.draw.encoder != nullptr) {
        return false;
      }
      break;
    default:
      return false;
    }
  }
  return true;
}

void render_pass_direct(wgpu::CommandEncoder& cmd, RenderPass& passInfo) {
  const auto& c0 = passInfo.colorAttachments[0];
  const auto colorLoad =
      c0.loadOp != wgpu::LoadOp::Undefined ? c0.loadOp : (c0.clear ? wgpu::LoadOp::Clear : wgpu::LoadOp::Load);
  kd::direct::PassDesc desc{
      .color = c0.view.Get(),
      .depth = passInfo.depthStencilView.Get(),
      .clearColor = colorLoad == wgpu::LoadOp::Clear,
      .clearValue = {c0.clearValue.x(), c0.clearValue.y(), c0.clearValue.z(), c0.clearValue.w()},
      .clearDepth = passInfo.depthStencilView && passInfo.hasDepth &&
                    (passInfo.depthLoadOp != wgpu::LoadOp::Undefined ? passInfo.depthLoadOp
                                                                     : (passInfo.clearDepth ? wgpu::LoadOp::Clear
                                                                                            : wgpu::LoadOp::Load)) ==
                        wgpu::LoadOp::Clear,
      .depthClearValue = passInfo.clearDepthValue,
      .label = passInfo.label.c_str(),
  };
  const auto& res = resources();
  kd::direct::begin_pass(cmd.Get(), desc,
                         {
                             .staticGroup = res.staticBindGroup.Get(),
                             .uniformGroup = res.uniformBindGroup.Get(),
                             .emptyTextures = gx::g_emptyTextureBindGroup.Get(),
                             .indexBuffer = res.indexBuffer.Get(),
                         });
  const auto gxEncoder = gx_draw_encoder();
  const auto& size = c0.size;
  for (auto& command : passInfo.commands) {
    switch (command.type) {
    case CommandType::SetViewport: {
      const auto& vp = command.data.setViewport;
      const auto depth = gx::viewport_depth(vp.znear, vp.zfar, gx::UseReversedZ);
      kd::direct::set_viewport(vp.left, vp.top, vp.width, vp.height, depth.min, depth.max);
    } break;
    case CommandType::SetScissor: {
      const auto& sc = command.data.setScissor;
      const auto x = std::clamp(static_cast<uint32_t>(sc.x), 0u, size.width);
      const auto y = std::clamp(static_cast<uint32_t>(sc.y), 0u, size.height);
      const auto w = std::clamp(static_cast<uint32_t>(sc.width), 0u, size.width - x);
      const auto h = std::clamp(static_cast<uint32_t>(sc.height), 0u, size.height - y);
      kd::direct::set_scissor(x, y, w, h);
    } break;
    case CommandType::Draw: {
      auto& draw = command.data.draw;
      if (draw.encoder == nullptr) {
        break;
      }
      if (draw.encoder == gxEncoder) {
        gx::DrawData data;
        std::memcpy(&data, draw.payload.data(), sizeof(data));
        WGPURenderPipeline main = nullptr;
        WGPURenderPipeline prepass = nullptr;
        if (!get_pipeline_raw(data.pipeline, main, prepass)) {
          break; // still compiling: skipped, as gx::render does
        }
        WGPUBindGroup textures = nullptr;
        if (data.bindGroups.textureBindGroup) {
          textures = find_bind_group_raw(data.bindGroups.textureBindGroup);
          if (!textures) {
            break;
          }
        }
        kd::direct::gx_draw({
            .main = main,
            .prepass = prepass,
            .textures = textures,
            .uniformOffset = data.uniformRange.offset,
            .indexOffset = data.idxRange.offset,
            .immediates = &data.immediateData,
            .immediateBytes = sizeof(data.immediateData),
            .vtxCount = data.vtxCount,
            .indexCount = data.indexCount,
            .instanceCount = data.instanceCount,
            .blendConstant = data.dstAlpha != UINT32_MAX,
            .blendAlpha = data.dstAlpha / 255.f,
        });
      } else {
        clear::DrawData data;
        std::memcpy(&data, draw.payload.data(), sizeof(data));
        WGPURenderPipeline main = nullptr;
        WGPURenderPipeline prepass = nullptr;
        if (!get_pipeline_raw(data.pipeline, main, prepass)) {
          break;
        }
        kd::direct::clear_draw({
            .pipeline = main,
            .color = {static_cast<float>(data.color.r), static_cast<float>(data.color.g),
                      static_cast<float>(data.color.b), static_cast<float>(data.color.a)},
            .viewport = {0.f, 0.f, static_cast<float>(size.width), static_cast<float>(size.height), data.depth,
                         data.depth},
            .scissor = {static_cast<uint32_t>(data.rect.x), static_cast<uint32_t>(data.rect.y),
                        static_cast<uint32_t>(data.rect.width), static_cast<uint32_t>(data.rect.height)},
        });
      }
    } break;
    default:
      break;
    }
  }
  kd::direct::end_pass();
}
#endif

void render(wgpu::CommandEncoder& cmd, FramePacket& frame, RenderPass& passInfo, uint32_t passIndex) {
  ZoneScoped;
  if (!passInfo.sealed) {
    return;
  }

  for (const auto& conv : passInfo.paletteConvs) {
    tex_palette_conv::run(cmd, conv);
  }
  if (passInfo.discardable) {
    // This pass has no effect and can be safely discarded (e.g. an empty EFB segment between two back-to-back pass
    // breaks, or an unresolved offscreen pass).
    return;
  }

  std::array<wgpu::RenderPassColorAttachment, MaxColorAttachments> attachments{};
  for (uint32_t i = 0; i < passInfo.colorAttachmentCount; ++i) {
    const auto& source = passInfo.colorAttachments[i];
    attachments[i] = {
        .view = source.view,
        .resolveTarget = source.resolveView,
        .loadOp = source.loadOp != wgpu::LoadOp::Undefined ? source.loadOp
                                                           : (source.clear ? wgpu::LoadOp::Clear : wgpu::LoadOp::Load),
        .storeOp = source.storeOp,
        .clearValue =
            {
                .r = source.clearValue.x(),
                .g = source.clearValue.y(),
                .b = source.clearValue.z(),
                .a = source.clearValue.w(),
            },
    };
  }
  wgpu::RenderPassDepthStencilAttachment depthStencilAttachment{};
  const wgpu::RenderPassDepthStencilAttachment* depthStencilAttachmentPtr = nullptr;
  if (passInfo.depthStencilView) {
    depthStencilAttachment = {
        .view = passInfo.depthStencilView,
        .depthLoadOp = passInfo.hasDepth ? (passInfo.depthLoadOp != wgpu::LoadOp::Undefined
                                                ? passInfo.depthLoadOp
                                                : (passInfo.clearDepth ? wgpu::LoadOp::Clear : wgpu::LoadOp::Load))
                                         : wgpu::LoadOp::Undefined,
        .depthStoreOp = passInfo.hasDepth ? passInfo.depthStoreOp : wgpu::StoreOp::Undefined,
        .depthClearValue = passInfo.clearDepthValue,
        .stencilLoadOp = passInfo.hasStencil ? passInfo.stencilLoadOp : wgpu::LoadOp::Undefined,
        .stencilStoreOp = passInfo.hasStencil ? passInfo.stencilStoreOp : wgpu::StoreOp::Undefined,
        .stencilClearValue = passInfo.stencilClearValue,
    };
    depthStencilAttachmentPtr = &depthStencilAttachment;
  }
#ifdef AURORA_GFX_DEKO3D
  if (aurora_direct_renderer() && direct_eligible(passInfo)) {
    render_pass_direct(cmd, passInfo);
  } else
#endif
  {
  const auto label = passInfo.label.empty() ? fmt::format("Render pass {}", passIndex)
                                            : fmt::format("{} {}", passInfo.label, passIndex);
  const wgpu::RenderPassDescriptor renderPassDescriptor{
      .label = label.c_str(),
      .colorAttachmentCount = passInfo.colorAttachmentCount,
      .colorAttachments = attachments.data(),
      .depthStencilAttachment = depthStencilAttachmentPtr,
      .timestampWrites = webgpu::gpu_prof::pass_writes(label),
  };

  auto pass = cmd.BeginRenderPass(&renderPassDescriptor);
  render_pass(pass, frame, passInfo);
  pass.End();
  }

  if (passInfo.captureDepthSnapshot) {
    depth_peek::encode_frame_snapshot(cmd, passInfo.copySourceDepthView,
                                      passInfo.colorAttachments[SceneColorAttachmentIndex].size, passInfo.msaaSamples);
  }

  if (passInfo.resolveTarget) {
    const auto& dstSize = passInfo.resolveTarget->size;
    const bool needsConversion = tex_copy_conv::needs_conversion(passInfo.resolveFormat);
    const bool needsScaling = passInfo.resolvePartial ||
                              dstSize.width != static_cast<uint32_t>(passInfo.resolveRect.width) ||
                              dstSize.height != static_cast<uint32_t>(passInfo.resolveRect.height);
    const bool isDepth = gx::is_depth_format(passInfo.resolveFormat);
    if (isDepth && passInfo.msaaSamples > 1) {
      Log.fatal("Depth tex copies from multisampled EFB targets are not supported");
    }
    const tex_copy_conv::ConvRequest convReq{
        .fmt = passInfo.resolveFormat,
        .srcFmt = passInfo.resolveSourceFormat,
        .srcView = isDepth ? passInfo.copySourceDepthView : passInfo.copySourceView,
        .uniformRange = passInfo.resolveUniformRange,
        .dst = passInfo.resolveTarget,
        .sampleFilter = needsScaling ? tex_copy_conv::SampleFilter::Linear : tex_copy_conv::SampleFilter::Nearest,
    };
    const bool needsOpaqueAlpha = !isDepth && !gx::efb_has_alpha(convReq.srcFmt);
    const bool sameFormat =
        passInfo.resolveTarget->format == passInfo.colorAttachments[SceneColorAttachmentIndex].format;
    if (needsConversion) {
      tex_copy_conv::run(cmd, convReq);
    } else if (needsScaling || needsOpaqueAlpha || !sameFormat) {
      tex_copy_conv::blit(cmd, convReq);
    } else {
      const webgpu::gpu_prof::Zone zone{cmd, "EFB copy"};
      const wgpu::TexelCopyTextureInfo src{
          .texture = passInfo.copySourceTexture,
          .origin =
              wgpu::Origin3D{
                  .x = static_cast<uint32_t>(passInfo.resolveRect.x),
                  .y = static_cast<uint32_t>(passInfo.resolveRect.y),
              },
      };
      const wgpu::TexelCopyTextureInfo dst{
          .texture = passInfo.resolveTarget->texture,
      };
      const wgpu::Extent3D size{
          .width = static_cast<uint32_t>(passInfo.resolveRect.width),
          .height = static_cast<uint32_t>(passInfo.resolveRect.height),
          .depthOrArrayLayers = 1,
      };
      cmd.CopyTextureToTexture(&src, &dst, &size);
    }
  }

  if (passInfo.snapshotColorDst) {
    const webgpu::gpu_prof::Zone zone{cmd, "Pass snapshot"};
    const wgpu::TexelCopyTextureInfo src{
        .texture = passInfo.copySourceTexture,
    };
    const wgpu::TexelCopyTextureInfo dst{
        .texture = passInfo.snapshotColorDst,
    };
    const wgpu::Extent3D size{
        .width = passInfo.colorAttachments[SceneColorAttachmentIndex].size.width,
        .height = passInfo.colorAttachments[SceneColorAttachmentIndex].size.height,
        .depthOrArrayLayers = 1,
    };
    cmd.CopyTextureToTexture(&src, &dst, &size);
  }
  if (passInfo.snapshotDepthDst) {
    tex_copy_conv::snapshot_depth(cmd, passInfo.copySourceDepthView, passInfo.msaaSamples, passInfo.snapshotDepthDst);
  }
  if (passInfo.snapshotNormalDst) {
    const webgpu::gpu_prof::Zone zone{cmd, "Normal snapshot"};
    const wgpu::TexelCopyTextureInfo src{
        .texture = passInfo.copySourceNormalTexture,
    };
    const wgpu::TexelCopyTextureInfo dst{
        .texture = passInfo.snapshotNormalDst,
    };
    const wgpu::Extent3D size{
        .width = passInfo.colorAttachments[SceneColorAttachmentIndex].size.width,
        .height = passInfo.colorAttachments[SceneColorAttachmentIndex].size.height,
        .depthOrArrayLayers = 1,
    };
    cmd.CopyTextureToTexture(&src, &dst, &size);
  }
}

constexpr uint64_t VertexStagingOffset = 0;
constexpr uint64_t UniformStagingOffset = VertexStagingOffset + VertexBufferSize;
constexpr uint64_t IndexStagingOffset = UniformStagingOffset + UniformBufferSize;
constexpr uint64_t StorageStagingOffset = IndexStagingOffset + IndexBufferSize;
constexpr uint64_t TextureUploadStagingOffset = StorageStagingOffset + StorageBufferSize;

constexpr uint32_t align_down_copy_offset(uint32_t value) noexcept { return value & ~3u; }

void copy_staging_buffer_range(wgpu::CommandEncoder& cmd, const FramePacket& frame, uint32_t& copied,
                               uint32_t highWater, uint64_t stagingOffset, const wgpu::Buffer& dst) {
  if (highWater <= copied) {
    return;
  }
  const uint32_t copyStart = align_down_copy_offset(copied);
  const uint32_t copyEnd = AURORA_ALIGN(highWater, 4);
  cmd.CopyBufferToBuffer(staging_buffer(frame.stagingBuffer), stagingOffset + copyStart, dst, copyStart,
                         copyEnd - copyStart);
  copied = highWater;
}

bool needs_staging_copy(const FramePacket& frame, const FrameOp& op) {
  const auto& highWater = op.highWater;
  if (highWater.verts > frame.copied.verts || highWater.uniforms > frame.copied.uniforms ||
      highWater.indices > frame.copied.indices || highWater.storage > frame.copied.storage) {
    return true;
  }
  if constexpr (UseTextureBuffer) {
    return op.highWater.textureUploadCount > frame.copied.textureUploadCount;
  }
  return false;
}

void copy_staging_to_high_water(wgpu::CommandEncoder& cmd, FramePacket& frame, const FrameOp& op) {
  if (!needs_staging_copy(frame, op)) {
    return;
  }
  const webgpu::gpu_prof::Zone zone{cmd, "Staging copies"};
  const auto& highWater = op.highWater;
  auto& res = resources();
  copy_staging_buffer_range(cmd, frame, frame.copied.verts, highWater.verts, VertexStagingOffset, res.vertexBuffer);
  copy_staging_buffer_range(cmd, frame, frame.copied.uniforms, highWater.uniforms, UniformStagingOffset,
                            res.uniformBuffer);
  copy_staging_buffer_range(cmd, frame, frame.copied.indices, highWater.indices, IndexStagingOffset, res.indexBuffer);
  copy_staging_buffer_range(cmd, frame, frame.copied.storage, highWater.storage, StorageStagingOffset,
                            res.storageBuffer);

  if constexpr (UseTextureBuffer) {
    // SFA-NX: the op lists only its new uploads (FrameOp::newTextureUploads); index = base + j
    const size_t base = op.highWater.textureUploadCount - op.newTextureUploads.size();
    for (size_t j = 0; j < op.newTextureUploads.size(); ++j) {
      if (base + j < frame.copied.textureUploadCount) {
        continue; // already copied
      }
      const auto& item = *op.newTextureUploads[j];
      const wgpu::TexelCopyBufferInfo buf{
          .layout =
              wgpu::TexelCopyBufferLayout{
                  .offset = item.buffer ? item.layout.offset : item.layout.offset + TextureUploadStagingOffset,
                  .bytesPerRow = AURORA_ALIGN(item.layout.bytesPerRow, 256),
                  .rowsPerImage = item.layout.rowsPerImage,
              },
          .buffer = item.buffer ? item.buffer : staging_buffer(frame.stagingBuffer),
      };
      cmd.CopyBufferToTexture(&buf, &item.tex, &item.size);
    }
    frame.copied.textureUpload = highWater.textureUpload;
    frame.copied.textureUploadCount = op.highWater.textureUploadCount;
  }
}
} // namespace

namespace detail {
void encode_op(wgpu::CommandEncoder& cmd, FramePacket& frame, const FrameOp& op) {
  copy_staging_to_high_water(cmd, frame, op);
  switch (op.type) {
  case FrameOpType::RenderPass:
    if (op.renderPass != nullptr) {
      render(cmd, frame, *op.renderPass, op.index);
    }
    break;
  case FrameOpType::TextureCopy:
    if (op.textureCopy != nullptr) {
      const webgpu::gpu_prof::Zone zone{cmd, "Texture copy"};
      cmd.CopyTextureToTexture(&op.textureCopy->src, &op.textureCopy->dst, &op.textureCopy->size);
    }
    break;
  case FrameOpType::EncoderTask:
    if (op.encoderTask != nullptr) {
      execute_encoder_task(cmd, frame, *op.encoderTask);
    }
    break;
  }
}
} // namespace detail

void bind_pipeline(const wgpu::RenderPipeline& pipeline, const wgpu::RenderPassEncoder& pass) {
  if (pipeline.Get() == g_currentPipeline) {
    return;
  }
  pass.SetPipeline(pipeline);
  g_currentPipeline = pipeline.Get();
}

bool bind_pipeline(PipelineRef ref, const wgpu::RenderPassEncoder& pass) {
  CompiledPipeline pipeline;
  if (!get_pipeline(ref, pipeline)) {
    return false;
  }
  AURORA_ASSERT(!pipeline.prepass, "Prepass pipelines require individual binding");
  bind_pipeline(pipeline.main, pass);
  return true;
}
} // namespace aurora::gfx
