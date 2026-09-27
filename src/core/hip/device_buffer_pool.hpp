#ifndef GUFO_CORE_HIP_DEVICE_BUFFER_POOL_HPP_
#define GUFO_CORE_HIP_DEVICE_BUFFER_POOL_HPP_

#include <hip/hip_runtime.h>

#include <algorithm>
#include <cstddef>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace gufo::hip {

/// A device allocation that may be larger than the bytes it was acquired for.
struct PooledDeviceBuffer {
  void* data{nullptr};
  std::size_t capacity{0};
};

/// Keeps one released device allocation per slot for reuse.
///
/// A large hipMalloc commits and zeroes fresh pages, which costs far more
/// than copying the same bytes, and HIP's own freed-block cache grows without
/// bound when enabled. Request snapshots are allocated and freed once per
/// request and grow slowly as a conversation does, so a new buffer gets room
/// to grow for a released one to fit the next several, larger, requests.
/// A slot is one buffer role, such as a snapshot's KV planes.
class DeviceBufferPool {
public:
  explicit DeviceBufferPool(std::size_t slots) : retained_(slots) {}
  ~DeviceBufferPool() {
    for (const auto& buffer : retained_)
      if (buffer.data != nullptr)
        (void)hipFree(buffer.data);
  }
  DeviceBufferPool(const DeviceBufferPool&) = delete;
  DeviceBufferPool& operator=(const DeviceBufferPool&) = delete;

  /// Returns the slot's retained buffer if it holds `bytes`, else a new one
  /// with a quarter more room, but no more than `max_bytes`. A retained
  /// buffer too small for `bytes` belonged to an older, shorter request and
  /// is freed.
  [[nodiscard]] PooledDeviceBuffer Acquire(std::size_t slot, std::size_t bytes,
                                           std::size_t max_bytes) {
    if (bytes == 0)
      return {};
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      auto& retained = retained_.at(slot);
      if (retained.data != nullptr) {
        if (retained.capacity >= bytes)
          return std::exchange(retained, {});
        (void)hipFree(retained.data);
        retained = {};
      }
    }
    PooledDeviceBuffer buffer{
        .capacity =
            std::clamp(bytes + bytes / 4, bytes, std::max(bytes, max_bytes))};
    if (hipMalloc(&buffer.data, buffer.capacity) == hipSuccess)
      return buffer;
    (void)hipGetLastError();
    buffer.capacity = bytes;
    const auto status = hipMalloc(&buffer.data, buffer.capacity);
    if (status != hipSuccess)
      throw std::runtime_error(std::string("hipMalloc failed: ") +
                               hipGetErrorString(status));
    return buffer;
  }

  /// Retains the larger of `buffer` and the slot's retained buffer.
  void Release(std::size_t slot, PooledDeviceBuffer buffer) noexcept {
    if (buffer.data == nullptr)
      return;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (slot < retained_.size()) {
        auto& retained = retained_[slot];
        if (retained.capacity < buffer.capacity)
          std::swap(retained, buffer);
      }
    }
    if (buffer.data != nullptr)
      (void)hipFree(buffer.data);
  }

private:
  std::mutex mutex_;
  std::vector<PooledDeviceBuffer> retained_;
};

}  // namespace gufo::hip

#endif  // GUFO_CORE_HIP_DEVICE_BUFFER_POOL_HPP_
