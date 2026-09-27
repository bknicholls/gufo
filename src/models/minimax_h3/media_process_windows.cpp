#if defined(_WIN32)

#include <windows.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <string>
#include <vector>

#include "src/models/minimax_h3/media_process.hpp"
#include "src/models/minimax_h3/runtime.hpp"

namespace gufo::minimax_h3 {
namespace {

constexpr DWORD kPipeBufferBytes = 1U << 20U;
constexpr DWORD kPollMilliseconds = 50;

void SetError(std::string* error, std::string message) {
  if (error != nullptr) {
    *error = std::move(message);
  }
}

bool IsCancelled(const CancellationToken* cancellation) {
  return cancellation != nullptr && cancellation->IsCancelled();
}

std::string SystemMessage(DWORD code) {
  char* buffer = nullptr;
  const DWORD length = FormatMessageA(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, code, 0, reinterpret_cast<char*>(&buffer), 0, nullptr);
  std::string message = length != 0 ? std::string(buffer, length)
                                    : "error " + std::to_string(code);
  LocalFree(buffer);
  while (!message.empty() &&
         (message.back() == '\n' || message.back() == '\r' ||
          message.back() == '.')) {
    message.pop_back();
  }
  return message;
}

std::wstring Widen(const std::string& text) {
  if (text.empty()) {
    return {};
  }
  const int length = MultiByteToWideChar(
      CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring wide(static_cast<std::size_t>(length), L'\0');
  (void)MultiByteToWideChar(CP_UTF8, 0, text.data(),
                            static_cast<int>(text.size()), wide.data(), length);
  return wide;
}

// Quotes one argument so the child's C runtime parses it back unchanged:
// backslashes are literal except before a quote, where they escape.
void AppendQuoted(std::wstring* command_line, const std::wstring& argument) {
  if (!command_line->empty()) {
    command_line->push_back(L' ');
  }
  if (!argument.empty() &&
      argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
    command_line->append(argument);
    return;
  }
  command_line->push_back(L'"');
  std::size_t backslashes = 0;
  for (const wchar_t character : argument) {
    if (character == L'\\') {
      ++backslashes;
      continue;
    }
    command_line->append(character == L'"' ? backslashes * 2 + 1 : backslashes,
                         L'\\');
    backslashes = 0;
    command_line->push_back(character);
  }
  command_line->append(backslashes * 2, L'\\');
  command_line->push_back(L'"');
}

std::wstring CommandLine(const char* program,
                         const std::vector<std::string>& arguments) {
  std::wstring command_line;
  AppendQuoted(&command_line, Widen(program));
  for (std::size_t index = 1; index < arguments.size(); ++index) {
    AppendQuoted(&command_line, Widen(arguments[index]));
  }
  return command_line;
}

HANDLE InheritableCopy(HANDLE handle) {
  HANDLE copy = nullptr;
  if (handle == nullptr || handle == INVALID_HANDLE_VALUE ||
      !DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), &copy,
                       0, TRUE, DUPLICATE_SAME_ACCESS)) {
    return nullptr;
  }
  return copy;
}

HANDLE OpenNullDevice(DWORD access) {
  SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
  const HANDLE device =
      CreateFileW(L"NUL", access, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                  OPEN_EXISTING, 0, nullptr);
  return device == INVALID_HANDLE_VALUE ? nullptr : device;
}

void CloseOwned(HANDLE* handle) {
  if (*handle != nullptr && *handle != INVALID_HANDLE_VALUE) {
    CloseHandle(*handle);
  }
  *handle = nullptr;
}

// The server end of a named pipe the parent writes. Overlapped I/O lets a
// write wait for pipe space while still noticing cancellation and child exit.
struct OutboundPipe {
  HANDLE handle{nullptr};
  HANDLE event{nullptr};
  bool connected{false};
  bool connect_pending{false};
  OVERLAPPED connect{};
};

}  // namespace

struct MediaProcess::Impl {
  HANDLE process{nullptr};
  bool reaped{false};
  HANDLE output{nullptr};
  std::array<OutboundPipe, 2> inputs{};

  // Waits for an overlapped operation on `pipe`. Returns false with errno set
  // when the child exits, cancellation fires or `stop` is set first.
  bool Complete(OutboundPipe& pipe, OVERLAPPED* operation, DWORD* transferred,
                const CancellationToken* cancellation,
                const std::atomic<bool>& stop) {
    const std::array<HANDLE, 2> handles = {operation->hEvent, process};
    while (true) {
      const DWORD wait =
          WaitForMultipleObjects(static_cast<DWORD>(handles.size()),
                                 handles.data(), FALSE, kPollMilliseconds);
      if (wait == WAIT_OBJECT_0) {
        if (GetOverlappedResult(pipe.handle, operation, transferred, FALSE)) {
          return true;
        }
        errno = EPIPE;
        return false;
      }
      const bool exited = wait == WAIT_OBJECT_0 + 1;
      const bool stopped =
          stop.load(std::memory_order_acquire) || IsCancelled(cancellation);
      if (exited || stopped || wait == WAIT_FAILED) {
        (void)CancelIoEx(pipe.handle, operation);
        (void)GetOverlappedResult(pipe.handle, operation, transferred, TRUE);
        errno = stopped ? ECANCELED : EPIPE;
        return false;
      }
    }
  }

  bool Launch(const char* program, const std::vector<std::string>& arguments,
              HANDLE standard_input, HANDLE standard_output,
              std::string_view name, std::string* error) {
    HANDLE standard_error = InheritableCopy(GetStdHandle(STD_ERROR_HANDLE));
    if (standard_error == nullptr) {
      standard_error = OpenNullDevice(GENERIC_WRITE);
    }
    std::array<HANDLE, 3> inherited = {standard_input, standard_output,
                                       standard_error};
    SIZE_T attribute_bytes = 0;
    (void)InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_bytes);
    std::vector<std::byte> attribute_storage(attribute_bytes);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
        attribute_storage.data());
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = standard_input;
    startup.StartupInfo.hStdOutput = standard_output;
    startup.StartupInfo.hStdError = standard_error;
    // Inherit only the three standard handles, not every inheritable handle
    // other threads of this process hold.
    bool launched =
        InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_bytes) &&
        UpdateProcThreadAttribute(
            attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited.data(),
            inherited.size() * sizeof(HANDLE), nullptr, nullptr);
    DWORD failure = GetLastError();
    PROCESS_INFORMATION information{};
    if (launched) {
      startup.lpAttributeList = attributes;
      std::wstring command_line = CommandLine(program, arguments);
      launched =
          CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, TRUE,
                         EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW,
                         nullptr, nullptr, &startup.StartupInfo, &information);
      failure = GetLastError();
      DeleteProcThreadAttributeList(attributes);
    }
    CloseOwned(&standard_error);
    if (!launched) {
      SetError(error, "cannot start " + std::string(name) + ": " +
                          SystemMessage(failure));
      return false;
    }
    CloseHandle(information.hThread);
    process = information.hProcess;
    return true;
  }
};

MediaProcess::MediaProcess() : impl_(std::make_unique<Impl>()) {}

MediaProcess::~MediaProcess() {
  CloseOwned(&impl_->output);
  for (OutboundPipe& pipe : impl_->inputs) {
    if (pipe.connect_pending) {
      (void)CancelIoEx(pipe.handle, &pipe.connect);
      DWORD ignored = 0;
      (void)GetOverlappedResult(pipe.handle, &pipe.connect, &ignored, TRUE);
    }
    CloseOwned(&pipe.handle);
    CloseOwned(&pipe.event);
  }
  if (impl_->process != nullptr && !impl_->reaped) {
    Terminate();
    int ignored = 0;
    (void)Wait(&ignored, nullptr);
  }
  CloseOwned(&impl_->process);
}

bool MediaProcess::StartCapture(const char* program,
                                std::vector<std::string> arguments,
                                std::string_view name, std::string* error) {
  SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
  HANDLE read_end = nullptr;
  HANDLE write_end = nullptr;
  if (!CreatePipe(&read_end, &write_end, &inherit, 0) ||
      !SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0)) {
    CloseOwned(&read_end);
    CloseOwned(&write_end);
    SetError(error, "cannot create media inspection pipe: " +
                        SystemMessage(GetLastError()));
    return false;
  }
  HANDLE null_input = OpenNullDevice(GENERIC_READ);
  const bool started =
      impl_->Launch(program, arguments, null_input, write_end, name, error);
  CloseOwned(&null_input);
  CloseOwned(&write_end);
  if (!started) {
    CloseOwned(&read_end);
    return false;
  }
  impl_->output = read_end;
  return true;
}

bool MediaProcess::StartWithInputs(const char* program,
                                   std::vector<std::string> arguments,
                                   std::string_view name, std::string* error) {
  static std::atomic<std::uint64_t> sequence{0};
  const std::wstring base =
      L"\\\\.\\pipe\\gufo-media-" + std::to_wstring(GetCurrentProcessId()) +
      L"-" + std::to_wstring(sequence.fetch_add(1, std::memory_order_relaxed));
  const std::array<std::wstring, 2> names = {base + L"-video",
                                             base + L"-audio"};
  for (std::size_t index = 0; index < names.size(); ++index) {
    OutboundPipe& pipe = impl_->inputs[index];
    pipe.handle = CreateNamedPipeW(
        names[index].c_str(),
        PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED |
            FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
        kPipeBufferBytes, 0, 0, nullptr);
    pipe.event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (pipe.handle == INVALID_HANDLE_VALUE || pipe.event == nullptr) {
      SetError(error, "cannot create FFmpeg media pipes: " +
                          SystemMessage(GetLastError()));
      return false;
    }
  }
  // The child reads the video from standard input, which is the client end
  // of the first pipe, opened here. FFmpeg opens the audio pipe by name.
  SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
  HANDLE video_client = CreateFileW(names[0].c_str(), GENERIC_READ, 0, &inherit,
                                    OPEN_EXISTING, 0, nullptr);
  if (video_client == INVALID_HANDLE_VALUE) {
    SetError(error, "cannot create FFmpeg media pipes: " +
                        SystemMessage(GetLastError()));
    return false;
  }
  impl_->inputs[0].connected = true;
  OutboundPipe& audio = impl_->inputs[1];
  audio.connect.hEvent = audio.event;
  if (ConnectNamedPipe(audio.handle, &audio.connect)) {
    audio.connected = true;
  } else if (GetLastError() == ERROR_PIPE_CONNECTED) {
    audio.connected = true;
  } else if (GetLastError() == ERROR_IO_PENDING) {
    audio.connect_pending = true;
  } else {
    CloseOwned(&video_client);
    SetError(error, "cannot create FFmpeg media pipes: " +
                        SystemMessage(GetLastError()));
    return false;
  }
  const std::string audio_url(names[1].begin(), names[1].end());
  for (std::string& argument : arguments) {
    if (argument == kSecondaryInputUrl) {
      argument = audio_url;
    }
  }
  HANDLE null_output = OpenNullDevice(GENERIC_WRITE);
  const bool started =
      impl_->Launch(program, arguments, video_client, null_output, name, error);
  CloseOwned(&null_output);
  CloseOwned(&video_client);
  return started;
}

long MediaProcess::Read(void* data, std::size_t bytes) {
  DWORD count = 0;
  const auto request =
      static_cast<DWORD>(bytes < 0x7FFFFFFFULL ? bytes : 0x7FFFFFFFULL);
  if (!ReadFile(impl_->output, data, request, &count, nullptr)) {
    return GetLastError() == ERROR_BROKEN_PIPE ? 0 : -1;
  }
  return static_cast<long>(count);
}

long MediaProcess::Write(Input input, const void* data, std::size_t bytes,
                         const CancellationToken* cancellation,
                         const std::atomic<bool>& stop) {
  OutboundPipe& pipe = impl_->inputs[static_cast<std::size_t>(input)];
  if (stop.load(std::memory_order_acquire) || IsCancelled(cancellation)) {
    errno = ECANCELED;
    return -1;
  }
  if (pipe.connect_pending) {
    DWORD ignored = 0;
    pipe.connect_pending = false;
    if (!impl_->Complete(pipe, &pipe.connect, &ignored, cancellation, stop)) {
      return -1;
    }
    pipe.connected = true;
  }
  OVERLAPPED operation{};
  operation.hEvent = pipe.event;
  (void)ResetEvent(pipe.event);
  const auto request =
      static_cast<DWORD>(bytes < 0x7FFFFFFFULL ? bytes : 0x7FFFFFFFULL);
  DWORD written = 0;
  if (WriteFile(pipe.handle, data, request, &written, &operation)) {
    return static_cast<long>(written);
  }
  if (GetLastError() != ERROR_IO_PENDING) {
    errno = EPIPE;
    return -1;
  }
  if (!impl_->Complete(pipe, &operation, &written, cancellation, stop)) {
    return -1;
  }
  return static_cast<long>(written);
}

void MediaProcess::CloseInput(Input input, bool drain) {
  OutboundPipe& pipe = impl_->inputs[static_cast<std::size_t>(input)];
  if (drain && pipe.connected) {
    (void)FlushFileBuffers(pipe.handle);
  }
  CloseOwned(&pipe.handle);
}

void MediaProcess::Terminate() {
  if (impl_->process != nullptr && !impl_->reaped) {
    (void)TerminateProcess(impl_->process, 1);
  }
}

bool MediaProcess::Wait(int* exit_code, std::string* error) {
  CloseOwned(&impl_->output);
  if (WaitForSingleObject(impl_->process, INFINITE) != WAIT_OBJECT_0) {
    SetError(error,
             "cannot wait for media process: " + SystemMessage(GetLastError()));
    return false;
  }
  DWORD code = 0;
  impl_->reaped = true;
  *exit_code =
      GetExitCodeProcess(impl_->process, &code) ? static_cast<int>(code) : -1;
  return true;
}

}  // namespace gufo::minimax_h3

#endif  // defined(_WIN32)
