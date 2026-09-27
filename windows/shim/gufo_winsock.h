#ifndef GUFO_WINDOWS_SHIM_GUFO_WINSOCK_H_
#define GUFO_WINDOWS_SHIM_GUFO_WINSOCK_H_

// Maps the BSD socket headers gufo includes onto Winsock. Socket handles are
// stored in `int` fields by the portable code; Windows socket handles are
// kernel handle values that fit in 32 bits, so the narrowing is lossless.

#include <winsock2.h>
#include <ws2tcpip.h>

#define SHUT_RD SD_RECEIVE
#define SHUT_WR SD_SEND
#define SHUT_RDWR SD_BOTH

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
// Windows socket handles are not inherited by child processes unless asked.
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#endif
// Callers peek only after poll reports the socket readable, so the peek
// cannot block even without a non-blocking flag.
#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0
#endif

using nfds_t = unsigned long;

inline int poll(pollfd* descriptors, nfds_t count, int timeout_ms) {
  // WSAPoll rejects the output-only flags (POLLERR, POLLHUP, POLLNVAL) and the
  // unsupported POLLPRI in the requested events, which POSIX poll accepts.
  for (nfds_t index = 0; index < count; ++index)
    descriptors[index].events &= POLLIN | POLLOUT;
  return WSAPoll(descriptors, count, timeout_ms);
}

#endif  // GUFO_WINDOWS_SHIM_GUFO_WINSOCK_H_
