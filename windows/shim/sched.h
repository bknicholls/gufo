#ifndef GUFO_WINDOWS_SHIM_SCHED_H_
#define GUFO_WINDOWS_SHIM_SCHED_H_

#include <windows.h>

inline int sched_yield() {
  (void)SwitchToThread();
  return 0;
}

#endif  // GUFO_WINDOWS_SHIM_SCHED_H_
