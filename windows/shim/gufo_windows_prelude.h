#ifndef GUFO_WINDOWS_SHIM_GUFO_WINDOWS_PRELUDE_H_
#define GUFO_WINDOWS_SHIM_GUFO_WINDOWS_PRELUDE_H_

// Force-included ahead of every Windows translation unit, before any system
// header can define the types below.

// POSIX code keeps file offsets in off_t, which the CRT defines as a 32-bit
// long. Model files exceed 2 GiB, so a 32-bit off_t silently truncates the
// offsets of pread and lseek-style calls. The CRT's own structures use
// _off_t, which keeps its original width.
#ifndef _OFF_T_DEFINED
#define _OFF_T_DEFINED
typedef long _off_t;
typedef long long off_t;
#endif

#endif  // GUFO_WINDOWS_SHIM_GUFO_WINDOWS_PRELUDE_H_
