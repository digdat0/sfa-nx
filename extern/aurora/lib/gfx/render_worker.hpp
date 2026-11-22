#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace aurora::gfx::render_worker {

enum class ItemType : uint8_t {
  BeginFrame,
  EncodePass,
  EndFrame,
  Sync,
  Shutdown,
};

using WorkCallback = std::function<void()>;

struct SyncState {
  std::mutex mutex;
  std::condition_variable cv;
  bool complete = false;
};

struct QueueItem {
  ItemType type = ItemType::Sync;
  uint64_t frameId = 0;
  uint32_t passIndex = 0;
  WorkCallback work;
  std::shared_ptr<SyncState> sync;
};

class BoundedQueue {
public:
  explicit BoundedQueue(size_t capacity);

  bool push(QueueItem item);
  std::optional<QueueItem> pop_for(std::chrono::milliseconds timeout, bool& closed);
  void close();
  void reset();
  [[nodiscard]] size_t size() const;

private:
  size_t m_capacity = 0;
  mutable std::mutex m_mutex;
  std::condition_variable m_notEmpty;
  std::condition_variable m_notFull;
  // SFA-NX: a fixed ring of m_capacity slots instead of a std::deque. The deque allocated and freed a
  // 512-byte node every 7 items as passes streamed through (two per pass, ~25 passes a frame), the
  // churn that fragmented newlib's heap on Switch. Slots are reused; a popped slot is reset so its
  // callback and sync state are released at once.
  std::vector<QueueItem> m_ring;
  size_t m_head = 0;  // index of the oldest item
  size_t m_count = 0; // items queued
  bool m_closed = false;
};

class FrameSlotPool {
public:
  explicit FrameSlotPool(size_t slotCount);

  size_t acquire();
  std::optional<size_t> try_acquire();
  void release(size_t slot);
  void reset();
  [[nodiscard]] size_t free_count() const;

private:
  mutable std::mutex m_mutex;
  std::condition_variable m_cv;
  std::vector<bool> m_freeSlots;
};

void initialize();
void shutdown();

void enqueue_begin_frame(uint64_t frameId, WorkCallback work);
void enqueue_encode_pass(uint64_t frameId, uint32_t passIndex, WorkCallback work);
void enqueue_end_frame(uint64_t frameId, WorkCallback work);
void enqueue_work(WorkCallback work);
void synchronize();

bool is_worker_thread() noexcept;
bool is_idle() noexcept;

} // namespace aurora::gfx::render_worker
