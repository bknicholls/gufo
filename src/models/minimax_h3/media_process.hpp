#ifndef GUFO_MODELS_MINIMAX_H3_MEDIA_PROCESS_HPP_
#define GUFO_MODELS_MINIMAX_H3_MEDIA_PROCESS_HPP_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace gufo::minimax_h3 {

class CancellationToken;

/// An FFmpeg or FFprobe child process connected to this process by pipes.
/// Platforms differ in how a secondary input stream reaches the child: POSIX
/// passes an inherited descriptor, Windows a named pipe the child opens.
class MediaProcess {
public:
  enum class Input : std::uint8_t { kPrimary, kSecondary };

  /// Arguments equal to this are replaced with the URL through which FFmpeg
  /// reads the secondary input stream.
  static constexpr std::string_view kSecondaryInputUrl = "{secondary-input}";

  MediaProcess();
  /// Terminates and reaps a child that is still running.
  ~MediaProcess();
  MediaProcess(const MediaProcess&) = delete;
  MediaProcess& operator=(const MediaProcess&) = delete;

  /// Starts `program`, found through PATH, with `arguments` (whose first
  /// element names the program) and its standard output available to Read().
  /// A failure to launch is reported as "cannot start <name>: <reason>".
  [[nodiscard]] bool StartCapture(const char* program,
                                  std::vector<std::string> arguments,
                                  std::string_view name, std::string* error);

  /// Starts `program` with its standard input as the primary input and a
  /// secondary input stream, both fed through Write().
  [[nodiscard]] bool StartWithInputs(const char* program,
                                     std::vector<std::string> arguments,
                                     std::string_view name, std::string* error);

  /// Reads captured output: bytes read, 0 at the end of the stream, or -1.
  [[nodiscard]] long Read(void* data, std::size_t bytes);

  /// Writes a prefix of `data` to an input, waiting while its pipe is full.
  /// Returns the bytes written, or -1 with errno set: ECANCELED once `stop` is
  /// set or `cancellation` fires, EPIPE once the child stops reading.
  [[nodiscard]] long Write(Input input, const void* data, std::size_t bytes,
                           const CancellationToken* cancellation,
                           const std::atomic<bool>& stop);

  /// Closes an input, signalling end of stream. With `drain`, waits until the
  /// child has read everything written, because a Windows named pipe discards
  /// unread data when its server end closes. Only drain an input whose child
  /// is still reading, or the wait does not end.
  void CloseInput(Input input, bool drain);

  /// Asks the child to stop: SIGTERM on POSIX, TerminateProcess on Windows.
  void Terminate();

  /// Waits for the child. `exit_code` receives its exit status, or -1 when it
  /// did not exit normally.
  [[nodiscard]] bool Wait(int* exit_code, std::string* error);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace gufo::minimax_h3

#endif  // GUFO_MODELS_MINIMAX_H3_MEDIA_PROCESS_HPP_
