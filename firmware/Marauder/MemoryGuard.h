#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef ARDUINO
#include "freertos/FreeRTOS.h"
#endif

namespace marauder {

struct MemoryGuardThresholds {
  size_t pauseFreeBytes = 24 * 1024;
  size_t resumeFreeBytes = 40 * 1024;
  size_t pauseLargestBlockBytes = 8 * 1024;
  size_t resumeLargestBlockBytes = 12 * 1024;
};

class MemoryGuardState {
 public:
  explicit MemoryGuardState(
      MemoryGuardThresholds thresholds = MemoryGuardThresholds{});

  bool allow(size_t freeBytes, size_t largestBlockBytes,
             size_t requestedBytes = 0);
  bool paused() const;

 private:
  MemoryGuardThresholds thresholds_;
  bool paused_ = false;
};

class QueueBackpressureState {
 public:
  explicit QueueBackpressureState(size_t limit);
  bool allow(size_t currentSize);
  bool paused() const;

 private:
  size_t limit_;
  size_t resume_limit_;
  bool paused_ = false;
};

#ifdef ARDUINO
class RuntimeMemoryGuard {
 public:
  static RuntimeMemoryGuard& instance();

  bool allow(size_t requestedBytes = 0);
  bool paused() const;
  uint32_t rejectedAllocations() const;

 private:
  MemoryGuardState state_;
  uint32_t rejected_allocations_ = 0;
  mutable portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
};

template <typename List, typename Value>
bool memoryGuardedAdd(List* list, const Value& value,
                      size_t extraReserveBytes = 32) {
  if (list == nullptr) return false;
  const size_t requested = sizeof(Value) + sizeof(void*) + extraReserveBytes;
  if (!RuntimeMemoryGuard::instance().allow(requested)) return false;
  return list->add(value);
}

// RAII helper that, while in scope, steers ordinary heap allocations (operator
// new / malloc, e.g. LinkedList nodes and Arduino String buffers) toward PSRAM
// instead of the internal (DMA-capable) heap. Allocations that explicitly
// request MALLOC_CAP_INTERNAL / MALLOC_CAP_DMA -- the WiFi driver's RX buffers
// among them -- are unaffected and still come from internal RAM.
//
// A dense dual-band AP scan makes several tiny internal-heap allocations per
// discovered AP; those fragment the internal heap until RuntimeMemoryGuard
// pauses additions (the "AP list paused: low memory" stall). Wrapping just the
// AccessPoint build+insert in one of these keeps that churn in PSRAM (which has
// megabytes free on the V8), so the internal heap stays contiguous and the list
// can populate fully. If no PSRAM is present the helper is a no-op.
class ScopedPsramHeap {
 public:
  ScopedPsramHeap();
  ~ScopedPsramHeap();
  ScopedPsramHeap(const ScopedPsramHeap&) = delete;
  ScopedPsramHeap& operator=(const ScopedPsramHeap&) = delete;

 private:
  bool active_;
};
#endif

}  // namespace marauder
