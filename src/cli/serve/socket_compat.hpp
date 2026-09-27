#ifndef GUFO_SERVER_SOCKET_COMPAT_HPP_
#define GUFO_SERVER_SOCKET_COMPAT_HPP_

#include <cstddef>

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <csignal>
#endif

namespace gufo::server {

// Winsock needs a per-process startup before any socket call; POSIX sockets
// need none. Safe to call repeatedly and from several threads.
inline bool InitializeSockets() {
#if defined(_WIN32)
  static const bool initialized = [] {
    WSADATA data{};
    return ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  return initialized;
#else
  return true;
#endif
}

// Winsock sockets are not CRT file descriptors, so read/close must use the
// socket API there. POSIX keeps the original descriptor calls.
inline long ReadSocket(int fd, char* data, std::size_t size) {
#if defined(_WIN32)
  return ::recv(static_cast<SOCKET>(fd), data, static_cast<int>(size), 0);
#else
  return static_cast<long>(::read(fd, data, size));
#endif
}

inline void CloseSocket(int fd) {
#if defined(_WIN32)
  (void)::closesocket(static_cast<SOCKET>(fd));
#else
  (void)::close(fd);
#endif
}

// SO_RCVTIMEO/SO_SNDTIMEO take a timeval on POSIX but a DWORD of milliseconds
// on Windows; passing a timeval there silently sets a near-zero timeout.
inline void SetSocketTimeout(int fd, int option, int seconds) {
#if defined(_WIN32)
  const DWORD milliseconds = static_cast<DWORD>(seconds) * 1000;
  (void)::setsockopt(static_cast<SOCKET>(fd), SOL_SOCKET, option,
                     reinterpret_cast<const char*>(&milliseconds),
                     sizeof(milliseconds));
#else
  const timeval timeout{seconds, 0};
  (void)::setsockopt(fd, SOL_SOCKET, option, &timeout, sizeof(timeout));
#endif
}

inline bool SetSocketNonBlocking(int fd, bool enabled) {
#if defined(_WIN32)
  u_long mode = enabled ? 1 : 0;
  return ::ioctlsocket(static_cast<SOCKET>(fd), FIONBIO, &mode) == 0;
#else
  const int flags = ::fcntl(fd, F_GETFL, 0);
  return flags >= 0 &&
         ::fcntl(fd, F_SETFL,
                 enabled ? flags | O_NONBLOCK : flags & ~O_NONBLOCK) == 0;
#endif
}

// A socket accepted from a non-blocking listener inherits non-blocking mode on
// Windows but not on POSIX. Connection handlers expect blocking reads.
inline void RestoreAcceptedSocketBlocking(int fd) {
#if defined(_WIN32)
  (void)SetSocketNonBlocking(fd, false);
#else
  (void)fd;
#endif
}

// Writes to a disconnected peer raise SIGPIPE on POSIX; Windows reports the
// failure through the send return value instead.
inline void IgnoreBrokenPipeSignal() {
#if !defined(_WIN32)
  (void)::signal(SIGPIPE, SIG_IGN);
#endif
}

}  // namespace gufo::server

#endif  // GUFO_SERVER_SOCKET_COMPAT_HPP_
