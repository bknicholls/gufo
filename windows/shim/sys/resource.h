#ifndef GUFO_WINDOWS_SHIM_SYS_RESOURCE_H_
#define GUFO_WINDOWS_SHIM_SYS_RESOURCE_H_

// clang-format off
// psapi.h needs the types windows.h declares, so the order is fixed.
#include <windows.h>
#include <psapi.h>
// clang-format on

#include <errno.h>

#define RUSAGE_SELF 0

struct rusage {
  long ru_maxrss;
  long ru_minflt;
  long ru_majflt;
};

// Windows counts page faults without splitting soft from hard ones, so every
// fault is reported as minor. ru_maxrss is the peak working set in KiB.
inline int getrusage(int who, struct rusage* usage) {
  PROCESS_MEMORY_COUNTERS counters{};
  if (who != RUSAGE_SELF ||
      !GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
    errno = EINVAL;
    return -1;
  }
  usage->ru_maxrss = static_cast<long>(counters.PeakWorkingSetSize / 1024);
  usage->ru_minflt = static_cast<long>(counters.PageFaultCount);
  usage->ru_majflt = 0;
  return 0;
}

#endif  // GUFO_WINDOWS_SHIM_SYS_RESOURCE_H_
