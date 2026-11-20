// Native deko3d adapter, part 1: object registries. Buffers keep their GPU memory in
// generations so a mapped write never changes bytes an already recorded copy will read; upload
// memory is pooled and retired by fence. See backend_internal.h for the whole adapter.
#include "backend_internal.h"

#include <optional>

namespace kd {
std::recursive_mutex mutex;  // no parser, translator or compiler wait may own it
std::mutex compilerMutex;
std::recursive_mutex queueMutex;  // backend_internal.h QueueLock
thread_local int t_objectLockDepth = 0, t_queueLockDepth = 0;
// A WebGPU string view as std::string (WGPU_STRLEN means NUL-terminated).
std::string str(WGPUStringView s) {
  return s.data ? std::string(s.data, s.length == WGPU_STRLEN ? strlen(s.data) : s.length) : "";
}
// New GPU memory with a completion fence.
std::shared_ptr<Allocation> allocation(uint32_t bytes, const char *label) {
  auto a = std::make_shared<Allocation>();
  a->mem = block_create(bytes, label);
  return a;
}
// Reusable staging memory for queue writes, capped at 64 MiB in total; an allocation is reused once
// nothing else holds it and its fence shows the GPU copy that read it has finished.
std::vector<std::shared_ptr<Allocation>> uploadPool;
uint64_t uploadPoolBytes = 0;
// Uploads outside the pool. used() records &fence in the frame's command
// buffer and deko3d fills it in at the next submit, so the Allocation must
// outlive that submit and the GPU copy. Dropping it with the caller's
// temporary let deko3d write the fence into freed heap memory: the Retro WFC
// heap corruption (docs/RETRO-WFC-HEAP-GUARD-RR6133-2026-10-17.md).
std::vector<std::shared_ptr<Allocation>> uploadsInFlight;
// Drops out-of-pool uploads whose GPU copies have finished (called after every submit).
void retire_uploads() {
  std::erase_if(uploadsInFlight, [](const std::shared_ptr<Allocation> &a) { return a->ready(); });
}
// Staging memory for one queue write: a free pooled block if one is big enough, else a new one.
std::shared_ptr<Allocation> upload_allocation(uint32_t bytes) {
  for (auto &a : uploadPool)
    if (a.use_count() == 1 && a->mem.size >= bytes && a->ready())
      return a;
  auto a = allocation(bytes, "queue upload");
  // Retain ordinary uploads for subsequent frames; very large, unusual uploads
  // are released once their fence has signalled instead of permanently
  // increasing the pool's budget.
  if (a->mem.size <= (8u << 20) && uploadPoolBytes + a->mem.size <= (64u << 20)) {
    uploadPoolBytes += a->mem.size;
    uploadPool.push_back(a);
  } else {
    retire_uploads();
    uploadsInFlight.push_back(a);
  }
  return a;
}
std::unordered_map<WGPUBuffer, std::shared_ptr<Buffer>> buffers;
// A read map in flight: the buffer, the fence after the GPU writes it, and Aurora's callback, which is
// called from ProcessEvents once Dawn and the fence are both done.
struct PendingMap {
  std::shared_ptr<Buffer> buffer;
  DkFence fence{};
  WGPUBufferMapCallbackInfo callback{};
  std::atomic<bool> notified{false};
  WGPUMapAsyncStatus status = WGPUMapAsyncStatus_Aborted;
  std::string message;
};
std::vector<std::shared_ptr<PendingMap>> pendingMaps;
// What the adapter needs from a bind group layout: which bindings take dynamic offsets, in order.
struct Layout {
  std::vector<uint32_t> dynamic;
  int refs = 1;
};
std::unordered_map<WGPUBindGroupLayout, Layout> layouts;
std::unordered_map<WGPUBindGroup, std::shared_ptr<Group>> groups;
} // namespace kd

// Object creation, external references and native queue operations share an
// order. In particular, Dawn may return an interned handle from a concurrent
// Create call.
std::recursive_mutex &kartpad_deko_object_mutex() { return kd::mutex; }

extern "C" {
// Every buffer gets CPU-visible GPU memory (at least 256 bytes, zero-filled), so mapping is just a pointer.
WGPUBuffer __wrap_wgpuDeviceCreateBuffer(WGPUDevice d, const WGPUBufferDescriptor *desc) {
  auto h = __real_wgpuDeviceCreateBuffer(d, desc);
  kd::Lock l(kd::mutex);
  auto b = std::make_shared<kd::Buffer>();
  b->size = desc->size;
  b->usage = desc->usage;
  b->active = kd::allocation(std::max<uint64_t>(256, desc->size), "Aurora buffer");
  b->versions.push_back(b->active);
  b->mem = b->active->mem;
  if (!b->mem)
    swdk::fatal("Buffer allocation failed: %llu", (unsigned long long)desc->size);
  memset(b->mem.cpu, 0, desc->size);
  kd::buffers[h] = b;
  return h;
}
void __wrap_wgpuBufferAddRef(WGPUBuffer h) {
  kd::Lock l(kd::mutex);
  if (kd::buffers.count(h))
    ++kd::buffers[h]->refs;
  __real_wgpuBufferAddRef(h);
}
void __wrap_wgpuBufferRelease(WGPUBuffer h) {
  kd::Lock l(kd::mutex);
  auto it = kd::buffers.find(h);
  if (it != kd::buffers.end() && !--it->second->refs)
    kd::buffers.erase(it);
  __real_wgpuBufferRelease(h);
}
// Mapped ranges point straight into the buffer's current generation.
void *__wrap_wgpuBufferGetMappedRange(WGPUBuffer h, size_t off, size_t n) {
  kd::Lock l(kd::mutex);
  return kd::required(kd::buffers, h, __func__)->mem.cpu + off;
}
const void *__wrap_wgpuBufferGetConstMappedRange(WGPUBuffer h, size_t off, size_t n) {
  kd::Lock l(kd::mutex);
  return kd::required(kd::buffers, h, __func__)->mem.cpu + off;
}
WGPUFuture __wrap_wgpuBufferMapAsync(WGPUBuffer h, WGPUMapMode mode, size_t off, size_t n,
                                     WGPUBufferMapCallbackInfo info) {
  // A write map only picks a staging generation (object lock); a read map records a fence or
  // drains the queue, so it takes the queue lock first.
  std::optional<kd::QueueLock> queueLock;
  if (mode & WGPUMapMode_Read)
    queueLock.emplace();
  kd::Lock l(kd::mutex);
  if (!(mode & WGPUMapMode_Read)) {
    if (mode & WGPUMapMode_Write)
      kd::required(kd::buffers, h, __func__)->prepare_map();
    return __real_wgpuBufferMapAsync(h, mode, off, n, info);
  }
  if (info.mode == WGPUCallbackMode_WaitAnyOnly) {
    kd::idle();
    return __real_wgpuBufferMapAsync(h, mode, off, n, info);
  }
  auto p = std::make_shared<kd::PendingMap>();
  p->buffer = kd::required(kd::buffers, h, __func__);
  p->callback = info;
  kd::open();
  kd::barrier();
  dkCmdBufSignalFence(swdk::R.cmd, &p->fence, true);
  swdk::submit_readback("asynchronous readback fence");
  kd::pendingMaps.push_back(p);
  info.userdata1 = p.get();
  info.userdata2 = nullptr;
  info.callback = [](WGPUMapAsyncStatus status, WGPUStringView message, void *user, void *) {
    auto *p = static_cast<kd::PendingMap *>(user);
    p->status = status;
    p->message = kd::str(message);
    p->notified.store(true, std::memory_order_release);
  };
  return __real_wgpuBufferMapAsync(h, mode, off, n, info);
}
// Runs Aurora's map callbacks for read maps whose fence has signalled.
void __wrap_wgpuInstanceProcessEvents(WGPUInstance instance) {
  __real_wgpuInstanceProcessEvents(instance);
  std::vector<std::shared_ptr<kd::PendingMap>> ready;
  {
    kd::Lock l(kd::mutex);
    for (auto it = kd::pendingMaps.begin(); it != kd::pendingMaps.end();) {
      auto &p = *it;
      if (p->notified.load(std::memory_order_acquire) &&
          (p->status != WGPUMapAsyncStatus_Success ||
           dkFenceWait(&p->fence, 0) == DkResult_Success)) {
        ready.push_back(p);
        it = kd::pendingMaps.erase(it);
      } else
        ++it;
    }
  }
  // Client callbacks may take GX locks. Never invoke one while holding the
  // native queue lock.
  for (auto &p : ready) {
    WGPUStringView msg{p->message.data(), p->message.size()};
    p->callback.callback(p->status, msg, p->callback.userdata1, p->callback.userdata2);
  }
}
WGPUBindGroupLayout
// Dawn may return the same (interned) layout handle for identical descriptors, so an existing record
// only gains a reference.
__wrap_wgpuDeviceCreateBindGroupLayout(WGPUDevice d, const WGPUBindGroupLayoutDescriptor *desc) {
  kd::Lock l(kd::mutex);
  auto h = __real_wgpuDeviceCreateBindGroupLayout(d, desc);
  if (auto it = kd::layouts.find(h); it != kd::layouts.end()) {
    ++it->second.refs;
    return h;
  }
  auto &out = kd::layouts[h].dynamic;
  for (size_t i = 0; i < desc->entryCount; ++i)
    if (desc->entries[i].buffer.hasDynamicOffset)
      out.push_back(desc->entries[i].binding);
  std::sort(out.begin(), out.end());
  return h;
}
void __wrap_wgpuBindGroupLayoutAddRef(WGPUBindGroupLayout h) {
  kd::Lock l(kd::mutex);
  if (auto it = kd::layouts.find(h); it != kd::layouts.end())
    ++it->second.refs;
  __real_wgpuBindGroupLayoutAddRef(h);
}
void __wrap_wgpuBindGroupLayoutRelease(WGPUBindGroupLayout h) {
  kd::Lock l(kd::mutex);
  auto it = kd::layouts.find(h);
  if (it != kd::layouts.end() && !--it->second.refs)
    kd::layouts.erase(it);
  __real_wgpuBindGroupLayoutRelease(h);
}

// Called by dk_objects.cpp's bind-group lifetime wrappers.
// Builds the adapter's record of a bind group: buffers resolved to native records, views and samplers
// referenced so they live as long as the group.
void kartpad_deko_group_create(WGPUBindGroup h, const WGPUBindGroupDescriptor *d) {
  kd::Lock l(kd::mutex);
  auto g = std::make_shared<kd::Group>();
  auto dyn = kd::layouts[d->layout].dynamic;
  for (size_t i = 0; i < d->entryCount; ++i) {
    const auto &e = d->entries[i];
    kd::Binding b{};
    b.binding = e.binding;
    if (e.buffer) {
      b.buffer = kd::required(kd::buffers, e.buffer, __func__);
      b.offset = e.offset;
      b.size = e.size == WGPU_WHOLE_SIZE ? b.buffer->size - e.offset : e.size;
      auto it = std::find(dyn.begin(), dyn.end(), e.binding);
      if (it != dyn.end())
        b.dynamic = it - dyn.begin();
    }
    if (e.textureView)
      b.view = kd::keep(e.textureView);
    if (e.sampler)
      b.sampler = kd::keep(e.sampler);
    g->entries.push_back(std::move(b));
  }
  kd::groups[h] = g;
}
// Adds n references (negative to release); the record is dropped at zero.
void kartpad_deko_group_ref(WGPUBindGroup h, int n) {
  kd::Lock l(kd::mutex);
  auto it = kd::groups.find(h);
  if (it != kd::groups.end() && (it->second->refs += n) == 0)
    kd::groups.erase(it);
}
}
