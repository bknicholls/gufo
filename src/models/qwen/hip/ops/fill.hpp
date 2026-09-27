#ifndef GUFO_MODELS_QWEN_HIP_OPS_FILL_HPP_
#define GUFO_MODELS_QWEN_HIP_OPS_FILL_HPP_

#include <cstddef>

#if defined(ENGINE_ENABLE_HIP)
#include <hip/hip_runtime.h>

namespace gufo::hip {

/// Zeroes `bytes` of device memory on `stream` with a kernel. On Windows,
/// hipMemsetAsync of memory outside the dedicated VRAM carve-out, which is
/// most of an APU's device memory, runs orders of magnitude slower than a
/// kernel store, so per-request clears must not use it.
[[nodiscard]] hipError_t LaunchZeroFill(void* data, std::size_t bytes,
                                        hipStream_t stream = nullptr);

}  // namespace gufo::hip

#endif  // defined(ENGINE_ENABLE_HIP)
#endif  // GUFO_MODELS_QWEN_HIP_OPS_FILL_HPP_
