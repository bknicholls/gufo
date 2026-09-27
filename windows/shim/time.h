#ifndef GUFO_WINDOWS_SHIM_TIME_H_
#define GUFO_WINDOWS_SHIM_TIME_H_

#include <errno.h>
#include_next <time.h>
#include <windows.h>

typedef int clockid_t;
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1

inline int clock_gettime(clockid_t clock, struct timespec* time) {
  if (clock == CLOCK_REALTIME)
    return timespec_get(time, TIME_UTC) == TIME_UTC ? 0 : -1;
  if (clock != CLOCK_MONOTONIC) {
    errno = EINVAL;
    return -1;
  }
  LARGE_INTEGER frequency{};
  LARGE_INTEGER counter{};
  (void)QueryPerformanceFrequency(&frequency);
  (void)QueryPerformanceCounter(&counter);
  time->tv_sec = static_cast<time_t>(counter.QuadPart / frequency.QuadPart);
  time->tv_nsec = static_cast<long>((counter.QuadPart % frequency.QuadPart) *
                                    1000000000LL / frequency.QuadPart);
  return 0;
}

// The CRT's reentrant forms take their arguments in the opposite order.
inline struct tm* localtime_r(const time_t* seconds, struct tm* calendar) {
  return localtime_s(calendar, seconds) == 0 ? calendar : nullptr;
}

inline struct tm* gmtime_r(const time_t* seconds, struct tm* calendar) {
  return gmtime_s(calendar, seconds) == 0 ? calendar : nullptr;
}

#endif  // GUFO_WINDOWS_SHIM_TIME_H_
