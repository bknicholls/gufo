#if defined(ENGINE_ENABLE_HIP)
#include <hip/hip_runtime.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "src/models/qwen/hip/ops/fill.hpp"

namespace gufo::hip {
namespace {

constexpr std::size_t kZeroFillThreads = 256;
constexpr std::size_t kZeroFillMaxBlocks = 1024;

__global__ void ZeroFillKernel(uint4* __restrict__ vectors,
                               std::size_t vector_count,
                               std::uint8_t* __restrict__ tail,
                               std::size_t tail_bytes) {
  const std::size_t first =
      static_cast<std::size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
  const std::size_t stride = static_cast<std::size_t>(gridDim.x) * blockDim.x;
  for (std::size_t index = first; index < vector_count; index += stride)
    vectors[index] = uint4{0, 0, 0, 0};
  if (first < tail_bytes)
    tail[first] = 0;
}

}  // namespace

hipError_t LaunchZeroFill(void* data, std::size_t bytes, hipStream_t stream) {
  if (bytes == 0)
    return hipSuccess;
  if (reinterpret_cast<std::uintptr_t>(data) % alignof(uint4) != 0)
    return hipMemsetAsync(data, 0, bytes, stream);
  const std::size_t vector_count = bytes / sizeof(uint4);
  const std::size_t tail_bytes = bytes % sizeof(uint4);
  const std::size_t blocks = std::clamp<std::size_t>(
      (vector_count + kZeroFillThreads - 1) / kZeroFillThreads, 1,
      kZeroFillMaxBlocks);
  auto* vectors = static_cast<uint4*>(data);
  ZeroFillKernel<<<static_cast<unsigned>(blocks),
                   static_cast<unsigned>(kZeroFillThreads), 0, stream>>>(
      vectors, vector_count,
      reinterpret_cast<std::uint8_t*>(vectors + vector_count), tail_bytes);
  return hipGetLastError();
}

}  // namespace gufo::hip
#endif  // defined(ENGINE_ENABLE_HIP)
