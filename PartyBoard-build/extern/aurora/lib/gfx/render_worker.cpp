#include "render_worker.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>

#include <tracy/Tracy.hpp>

#include "../internal.hpp"

#if defined(__APPLE__)
// Metal (through Dawn) autoreleases objects on this thread. A std::thread has
// no autorelease pool, so they piled up in the thread's implicit pool and were
// only released when the thread exited -- after shutdown had destroyed the
// device they belong to, which crashed on quit ("PartyBoard quit unexpectedly",
// in objc_release under _pthread_exit). Each work item now drains its own pool.
extern "C" void* objc_autoreleasePoolPush(void);
extern "C" void objc_autoreleasePoolPop(void* pool);
#endif

namespace aurora::gfx::render_worker {
namespace {
Module Log("aurora::gfx::render_worker");

// The worker's time per kind of item, every 5 s ("Render worker"): how long
// it encodes a frame's passes and submits them, how busy it is, and how far
// behind the game it runs (items waiting). On a headset, a worker near 100%
// busy caps the images per second before the GPU does.
struct ItemStat {
  double sum = 0, max = 0;
  uint32_t count = 0;
};
ItemStat g_itemStats[5];
double g_busyMs = 0;
size_t g_queueMax = 0;
// The game thread waiting for a free frame slot (FrameSlotPool::acquire):
// the worker or the GPU behind the game.
std::atomic<int64_t> g_slotWaitNs = 0, g_slotWaitMaxNs = 0;
std::atomic<uint32_t> g_slotWaits = 0;
std::chrono::steady_clock::time_point g_statsAt = std::chrono::steady_clock::now();

void record_item(ItemType type, std::chrono::steady_clock::duration took) {
  const double ms = std::chrono::duration<double, std::milli>(took).count();
  auto& stat = g_itemStats[std::min<size_t>(static_cast<size_t>(type), std::size(g_itemStats) - 1)];
  stat.sum += ms;
  stat.max = std::max(stat.max, ms);
  ++stat.count;
  g_busyMs += ms;
  const auto now = std::chrono::steady_clock::now();
  const double elapsed = std::chrono::duration<double, std::milli>(now - g_statsAt).count();
  if (elapsed < 5000.0) {
    return;
  }
  const auto avg = [](const ItemStat& s) { return s.count != 0 ? s.sum / s.count : 0.0; };
  const auto& begin = g_itemStats[static_cast<size_t>(ItemType::BeginFrame)];
  const auto& encode = g_itemStats[static_cast<size_t>(ItemType::EncodePass)];
  const auto& end = g_itemStats[static_cast<size_t>(ItemType::EndFrame)];
  const double frames = end.count != 0 ? end.count : 1;
  Log.info("Render worker: {} frames, per frame begin {:.2f} ms, passes {:.2f} ms ({} passes, max {:.2f}), "
           "end/submit {:.2f} ms (max {:.2f}), busy {:.0f}%, queue max {}; game waited for a frame slot {} times, "
           "{:.1f} ms in all (max {:.1f})",
           end.count, begin.sum / frames, encode.sum / frames, static_cast<uint32_t>(encode.count / frames), encode.max,
           avg(end), end.max, 100.0 * g_busyMs / elapsed, g_queueMax, g_slotWaits.exchange(0),
           g_slotWaitNs.exchange(0) / 1e6, g_slotWaitMaxNs.exchange(0) / 1e6);
  for (auto& s : g_itemStats) s = {};
  g_busyMs = 0;
  g_queueMax = 0;
  g_statsAt = now;
}
constexpr size_t QueueCapacity = 256;
// Nothing runs on a timeout (the loop just waits again), and both enqueue and
// close() notify, so the worker can sleep until there is work. A 1 ms timeout
// woke it a thousand times a second while idle: on a phone that is battery,
// and CPU time taken from the game thread in every frame's idle gap.
constexpr auto IdlePumpInterval = std::chrono::milliseconds{250};

BoundedQueue g_queue{QueueCapacity};
std::thread g_thread;
std::atomic_bool g_running = false;
std::atomic_size_t g_pendingItems = 0;
std::thread::id g_workerThreadId;

void complete_sync(const std::shared_ptr<SyncState>& sync) {
  if (!sync) {
    return;
  }
  ZoneScoped;
  {
    std::lock_guard lock{sync->mutex};
    sync->complete = true;
  }
  sync->cv.notify_all();
}

void worker_main() {
#ifdef TRACY_ENABLE
  tracy::SetThreadName("Aurora render worker");
#endif
  g_workerThreadId = std::this_thread::get_id();

  while (true) {
    bool closed = false;
    auto item = g_queue.pop_for(IdlePumpInterval, closed);
    if (!item) {
      if (closed) {
        break;
      }
      continue;
    }

    if (item->work) {
      ZoneScopedN("QueueItem work");
      const auto started = std::chrono::steady_clock::now();
#if defined(__APPLE__)
      void* pool = objc_autoreleasePoolPush();
      item->work();
      objc_autoreleasePoolPop(pool);
#else
      item->work();
#endif
      g_queueMax = std::max(g_queueMax, g_queue.size());
      record_item(item->type, std::chrono::steady_clock::now() - started);
    }
    complete_sync(item->sync);
    g_pendingItems.fetch_sub(1, std::memory_order_acq_rel);
    if (item->type == ItemType::Shutdown) {
      break;
    }
  }

  g_workerThreadId = {};
}

void enqueue(QueueItem item) {
  ZoneScoped;
  if (is_worker_thread()) {
    if (item.work) {
      item.work();
    }
    complete_sync(item.sync);
    return;
  }

  if (!g_running.load(std::memory_order_acquire)) {
    if (item.work) {
      item.work();
    }
    complete_sync(item.sync);
    return;
  }

  g_pendingItems.fetch_add(1, std::memory_order_acq_rel);
  if (!g_queue.push(std::move(item))) {
    g_pendingItems.fetch_sub(1, std::memory_order_acq_rel);
  }
}
} // namespace

BoundedQueue::BoundedQueue(size_t capacity) : m_capacity(capacity) {}

bool BoundedQueue::push(QueueItem item) {
  ZoneScoped;
  std::unique_lock lock{m_mutex};
  m_notFull.wait(lock, [&] { return m_closed || m_items.size() < m_capacity; });
  if (m_closed) {
    return false;
  }
  m_items.emplace_back(std::move(item));
  lock.unlock();
  m_notEmpty.notify_one();
  return true;
}

std::optional<QueueItem> BoundedQueue::pop_for(std::chrono::milliseconds timeout, bool& closed) {
  std::unique_lock lock{m_mutex};
  m_notEmpty.wait_for(lock, timeout, [&] { return m_closed || !m_items.empty(); });
  closed = m_closed && m_items.empty();
  if (m_items.empty()) {
    return std::nullopt;
  }
  auto item = std::move(m_items.front());
  m_items.pop_front();
  lock.unlock();
  m_notFull.notify_one();
  return item;
}

void BoundedQueue::close() {
  {
    std::lock_guard lock{m_mutex};
    m_closed = true;
  }
  m_notEmpty.notify_all();
  m_notFull.notify_all();
}

void BoundedQueue::reset() {
  {
    std::lock_guard lock{m_mutex};
    m_items.clear();
    m_closed = false;
  }
  m_notFull.notify_all();
}

size_t BoundedQueue::size() const {
  std::lock_guard lock{m_mutex};
  return m_items.size();
}

FrameSlotPool::FrameSlotPool(size_t slotCount) : m_freeSlots(slotCount, true) {}

size_t FrameSlotPool::acquire() {
  std::unique_lock lock{m_mutex};
  const bool anyFree = std::find(m_freeSlots.begin(), m_freeSlots.end(), true) != m_freeSlots.end();
  const auto waitStart = std::chrono::steady_clock::now();
  m_cv.wait(lock, [&] {
    for (const bool free : m_freeSlots) {
      if (free) {
        return true;
      }
    }
    return false;
  });
  if (!anyFree) {
    const int64_t waited =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - waitStart).count();
    g_slotWaitNs.fetch_add(waited, std::memory_order_relaxed);
    g_slotWaits.fetch_add(1, std::memory_order_relaxed);
    int64_t max = g_slotWaitMaxNs.load(std::memory_order_relaxed);
    while (waited > max && !g_slotWaitMaxNs.compare_exchange_weak(max, waited, std::memory_order_relaxed)) {
    }
  }
  for (size_t i = 0; i < m_freeSlots.size(); ++i) {
    if (m_freeSlots[i]) {
      m_freeSlots[i] = false;
      return i;
    }
  }
  return 0;
}

std::optional<size_t> FrameSlotPool::try_acquire() {
  std::lock_guard lock{m_mutex};
  for (size_t i = 0; i < m_freeSlots.size(); ++i) {
    if (m_freeSlots[i]) {
      m_freeSlots[i] = false;
      return i;
    }
  }
  return std::nullopt;
}

void FrameSlotPool::release(size_t slot) {
  {
    std::lock_guard lock{m_mutex};
    if (slot < m_freeSlots.size()) {
      m_freeSlots[slot] = true;
    }
  }
  m_cv.notify_one();
}

void FrameSlotPool::reset() {
  {
    std::lock_guard lock{m_mutex};
    std::fill(m_freeSlots.begin(), m_freeSlots.end(), true);
  }
  m_cv.notify_all();
}

size_t FrameSlotPool::free_count() const {
  std::lock_guard lock{m_mutex};
  return static_cast<size_t>(std::ranges::count(m_freeSlots, true));
}

void initialize() {
  if (g_running.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  g_queue.reset();
  g_pendingItems.store(0, std::memory_order_release);
  g_thread = std::thread(worker_main);
}

void shutdown() {
  if (!g_running.load(std::memory_order_acquire)) {
    return;
  }
  enqueue({
      .type = ItemType::Shutdown,
  });
  if (g_thread.joinable()) {
    g_thread.join();
  }
  g_running.store(false, std::memory_order_release);
  g_queue.close();
  g_queue.reset();
  g_pendingItems.store(0, std::memory_order_release);
}

void enqueue_begin_frame(uint64_t frameId, WorkCallback work) {
  enqueue({
      .type = ItemType::BeginFrame,
      .frameId = frameId,
      .work = std::move(work),
  });
}

void enqueue_encode_pass(uint64_t frameId, uint32_t passIndex, WorkCallback work) {
  enqueue({
      .type = ItemType::EncodePass,
      .frameId = frameId,
      .passIndex = passIndex,
      .work = std::move(work),
  });
}

void enqueue_end_frame(uint64_t frameId, WorkCallback work) {
  enqueue({
      .type = ItemType::EndFrame,
      .frameId = frameId,
      .work = std::move(work),
  });
}

void enqueue_work(WorkCallback work) {
  enqueue({
      .type = ItemType::Sync,
      .work = std::move(work),
  });
}

void synchronize() {
  if (is_worker_thread()) {
    return;
  }

  ZoneScoped;
  auto sync = std::make_shared<SyncState>();
  enqueue({
      .type = ItemType::Sync,
      .sync = sync,
  });
  std::unique_lock lock{sync->mutex};
  sync->cv.wait(lock, [&] { return sync->complete; });
}

bool is_worker_thread() noexcept { return g_workerThreadId == std::this_thread::get_id(); }

bool is_idle() noexcept { return g_pendingItems.load(std::memory_order_acquire) == 0; }

} // namespace aurora::gfx::render_worker
