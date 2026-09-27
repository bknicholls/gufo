#ifndef GUFO_WINDOWS_SHIM_STRINGS_H_
#define GUFO_WINDOWS_SHIM_STRINGS_H_

#include <string.h>

inline int strcasecmp(const char* left, const char* right) {
  return _stricmp(left, right);
}

inline int strncasecmp(const char* left, const char* right, size_t count) {
  return _strnicmp(left, right, count);
}

#endif  // GUFO_WINDOWS_SHIM_STRINGS_H_
