#include "shader_info.hpp"

#include "../gfx/recording.hpp"
#include "viewport_depth.hpp"

#include <cmath>

#include <tracy/Tracy.hpp>

namespace aurora::gx {
namespace {
constexpr Module Log{"aurora::gx"};

bool is_alpha_bump_channel(GXChannelID id) { return id == GX_ALPHA_BUMP || id == GX_ALPHA_BUMPN; }

bool uses_konst(const TevStage& stage) {
  return stage.colorPass.a == GX_CC_KONST || stage.colorPass.b == GX_CC_KONST || stage.colorPass.c == GX_CC_KONST ||
         stage.colorPass.d == GX_CC_KONST || stage.alphaPass.a == GX_CA_KONST || stage.alphaPass.b == GX_CA_KONST ||
         stage.alphaPass.c == GX_CA_KONST || stage.alphaPass.d == GX_CA_KONST;
}

float konst_fraction(u32 sel) { return static_cast<float>(8 - sel) / 8.f; }

Vec4<float> resolve_konst(GXTevKColorSel kcSel, GXTevKAlphaSel kaSel) {
  const auto& kcolors = g_gxState.kcolors;
  Vec4<float> out{0.f, 0.f, 0.f, 0.f};
  if (kcSel <= GX_TEV_KCSEL_1_8) {
    const float f = konst_fraction(kcSel);
    out[0] = f;
    out[1] = f;
    out[2] = f;
  } else if (kcSel >= GX_TEV_KCSEL_K0 && kcSel <= GX_TEV_KCSEL_K3) {
    const auto& kc = kcolors[kcSel - GX_TEV_KCSEL_K0];
    out[0] = kc[0];
    out[1] = kc[1];
    out[2] = kc[2];
  } else if (kcSel >= GX_TEV_KCSEL_K0_R && kcSel <= GX_TEV_KCSEL_K3_A) {
    const u32 idx = kcSel - GX_TEV_KCSEL_K0_R;
    const float f = kcolors[idx & 3][idx >> 2];
    out[0] = f;
    out[1] = f;
    out[2] = f;
  } else {
    FATAL("invalid kcSel {}", underlying(kcSel));
  }
  if (kaSel <= GX_TEV_KASEL_1_8) {
    out[3] = konst_fraction(kaSel);
  } else if (kaSel >= GX_TEV_KASEL_K0_R && kaSel <= GX_TEV_KASEL_K3_A) {
    const u32 idx = kaSel - GX_TEV_KASEL_K0_R;
    out[3] = kcolors[idx & 3][idx >> 2];
  } else {
    FATAL("invalid kaSel {}", underlying(kaSel));
  }
  return out;
}

Vec4<float> texture_size_bias(const gfx::TextureBind& tex) {
  auto width = static_cast<float>(tex.texObj.width());
  auto height = static_cast<float>(tex.texObj.height());
  float vpBias = 0.f;
  if (tex.ref && tex.ref->hasArbitraryMips) {
    const float viewportScale =
        std::min(g_gxState.renderViewport.width / std::max(g_gxState.logicalViewport.width, 1.f),
                 g_gxState.renderViewport.height / std::max(g_gxState.logicalViewport.height, 1.f));
    const float replacementScale = static_cast<float>(tex.ref->size.width) / std::max(width, 1.f);
    vpBias = std::log2(viewportScale / std::max(replacementScale, 0.001f));
  }
  return {width, height, tex.texObj.lod_bias() + vpBias, 0.0f};
}

void color_arg_reg_info(GXTevColorArg arg, const TevStage& stage, ShaderInfo& info,
                        const std::bitset<MaxTevRegs>& writtenColor, const std::bitset<MaxTevRegs>& writtenAlpha) {
  switch (arg) {
  case GX_CC_CPREV:
    if (!writtenColor.test(GX_TEVPREV)) {
      info.loadsTevReg.set(GX_TEVPREV);
    }
    break;
  case GX_CC_APREV:
    if (!writtenAlpha.test(GX_TEVPREV)) {
      info.loadsTevReg.set(GX_TEVPREV);
    }
    break;
  case GX_CC_C0:
    if (!writtenColor.test(GX_TEVREG0)) {
      info.loadsTevReg.set(GX_TEVREG0);
    }
    break;
  case GX_CC_A0:
    if (!writtenAlpha.test(GX_TEVREG0)) {
      info.loadsTevReg.set(GX_TEVREG0);
    }
    break;
  case GX_CC_C1:
    if (!writtenColor.test(GX_TEVREG1)) {
      info.loadsTevReg.set(GX_TEVREG1);
    }
    break;
  case GX_CC_A1:
    if (!writtenAlpha.test(GX_TEVREG1)) {
      info.loadsTevReg.set(GX_TEVREG1);
    }
    break;
  case GX_CC_C2:
    if (!writtenColor.test(GX_TEVREG2)) {
      info.loadsTevReg.set(GX_TEVREG2);
    }
    break;
  case GX_CC_A2:
    if (!writtenAlpha.test(GX_TEVREG2)) {
      info.loadsTevReg.set(GX_TEVREG2);
    }
    break;
  case GX_CC_TEXC:
  case GX_CC_TEXA:
    if (stage.texMapId != GX_TEXMAP_NULL) {
      CHECK(stage.texCoordId != GX_TEXCOORD_NULL, "tex coord not bound");
      info.sampledTexCoords.set(stage.texCoordId);
      info.sampledTextures.set(stage.texMapId);
    }
    break;
  case GX_CC_RASC:
  case GX_CC_RASA:
    if (stage.channelId != GX_COLOR_NULL && stage.channelId != GX_COLOR_ZERO &&
        !is_alpha_bump_channel(stage.channelId)) {
      info.sampledColorChannels.set(color_channel(stage.channelId));
    }
    break;
  default:
    break;
  }
}

void alpha_arg_reg_info(GXTevAlphaArg arg, const TevStage& stage, ShaderInfo& info,
                        const std::bitset<MaxTevRegs>& writtenAlpha) {
  switch (arg) {
  case GX_CA_APREV:
    if (!writtenAlpha.test(GX_TEVPREV)) {
      info.loadsTevReg.set(GX_TEVPREV);
    }
    break;
  case GX_CA_A0:
    if (!writtenAlpha.test(GX_TEVREG0)) {
      info.loadsTevReg.set(GX_TEVREG0);
    }
    break;
  case GX_CA_A1:
    if (!writtenAlpha.test(GX_TEVREG1)) {
      info.loadsTevReg.set(GX_TEVREG1);
    }
    break;
  case GX_CA_A2:
    if (!writtenAlpha.test(GX_TEVREG2)) {
      info.loadsTevReg.set(GX_TEVREG2);
    }
    break;
  case GX_CA_TEXA:
    if (stage.texMapId != GX_TEXMAP_NULL) {
      CHECK(stage.texCoordId != GX_TEXCOORD_NULL, "tex coord not bound");
      info.sampledTexCoords.set(stage.texCoordId);
      info.sampledTextures.set(stage.texMapId);
    }
    break;
  case GX_CA_RASA:
    if (stage.channelId != GX_COLOR_NULL && stage.channelId != GX_COLOR_ZERO &&
        !is_alpha_bump_channel(stage.channelId)) {
      info.sampledColorChannels.set(color_channel(stage.channelId));
    }
    break;
  default:
    break;
  }
}

f32 tex_offset(GXTexOffset offs) noexcept {
  switch (offs) {
  default:
  case GX_TO_ZERO:
    return 0.f;
  case GX_TO_SIXTEENTH:
    return 1.f / 16.f;
  case GX_TO_EIGHTH:
    return 1.f / 8.f;
  case GX_TO_FOURTH:
    return 1.f / 4.f;
  case GX_TO_HALF:
    return 1.f / 2.f;
  case GX_TO_ONE:
    return 1.f;
  }
}

u32 point_texcoord_mask() noexcept {
  u32 mask = 0;
  for (int i = 0; i < MaxTexCoord; ++i) {
    if (g_gxState.texCoordScales[i].pointOffset) {
      mask |= 1 << i;
    }
  }
  return mask;
}

u32 line_texcoord_mask() noexcept {
  u32 mask = 0;
  for (int i = 0; i < MaxTexCoord; ++i) {
    if (g_gxState.texCoordScales[i].lineOffset) {
      mask |= 1 << i;
    }
  }
  return mask;
}
} // namespace

ShaderInfo build_shader_info(const ShaderConfig& config) noexcept {
  ZoneScoped;

  // Fragment part only; the vertex part (vtxUniformSize) is added at the end.
  ShaderInfo info{};
  info.lineMode = config.lineMode;

  for (int attr = 0; attr < config.attrs.size(); attr++) {
    const auto attrType = config.attrs[attr].attrType;
    if ((attr == GX_VA_PNMTXIDX || (attr >= GX_VA_TEX0MTXIDX && attr <= GX_VA_TEX7MTXIDX)) && attrType == GX_DIRECT) {
      info.indexAttr.set(attr);
    } else if (attrType == GX_INDEX8 || attrType == GX_INDEX16) {
      info.indexAttr.set(attr);
    }
  }

  std::bitset<MaxTevRegs> writtenColor;
  std::bitset<MaxTevRegs> writtenAlpha;
  for (int i = 0; i < config.tevStageCount; ++i) {
    const auto& stage = config.tevStages[i];
    // Both passes read the register file before this stage's writes commit
    color_arg_reg_info(stage.colorPass.a, stage, info, writtenColor, writtenAlpha);
    color_arg_reg_info(stage.colorPass.b, stage, info, writtenColor, writtenAlpha);
    color_arg_reg_info(stage.colorPass.c, stage, info, writtenColor, writtenAlpha);
    color_arg_reg_info(stage.colorPass.d, stage, info, writtenColor, writtenAlpha);
    alpha_arg_reg_info(stage.alphaPass.a, stage, info, writtenAlpha);
    alpha_arg_reg_info(stage.alphaPass.b, stage, info, writtenAlpha);
    alpha_arg_reg_info(stage.alphaPass.c, stage, info, writtenAlpha);
    alpha_arg_reg_info(stage.alphaPass.d, stage, info, writtenAlpha);
    if (uses_konst(stage)) {
      info.konstStages.set(i);
    }

    // Color writes only touch rgb; alpha writes only touch a. A register first
    // touched by an alpha-only write still needs its rgb loaded (and vice versa
    // via the read tracking above) so the untouched components stay defined.
    if (!writtenColor.test(stage.alphaOp.outReg) && !writtenAlpha.test(stage.alphaOp.outReg)) {
      info.loadsTevReg.set(stage.alphaOp.outReg);
    }
    writtenColor.set(stage.colorOp.outReg);
    writtenAlpha.set(stage.alphaOp.outReg);
    info.writesTevReg.set(stage.colorOp.outReg);
    info.writesTevReg.set(stage.alphaOp.outReg);
  }
  for (int i = 0; i < config.tevStageCount; ++i) {
    const auto& stage = config.tevStages[i];
    // Skip if not enabled
    if (stage.indTexStage >= config.numIndStages) {
      continue;
    }
    const bool usesIndStage = stage.indTexMtxId != GX_ITM_OFF || is_alpha_bump_channel(stage.channelId);
    const bool usesTevTexCoord = stage.indTexMtxId != GX_ITM_OFF || stage.indTexWrapS != GX_ITW_OFF ||
                                 stage.indTexWrapT != GX_ITW_OFF || stage.indTexAddPrev;
    if (usesTevTexCoord && stage.texCoordId != GX_TEXCOORD_NULL) {
      info.sampledTexCoords.set(stage.texCoordId);
    }
    if (usesTevTexCoord && stage.texMapId != GX_TEXMAP_NULL &&
        (stage.indTexMtxId != GX_ITM_OFF || stage.indTexAddPrev)) {
      info.sampledTextures.set(stage.texMapId);
    }
    if (!usesIndStage) {
      continue;
    }
    info.usedIndStages.set(stage.indTexStage);
    const auto& indStage = config.indStages[stage.indTexStage];
    info.sampledTextures.set(indStage.texMapId);
    info.sampledTexCoords.set(indStage.texCoordId);
    info.sampledIndTextures.set(indStage.texMapId);
    // Track which indirect matrix is used
    if (stage.indTexMtxId >= GX_ITM_0 && stage.indTexMtxId <= GX_ITM_2) {
      info.usedIndTexMtxs.set(stage.indTexMtxId - GX_ITM_0);
    } else if (stage.indTexMtxId >= GX_ITM_S0 && stage.indTexMtxId <= GX_ITM_S2) {
      info.usedIndTexMtxs.set(stage.indTexMtxId - GX_ITM_S0);
    } else if (stage.indTexMtxId >= GX_ITM_T0 && stage.indTexMtxId <= GX_ITM_T2) {
      info.usedIndTexMtxs.set(stage.indTexMtxId - GX_ITM_T0);
    }
  }

  // Emboss bump needs its source texcoord generated and a light enabled
  for (int i = 0; i < info.sampledTexCoords.size(); ++i) {
    if (!info.sampledTexCoords.test(i)) {
      continue;
    }
    const auto& tcg = config.tcgs[i];
    if (tcg.type >= GX_TG_BUMP0 && tcg.type <= GX_TG_BUMP7) {
      info.sampledTexCoords.set(tcg.embossSrc);
      info.lightingEnabled = true;
    }
  }

  // Vertex part: every channel and every active texgen, whatever the fragment stage samples, so the
  // vertex shader does not depend on the TEV setup. Lights are included when any channel lights or a
  // texgen embosses.
  info.lightingEnabled = false;
  for (u32 k = 0; k < MaxColorChannels; ++k) {
    const auto& cc = config.colorChannels[k];
    u32 flags = cc.lightingEnabled ? 1u : 0u;
    flags |= cc.matSrc == GX_SRC_VTX ? 2u : 0u;
    flags |= cc.ambSrc == GX_SRC_VTX ? 4u : 0u;
    flags |= static_cast<u32>(cc.diffFn) << 4;
    flags |= static_cast<u32>(cc.attnFn) << 8;
    info.chanFlags[k] = flags;
    info.lightingEnabled = info.lightingEnabled || cc.lightingEnabled;
    if (cc.lightingEnabled) {
      info.litPairs |= 1u << (k & 1);
    }
  }
  for (u32 i = 0; i < MaxTexCoord; ++i) {
    const auto& tcg = config.tcgs[i];
    // tgParams, four words per texgen: source attribute (TgSrcNone = unused), kind (| 0x100 to normalize),
    // matrix index (0xffff = identity; bump: the light index) and post-transform matrix (0xffff = identity).
    u32* p = &info.tgParams[i * 4];
    p[0] = TgSrcNone;
    p[1] = TgKindMtx3x4;
    p[2] = 0xffffu;
    p[3] = 0xffffu;
    if (tcg.src == GX_MAX_TEXGENSRC) {
      continue;
    }
    info.tgCount = i + 1;
    if (tcg.type >= GX_TG_BUMP0 && tcg.type <= GX_TG_BUMP7) {
      p[0] = tcg.embossSrc;
      p[1] = TgKindBump;
      p[2] = tcg.type - GX_TG_BUMP0;
      info.lightingEnabled = true;
      continue;
    }
    p[0] = tcg.src;
    p[1] = tcg.type == GX_TG_MTX2x4 ? TgKindMtx2x4 : tcg.type == GX_TG_SRTG ? TgKindSrtg : TgKindMtx3x4;
    if (tcg.normalize) {
      p[1] |= 0x100u;
    }
    p[2] = tcg.mtx == GX_IDENTITY ? 0xffffu : static_cast<u32>(tcg.mtx) / 3;
    p[3] = tcg.postMtx == GX_PTIDENTITY ? 0xffffu : (tcg.postMtx - GX_PTTEXMTX0) / 3;
  }
  info.vtxUniformSize = VtxUniformBaseSize + (info.lightingEnabled ? sizeof(Light) * GX::MaxLights : 0);

  info.uniformSize += info.loadsTevReg.count() * sizeof(Vec4<float>);
  info.uniformSize += info.konstStages.count() * sizeof(Vec4<float>);
  if (config.fogType != GX_FOG_NONE) {
    info.usesFog = true;
    info.uniformSize += sizeof(Fog);
  }
  info.uniformSize += MaxTexCoord * sizeof(Vec4<float>);
  if (info.usedIndTexMtxs.any()) {
    info.uniformSize += MaxIndTexMtxs * sizeof(Mat2x4<float>);
  }
  info.uniformSize += info.sampledTextures.count() * sizeof(Vec4<float>);
  info.uniformSize = gfx::align_uniform(info.vtxUniformSize + info.uniformSize);
  if (info.uniformSize > MaxUniformSize) {
    Log.fatal("Uniform size exceeds maximum: {} > {}", info.uniformSize, MaxUniformSize);
  }
  return info;
}

static void fill_uniform(ByteBuffer& buf, const ShaderInfo& info) noexcept {
  buf.reserve_extra(info.uniformSize);

  buf.append<f32>(g_gxState.renderViewport.width);
  buf.append<f32>(g_gxState.renderViewport.height);
  buf.append<f32>(g_gxState.logicalViewport.width);
  buf.append<f32>(g_gxState.logicalViewport.height);
  // The line/point parameters always take their 16 bytes, so the vertex part has a fixed layout.
  if (info.lineMode == 0) {
    buf.append_zeroes(16);
  } else {
    if (info.lineMode == 3) { // GX_POINTS
      buf.append<f32>(static_cast<f32>(g_gxState.pointSize) / 6.f);
      buf.append<f32>(1.0f);
      buf.append<f32>(tex_offset(g_gxState.pointTexOffset));
      buf.append<u32>(point_texcoord_mask());
    } else { // GX_LINES / GX_LINESTRIP
      buf.append<f32>(static_cast<f32>(g_gxState.lineWidth) / 6.f);
      buf.append<f32>(g_gxState.lineHalfAspect ? 0.5f : 1.f);
      buf.append<f32>(tex_offset(g_gxState.lineTexOffset));
      buf.append<u32>(line_texcoord_mask());
    }
  }
  auto proj = g_gxState.proj;
  if constexpr (UseReversedZ) {
    proj.m2 = proj.m2 * Vec4{-1.f, -1.f, -1.f, -1.f};
  } else {
    proj.m2 = proj.m2 + proj.m3;
  }
  const auto depth = viewport_depth(g_gxState.renderViewport.znear, g_gxState.renderViewport.zfar, UseReversedZ);
  if (depth.scale != 1.f || depth.offset != 0.f) {
    proj.m2 = proj.m2 * Vec4{depth.scale, depth.scale, depth.scale, depth.scale} +
              proj.m3 * Vec4{depth.offset, depth.offset, depth.offset, depth.offset};
  }
  buf.append(proj);
  buf.append(Vec4<float>{depth.scale, depth.offset, 0.f, 0.f});

  for (int i = 0; i < MaxPnMtx; i++) {
    buf.append(g_gxState.pnMtx[i].pos);
  }

  for (int i = 0; i < MaxTexMtx; i++) {
    buf.append(g_gxState.texMtxs[i]);
  }

  for (int i = 0; i < MaxPnMtx; i++) {
    buf.append(g_gxState.pnMtx[i].nrm);
  }

  // Generic vertex shader setup (shader.cpp): channel flags, light masks, texgens, channel colors.
  buf.append(info.chanFlags);
  for (int i = 0; i < MaxColorChannels; ++i) {
    buf.append<u32>(static_cast<u32>(g_gxState.colorChannelState[i].lightMask.to_ulong()));
  }
  buf.append<u32>(info.tgCount);
  buf.append_zeroes(12);
  buf.append(info.tgParams);
  static const Mat3x4<float> identity{
      {1.f, 0.f, 0.f, 0.f},
      {0.f, 1.f, 0.f, 0.f},
      {0.f, 0.f, 1.f, 0.f},
  };
  for (u32 i = 0; i < MaxTexCoord; ++i) {
    const u32 post = info.tgParams[i * 4 + 3];
    buf.append(post != 0xffffu ? g_gxState.ptTexMtxs[post] : identity);
  }
  for (int i = 0; i < MaxColorChannels; ++i) {
    buf.append(g_gxState.colorChannelState[i].ambColor);
  }
  for (int i = 0; i < MaxColorChannels; ++i) {
    buf.append(g_gxState.colorChannelState[i].matColor);
  }
  if (info.lightingEnabled) {
    static_assert(sizeof(g_gxState.lights) == 80 * GX::MaxLights);
    buf.append(g_gxState.lights);
  }

  // Fragment part.
  for (int i = 0; i < info.loadsTevReg.size(); ++i) {
    if (info.loadsTevReg.test(i)) {
      buf.append(g_gxState.colorRegs[i]);
    }
  }
  for (int i = 0; i < info.konstStages.size(); ++i) {
    if (info.konstStages.test(i)) {
      buf.append(resolve_konst(g_gxState.tevKColorSels[i], g_gxState.tevKAlphaSels[i]));
    }
  }
  if (info.usesFog) {
    const auto& state = g_gxState.fog;
    const float logicalWidth = std::max(g_gxState.logicalViewport.width, 1.f);
    const float renderWidth = std::max(g_gxState.renderViewport.width, 1.f);
    Fog fog{
        .color = state.color,
        .a = state.a,
        .b = state.b,
        .c = state.c,
        .rangeCenter = ((static_cast<float>(state.rangeCenter) - g_gxState.logicalViewport.left) / logicalWidth) * 2.f -
                       1.f + (g_gxState.renderViewport.left / renderWidth) * 2.f,
    };
    for (u32 i = 0; i < state.rangeK.size(); ++i) {
      const u32 source = (i & ~1u) | (1u - (i & 1u));
      fog.rangeK[i / 4][i % 4] = static_cast<float>(state.rangeK[source]) / 64.f;
    }
    fog.rangeK[2][2] = fog.rangeK[2][1];
    fog.rangeK[2][3] = fog.rangeK[2][1];
    buf.append(fog);
  }
  for (const auto& scale : g_gxState.texCoordScales) {
    buf.append(Vec4{static_cast<f32>(scale.scaleS) + 1.0f, static_cast<f32>(scale.scaleT) + 1.0f, 0.0f, 0.0f});
  }
  if (info.usedIndTexMtxs.any()) {
    for (u32 i = 0; i < MaxIndTexMtxs; ++i) {
      const auto& mtx = g_gxState.indTexMtxs[i];
      buf.append(Vec4{mtx.mtx.m0.x, mtx.mtx.m0.y, mtx.mtx.m1.x, mtx.mtx.m1.y});
      buf.append(Vec4{mtx.mtx.m2.x, mtx.mtx.m2.y, std::exp2f(mtx.scaleExp), 0.0f});
    }
  }
  for (int i = 0; i < info.sampledTextures.size(); ++i) {
    if (!info.sampledTextures.test(i)) {
      continue;
    }
    buf.append(texture_size_bias(get_texture(static_cast<GXTexMapID>(i))));
  }
}

gfx::Range build_uniform(const ShaderInfo& info) noexcept {
  ZoneScoped;
  // SFA-NX: written straight into the frame's uniform buffer (gfx::begin_uniform_append) instead of a scratch
  // buffer that was then copied.
  auto* target = gfx::begin_uniform_append();
  if (target == nullptr) {
    return {};
  }
  const size_t begin = target->size();
  fill_uniform(*target, info);
  return {static_cast<uint32_t>(begin), static_cast<uint32_t>(target->size() - begin)};
}
} // namespace aurora::gx
