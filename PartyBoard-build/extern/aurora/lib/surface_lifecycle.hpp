#pragma once

#include <atomic>
#include <cstdint>

namespace aurora::window {
// Readiness alone loses a fast destroy/create transition between two frames.
// A generation also invalidates every surface created for the previous window.
class SurfaceLifecycle {
public:
  explicit SurfaceLifecycle(bool ready) noexcept : mReady(ready) {}
  void set_ready(bool ready) noexcept {
    if (!ready) {
      mReady.store(false, std::memory_order_release);
      mGeneration.fetch_add(1, std::memory_order_acq_rel);
    } else {
      mReady.store(true, std::memory_order_release);
    }
  }
  bool ready() const noexcept { return mReady.load(std::memory_order_acquire); }
  uint64_t generation() const noexcept { return mGeneration.load(std::memory_order_acquire); }
private:
  std::atomic_bool mReady;
  std::atomic_uint64_t mGeneration{0};
};
} // namespace aurora::window
