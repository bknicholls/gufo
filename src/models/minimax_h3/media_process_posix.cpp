#if !defined(_WIN32)

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "src/models/minimax_h3/media_process.hpp"
#include "src/models/minimax_h3/runtime.hpp"

// POSIX requires the process environment through a mutable-pointer ABI even
// though posix_spawnp does not mutate it.
extern char**
    environ;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

namespace gufo::minimax_h3 {
namespace {

void SetError(std::string* error, std::string message) {
  if (error != nullptr) {
    *error = std::move(message);
  }
}

bool IsCancelled(const CancellationToken* cancellation) {
  return cancellation != nullptr && cancellation->IsCancelled();
}

void CloseFd(int* descriptor) {
  if (*descriptor >= 0) {
    while (close(*descriptor) != 0 && errno == EINTR) {
    }
    *descriptor = -1;
  }
}

bool EnsureSigpipeIgnored(std::string* error) {
  static std::once_flag once;
  static int failure = 0;
  std::call_once(once, [] {
    struct sigaction ignore{};
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    if (sigaction(SIGPIPE, &ignore, nullptr) != 0) {
      failure = errno;
    }
  });
  if (failure == 0) {
    return true;
  }
  SetError(error, "cannot ignore SIGPIPE for FFmpeg writers: " +
                      std::string(std::strerror(failure)));
  return false;
}

bool MakeNonBlocking(int descriptor, std::string* error) {
  const int flags = fcntl(descriptor, F_GETFL);
  if (flags < 0 || fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) != 0) {
    SetError(error, "cannot configure FFmpeg media pipe: " +
                        std::string(std::strerror(errno)));
    return false;
  }
  return true;
}

int AddCloseUnlessKept(posix_spawn_file_actions_t* actions, int descriptor,
                       int keep_a, int keep_b) {
  if (descriptor == keep_a || descriptor == keep_b) {
    return 0;
  }
  return posix_spawn_file_actions_addclose(actions, descriptor);
}

std::vector<char*> MutableArgv(std::span<std::string> storage) {
  std::vector<char*> arguments;
  arguments.reserve(storage.size() + 1);
  for (std::string& argument : storage) {
    arguments.push_back(argument.data());
  }
  arguments.push_back(nullptr);
  return arguments;
}

}  // namespace

struct MediaProcess::Impl {
  pid_t child{-1};
  bool reaped{false};
  int output{-1};
  std::array<int, 2> inputs{-1, -1};
};

MediaProcess::MediaProcess() : impl_(std::make_unique<Impl>()) {}

MediaProcess::~MediaProcess() {
  CloseFd(&impl_->output);
  CloseFd(&impl_->inputs[0]);
  CloseFd(&impl_->inputs[1]);
  if (impl_->child > 0 && !impl_->reaped) {
    Terminate();
    int ignored = 0;
    (void)Wait(&ignored, nullptr);
  }
}

bool MediaProcess::StartCapture(const char* program,
                                std::vector<std::string> arguments,
                                std::string_view name, std::string* error) {
  std::vector<char*> argv = MutableArgv(arguments);
  int stream[2] = {-1, -1};
  if (pipe(stream) != 0) {
    SetError(error, "cannot create media inspection pipe: " +
                        std::string(std::strerror(errno)));
    return false;
  }
  posix_spawn_file_actions_t actions;
  int code = posix_spawn_file_actions_init(&actions);
  if (code == 0) {
    code = posix_spawn_file_actions_adddup2(&actions, stream[1], STDOUT_FILENO);
  }
  if (code == 0) {
    code = posix_spawn_file_actions_addclose(&actions, stream[0]);
  }
  if (code == 0) {
    code = posix_spawn_file_actions_addclose(&actions, stream[1]);
  }
  if (code == 0) {
    code = posix_spawnp(&impl_->child, program, &actions, nullptr, argv.data(),
                        environ);
  }
  (void)posix_spawn_file_actions_destroy(&actions);
  CloseFd(&stream[1]);
  if (code != 0) {
    impl_->child = -1;
    CloseFd(&stream[0]);
    SetError(error, "cannot start " + std::string(name) + ": " +
                        std::string(std::strerror(code)));
    return false;
  }
  impl_->output = stream[0];
  return true;
}

bool MediaProcess::StartWithInputs(const char* program,
                                   std::vector<std::string> arguments,
                                   std::string_view name, std::string* error) {
  int video_pipe[2] = {-1, -1};
  int audio_pipe[2] = {-1, -1};
  if (pipe(video_pipe) != 0 || pipe(audio_pipe) != 0) {
    const int saved = errno;
    CloseFd(&video_pipe[0]);
    CloseFd(&video_pipe[1]);
    CloseFd(&audio_pipe[0]);
    CloseFd(&audio_pipe[1]);
    SetError(error, "cannot create FFmpeg media pipes: " +
                        std::string(std::strerror(saved)));
    return false;
  }
  const int audio_target =
      std::max({video_pipe[0], video_pipe[1], audio_pipe[0], audio_pipe[1]}) +
      1;
  for (std::string& argument : arguments) {
    if (argument == kSecondaryInputUrl) {
      argument = "pipe:" + std::to_string(audio_target);
    }
  }
  std::vector<char*> argv = MutableArgv(arguments);
  posix_spawn_file_actions_t actions;
  int code = posix_spawn_file_actions_init(&actions);
  if (code == 0) {
    code =
        posix_spawn_file_actions_adddup2(&actions, video_pipe[0], STDIN_FILENO);
  }
  if (code == 0) {
    code =
        posix_spawn_file_actions_adddup2(&actions, audio_pipe[0], audio_target);
  }
  const std::array<int, 4> descriptors = {video_pipe[0], video_pipe[1],
                                          audio_pipe[0], audio_pipe[1]};
  for (const int descriptor : descriptors) {
    if (code == 0) {
      code =
          AddCloseUnlessKept(&actions, descriptor, STDIN_FILENO, audio_target);
    }
  }
  if (code == 0) {
    code = posix_spawnp(&impl_->child, program, &actions, nullptr, argv.data(),
                        environ);
  }
  (void)posix_spawn_file_actions_destroy(&actions);
  CloseFd(&video_pipe[0]);
  CloseFd(&audio_pipe[0]);
  if (code != 0) {
    impl_->child = -1;
    CloseFd(&video_pipe[1]);
    CloseFd(&audio_pipe[1]);
    SetError(error, "cannot start " + std::string(name) + ": " +
                        std::string(std::strerror(code)));
    return false;
  }
  impl_->inputs = {video_pipe[1], audio_pipe[1]};
  if (!MakeNonBlocking(impl_->inputs[0], error) ||
      !MakeNonBlocking(impl_->inputs[1], error) ||
      !EnsureSigpipeIgnored(error)) {
    CloseFd(&impl_->inputs[0]);
    CloseFd(&impl_->inputs[1]);
    Terminate();
    int ignored = 0;
    (void)Wait(&ignored, nullptr);
    return false;
  }
  return true;
}

long MediaProcess::Read(void* data, std::size_t bytes) {
  while (true) {
    const ssize_t amount = read(impl_->output, data, bytes);
    if (amount < 0 && errno == EINTR) {
      continue;
    }
    return static_cast<long>(amount);
  }
}

long MediaProcess::Write(Input input, const void* data, std::size_t bytes,
                         const CancellationToken* cancellation,
                         const std::atomic<bool>& stop) {
  const int descriptor = impl_->inputs[static_cast<std::size_t>(input)];
  while (!stop.load(std::memory_order_acquire) && !IsCancelled(cancellation)) {
    const ssize_t amount = write(descriptor, data, bytes);
    if (amount >= 0) {
      return static_cast<long>(amount);
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      return -1;
    }
    struct pollfd readiness{descriptor, POLLOUT, 0};
    const int result = poll(&readiness, 1, 50);
    if (result < 0 && errno != EINTR) {
      return -1;
    }
    if (result > 0 && (readiness.revents & (POLLERR | POLLHUP | POLLNVAL))) {
      errno = EPIPE;
      return -1;
    }
  }
  errno = ECANCELED;
  return -1;
}

void MediaProcess::CloseInput(Input input, bool /*drain*/) {
  // A closed pipe still delivers its buffered bytes before end of stream.
  CloseFd(&impl_->inputs[static_cast<std::size_t>(input)]);
}

void MediaProcess::Terminate() {
  if (impl_->child > 0 && !impl_->reaped) {
    (void)kill(impl_->child, SIGTERM);
  }
}

bool MediaProcess::Wait(int* exit_code, std::string* error) {
  CloseFd(&impl_->output);
  int status = 0;
  while (waitpid(impl_->child, &status, 0) < 0) {
    if (errno == EINTR) {
      continue;
    }
    SetError(error, "cannot wait for media process: " +
                        std::string(std::strerror(errno)));
    return false;
  }
  impl_->reaped = true;
  *exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return true;
}

}  // namespace gufo::minimax_h3

#endif  // !defined(_WIN32)
