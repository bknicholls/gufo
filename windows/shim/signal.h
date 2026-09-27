#ifndef GUFO_WINDOWS_SHIM_SIGNAL_H_
#define GUFO_WINDOWS_SHIM_SIGNAL_H_

#include <errno.h>
#include_next <signal.h>

// Windows has no SIGPIPE: a write to a closed pipe or socket fails with an
// error instead. Handlers installed for it are accepted and never run.
#define SIGPIPE 13

#define SA_RESTART 0

typedef int sigset_t;

struct sigaction {
  void (*sa_handler)(int);
  sigset_t sa_mask;
  int sa_flags;
};

inline int sigemptyset(sigset_t* set) {
  *set = 0;
  return 0;
}

// sigaction over the CRT's signal(). The CRT restores the default handler
// before a handler runs, so a handler that must see repeated signals has to
// reinstall itself; the one-shot shutdown and cancellation handlers here do
// not.
inline int sigaction(int number, const struct sigaction* action,
                     struct sigaction* previous) {
  if (number == SIGPIPE) {
    if (previous != nullptr)
      *previous = {SIG_DFL, 0, 0};
    return 0;
  }
  void (*old)(int) = SIG_DFL;
  if (action != nullptr) {
    old = signal(number, action->sa_handler);
  } else {
    old = signal(number, SIG_DFL);
    if (old != SIG_ERR)
      (void)signal(number, old);
  }
  if (old == SIG_ERR) {
    errno = EINVAL;
    return -1;
  }
  if (previous != nullptr)
    *previous = {old, 0, 0};
  return 0;
}

#endif  // GUFO_WINDOWS_SHIM_SIGNAL_H_
