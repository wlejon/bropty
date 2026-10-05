#pragma once
// POSIX child creation and exit collection that do not depend on the host
// leaving bropty's children alone, and a process-wide reaper for children
// that outlive their PtyProcess.
//
//  Linux   The pty child is not the host's child. A supervisor -- a process
//          created with clone(CLONE_VM) on a small stack of its own, sharing
//          the host's memory (so no copy-on-write cost) and with no exit
//          signal -- forks the pty child, waits for it and reports its pid and
//          exit status through a pipe. Because the supervisor never execs, it
//          stays a "clone child": the kernel sends the host no SIGCHLD for it,
//          and waitpid(-1) / wait() / a SIGCHLD handler never see it; the pty
//          child is the supervisor's child, invisible to them too. SIGCHLD =
//          SIG_IGN in the host does not auto-reap it either (the supervisor
//          resets its own copy of the disposition). The supervisor observes
//          the exit with WNOWAIT and keeps the zombie until bropty releases it
//          at teardown, so the pid and process group id cannot be reused while
//          bropty may still signal them. The supervisor runs only raw system
//          calls with every signal blocked. x86-64 and AArch64; elsewhere the
//          fallback below.
//  macOS   fork(), then a kqueue EVFILT_PROC NOTE_EXIT | NOTE_EXITSTATUS
//          registered before the child is released to exec (it waits on a
//          pipe): the kernel posts the wait status with the event even when
//          something else reaps the child first.
//  Other   fork() + waitpid(pid, WNOHANG) polled at a short interval; a host
//          that reaps every child loses bropty the status (reported as -1).
//
// Every waiter can be woken through a file descriptor, so no thread ever has
// to be detached.

#include "pty_base.h"

#include <cstddef>
#include <memory>
#include <sys/types.h>

namespace bropty::pty_detail {

struct ChildProc {
    pid_t pid{-1};          // the pty child
    // Linux supervisor
    pid_t super{-1};
    int status_fd{-1};      // supervisor -> host: int32 pid, then int32 exit code
    int release_fd{-1};     // host -> supervisor: closing it lets it reap and exit
    void* stack{nullptr};
    size_t stack_size{0};
    // macOS
    int kq{-1};
};

using ChildMain = void (*)(void* arg);  // must not return (exec or _exit)

// Start the pty child running `fn(arg)`. Returns its pid, or -1 with errno
// set. Call with all signals blocked; `arg` must stay valid until it returns.
pid_t spawn_child(ChildProc& c, ChildMain fn, void* arg);

// Waiter-thread body: block until the child exits (records its status in
// `st`, returns true) or `wake_fd` becomes readable (returns false).
bool wait_exit(ChildProc& c, int wake_fd, ExitState& st);

// Collect a child known to be exiting now (exec failure). Blocking.
void reap_blocking(ChildProc& c);
// Final release of an exited child: let its zombie go, free the supervisor.
void release_exited(ChildProc& c);
// A child that would not die in time: the process-wide reaper takes it (and
// records its status in `st` when it finally exits).
void hand_to_reaper(ChildProc c, std::shared_ptr<ExitState> st);

// Decoded exit status: exit code, or 128 + signal; -1 if unknown.
int decode_wait_status(int status);

} // namespace bropty::pty_detail
