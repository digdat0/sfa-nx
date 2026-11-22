// Native deko3d adapter, part 4: presentation of a finished frame, and depth snapshots that
// serve the game's CPU depth reads a frame late without stalling the GPU. See
// backend_internal.h for the whole adapter.
#include "backend_internal.h"

#include "present_fsh_dksh.h"
#include "present_vsh_dksh.h"
// Presents Aurora's finished frame texture: draws it scaled to the 1280x720 swapchain image with a
// linear filter (shaders/present_*.glsl) and queues the image for display. Called from Aurora's end of
// frame on the render worker.
void kartpad_deko_present(WGPUTexture texture, unsigned width, unsigned height) {
  // The swapchain image first, without the renderer's lock: the window's dequeue blocks until the
  // display gives an image back (about 1 ms a present), and the frame worker's next frame waited
  // for the lock all that time (dk.h PresentImage).
  swdk::PresentImage image;
  const bool acquired = swdk::present_acquire(&image);
  kd::QueueLock queueLock;
  kd::Lock l(kd::mutex);
  static bool firstGameSnapshot = true;
  if (firstGameSnapshot) {
    swdk::dklog("first game snapshot: %ux%u, native present", width, height);
    firstGameSnapshot = false;
  }
  kd::open();
  auto *t = kd::find_texture(texture);
  if (!t || !t->valid)
    swdk::fatal("Missing presentation image");
  if (swdk::capture_frame()) {
    auto view = kd::level_view(t, 0);
    swdk::capture_image(view, t->width, t->height, t->format, "snapshot");
  }
  struct Picture {
    kd::Tex *texture;
    DkShader vs, fs;
  };
  static DkShader vs{}, fs{};
  static bool loaded = false;
  if (!loaded) {
    loaded = swdk::code_load(vs, present_vsh_dksh, present_vsh_dksh_size, "present vertex") &&
             swdk::code_load(fs, present_fsh_dksh, present_fsh_dksh_size, "present fragment");
    if (!loaded)
      swdk::fatal("Cannot load presentation shaders");
  }
  Picture picture{t, vs, fs};
  swdk::frame_present(
      [](void *data, int stage) {
        if (stage != 1)
          return;
        kd::GpuCheckpoint checkpoint("presentation draw", swdk::R.width, swdk::R.height);
        auto &p = *static_cast<Picture *>(data);
        kd::barrier();
        swdk::bind_2d_state(false);
        swdk::set_view(0, 0, swdk::R.width, swdk::R.height);
        auto v = kd::level_view(p.texture, 0);
        uint32_t slot = swdk::image_slot_alloc();
        swdk::write_image_view_descriptor(slot, v);
        DkResHandle handle = dkMakeTextureHandle(slot, swdk::kSamplerLinearClamp);
        const DkShader *sh[] = {&p.vs, &p.fs};
        swdk::code_make_visible();
        dkCmdBufBindVtxAttribState(swdk::R.cmd, nullptr, 0);
        dkCmdBufBindVtxBufferState(swdk::R.cmd, nullptr, 0);
        dkCmdBufBindShaders(swdk::R.cmd, DkStageFlag_Vertex | DkStageFlag_Fragment, sh, 2);
        dkCmdBufBindTextures(swdk::R.cmd, DkStage_Fragment, 0, &handle, 1);
        dkCmdBufDraw(swdk::R.cmd, DkPrimitive_Triangles, 3, 1, 0, 0);
        kd::gpu_timing_present();
        swdk::image_slot_free(slot);
        checkpoint.finish(true);
      },
      &picture, acquired ? &image : nullptr, /*fullWindow=*/true); // one draw over the whole window
}

namespace kd {
// The game reads the depth buffer from the CPU (GameCube EFB peeks, e.g. for lens flares). Reading the live
// depth buffer would stall until the GPU finishes, so each frame's depth is copied into one of three
// buffers and reads use the newest copy whose fence has passed (one or two frames old). params maps the
// game's EFB coordinates onto the render target, reversed flips Aurora's reversed-Z depth back.
struct DepthSnapshot {
  Block data;
  DkFence fence{};
  bool pending = false, ready = false, reversed = true;
  uint64_t serial = 0;
  KartPadDepthParams params{};
};
std::array<DepthSnapshot, 3> depthSnapshots;
uint64_t depthSerial = 0;
// Guards depthSnapshots only. The main thread's depth reads (several per frame) used kd::mutex,
// which the render worker holds while it replays a whole frame in QueueSubmit: 3 ms/frame of main
// thread waiting on hardware (11192026-0924). The copy closure takes this inside kd::mutex; the
// read never takes kd::mutex, so the order is fixed.
std::mutex depthMutex;
// An R32 float image the size of the depth target, used to read hardware-compressed depth (below).
struct DepthScratch {
  DkImage image{};
  swdk::ImageAlloc mem;
  uint32_t width = 0, height = 0;
};
DepthScratch depthScratch;
// SFA-NX: with compression on, the copy engine cannot read the compressed depth target correctly, so the
// depth is first drawn into the float scratch image (draw_copy, which samples it through the 3D engine)
// and copied from there. Returns false when the target is not compressed (it is then copied directly).
static bool depth_as_float(View *v, uint32_t w, uint32_t h, DkImageView &out) {
  if (!v || !v->tex->hwCompression || v->tex->samples != 1 || (v->tex->width >> v->baseMip) != w ||
      (v->tex->height >> v->baseMip) != h)
    return false;
  auto &s = depthScratch;
  if (s.width != w || s.height != h) {
    if (s.mem.block)
      swdk::image_free_later(s.mem);
    s = {};
    DkImageLayoutMaker m;
    dkImageLayoutMakerDefaults(&m, swdk::R.device);
    m.type = DkImageType_2D;
    m.format = DkImageFormat_R32_Float;
    m.dimensions[0] = w;
    m.dimensions[1] = h;
    m.flags = DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine;
    swdk::image_tile_size_fix(m, h);
    DkImageLayout layout;
    dkImageLayoutInitialize(&layout, &m);
    s.mem = swdk::image_alloc(uint32_t(dkImageLayoutGetSize(&layout)), dkImageLayoutGetAlignment(&layout));
    if (!s.mem.block)
      return false;
    dkImageInitialize(&s.image, &layout, s.mem.block, s.mem.offset);
    s.width = w;
    s.height = h;
    swdk::dklog("depth snapshot: compressed depth read through a %ux%u float image", w, h);
  }
  DkImageView target;
  dkImageViewDefaults(&target, &s.image);
  if (!draw_copy(v->view, target, w, h, DkScissor{0, 0, w, h}))
    return false;
  kd::barrier();
  dkImageViewDefaults(&out, &s.image);
  return true;
}
} // namespace kd
// Records the depth copy into the frame. If all three snapshots are still in flight the copy is skipped
// and reads keep using the previous one.
void kartpad_deko_depth_snapshot(WGPUCommandEncoder encoder, WGPUTextureView view,
                                 KartPadDepthParams params, bool reversed) {
  auto hold = kd::keep(view);
  kd::record(encoder, [hold, params, reversed] {
    std::lock_guard<std::mutex> dl(kd::depthMutex);
    kd::DepthSnapshot *slot = nullptr;
    for (auto &s : kd::depthSnapshots)
      if (!s.pending || dkFenceWait(&s.fence, 0) == DkResult_Success) {
        slot = &s;
        break;
      }
    if (!slot)
      return;
    const uint32_t bytes = params.srcWidth * params.srcHeight * 4;
    if (slot->data.size < bytes) {
      if (slot->data)
        swdk::block_free_deferred(slot->data.block);
      slot->data = swdk::block_create(bytes, "depth snapshot");
    }
    slot->params = params;
    slot->reversed = reversed;
    slot->serial = ++kd::depthSerial;
    slot->ready = false;
    slot->pending = true;
    kd::barrier();
    auto *v = kd::find_view(hold.Get());
    DkCopyBuf dst{slot->data.gpu, params.srcWidth * 4, bytes};
    DkImageRect rect{0, 0, 0, params.srcWidth, params.srcHeight, 1};
    DkImageView floatView;
    const bool asFloat = kd::depth_as_float(v, params.srcWidth, params.srcHeight, floatView);
    dkCmdBufCopyImageToBuffer(swdk::R.cmd, asFloat ? &floatView : &v->view, &rect, &dst, 0);
    kd::barrier();
    dkCmdBufSignalFence(swdk::R.cmd, &slot->fence, true);
  });
}
// One depth read at EFB position (x, y) from the newest finished snapshot, as the GameCube's 24-bit
// value. Returns false before the first snapshot completes.
bool kartpad_deko_depth_read(unsigned x, unsigned y, unsigned &z) {
  std::lock_guard<std::mutex> dl(kd::depthMutex);
  kd::DepthSnapshot *latest = nullptr;
  for (auto &s : kd::depthSnapshots) {
    if (s.pending && dkFenceWait(&s.fence, 0) == DkResult_Success) {
      s.pending = false;
      s.ready = true;
    }
    if (s.ready && (!latest || s.serial > latest->serial))
      latest = &s;
  }
  if (!latest)
    return false;
  const auto &p = latest->params;
  if (x >= p.dstWidth || y >= p.dstHeight)
    return false;
  int sx = std::clamp(int(std::floor(p.offsetX + (x + 0.5f) * p.scaleX)), 0, int(p.srcWidth) - 1),
      sy = std::clamp(int(std::floor(p.offsetY + (y + 0.5f) * p.scaleY)), 0, int(p.srcHeight) - 1);
  float f;
  memcpy(&f, latest->data.cpu + (uint64_t(sy) * p.srcWidth + sx) * 4, 4);
  if (latest->reversed)
    f = 1 - f;
  z = std::min(uint32_t(std::clamp(f, 0.f, 1.f) * 16777216.f + 0.5f), 0xFFFFFFu);
  return true;
}

// SwitchWaker's host heap and thread hooks have no game-heap equivalent here.
// Hooks the imported SwitchWaker code calls; in SFA-NX they do nothing or map onto the KartPad thread API.
void JKRPcBeginHostAlloc() {}
extern "C" void cos_switch_flush_logs() {
  fflush(stdout);
  fflush(stderr);
}
extern "C" int kartpad_thread_apply(int, int);
extern "C" void cos_switch_thread_role(int) { kartpad_thread_apply(3, 0); }
