#pragma once
// A child process attached to a pseudo terminal: ConPTY on Windows, a POSIX
// pty elsewhere.
//
// Threading: an internal reader thread copies the child's output into a
// bounded ring buffer; when it is full the reader stops reading, the OS pipe
// fills and the child blocks in write() -- backpressure reaches the producer
// instead of memory growing without bound. Input written with write() is
// queued and delivered by an internal writer thread, so write() never blocks
// the caller (a child that stops reading its input cannot stall the host);
// the queue is bounded too, and a full queue refuses input visibly (write()
// returns 0) rather than growing.
//
// Teardown (terminate() / the destructor) never deadlocks: it stops consuming
// into the ring (output is drained and discarded so the child and conhost can
// make progress), asks the child to exit, escalates to a forced kill after
// PtyConfig::terminate_grace, reaps it, and wakes every internal thread
// regardless of what the child is doing.

#include "bropty/ring_buffer.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bropty {

struct PtySize {
    int cols{80};
    int rows{24};
    int pixel_width{0};
    int pixel_height{0};
};

struct PtyConfig {
    // Program to run. Empty: $SHELL (else /bin/sh) on POSIX, %COMSPEC% (else
    // cmd.exe) on Windows. A name without a path separator is looked up on
    // the child's PATH.
    std::string command;
    std::vector<std::string> args;  // argv[1..]; quoted for Windows as CommandLineToArgvW parses
    // Windows only: when non-empty, the exact command line passed to
    // CreateProcess (command/args are then ignored). For programs such as
    // cmd.exe whose own parsing differs from the C runtime's.
    std::string windows_command_line;
    std::string cwd;  // empty: inherit

    // The child's environment: the parent's (when inherit_env) minus the
    // variables describing the parent's own terminal (COLUMNS, LINES,
    // TERM_PROGRAM, WT_SESSION, ...), with TERM=xterm-256color and
    // COLORTERM=truecolor, then `env` entries applied in order (later wins),
    // then `env_unset` removed. Names compare case-insensitively on Windows.
    bool inherit_env{true};
    std::vector<std::pair<std::string, std::string>> env;
    std::vector<std::string> env_unset;

    PtySize size{80, 24, 0, 0};

    // Output buffered between the child and the consumer; the backpressure bound.
    size_t output_buffer_bytes{1u << 20};
    // Input queued for the child but not yet accepted by the pty; the bound
    // write() / write_some() enforce (see IPtyProcess::write).
    size_t input_buffer_bytes{1u << 20};

    // Which processes the session's teardown takes with it.
    //   Tree (default): everything the child started, however it detached.
    //     Windows: the child is created inside a job object that kills every
    //     member when terminate() runs (and when the last handle closes, so a
    //     crashed host leaks nothing). A program that legitimately outlives its
    //     terminal must say so by creating its process with
    //     CREATE_BREAKAWAY_FROM_JOB, which the job permits; nothing else
    //     escapes. POSIX: what a real terminal hangup does -- the child's
    //     process group (it is a session leader) gets SIGHUP, escalating to
    //     SIGTERM and SIGKILL while the child itself lives. A descendant that
    //     left the group (setsid / setpgid: daemons, an interactive shell's
    //     background jobs) or ignores SIGHUP after the child has gone (nohup)
    //     has opted out, exactly as when any terminal window closes.
    //   Console: Windows only -- no job; only the console's attached clients
    //     are closed, so a grandchild that detached from the console (or a GUI
    //     program started from the shell) survives. POSIX treats it as Tree.
    enum class ProcessTree : uint8_t { Tree, Console };
    ProcessTree process_tree{ProcessTree::Tree};
    // Each escalation step of terminate() waits this long for the child to exit
    // (POSIX: SIGHUP, then SIGTERM, then SIGKILL; Windows: close the console,
    // then TerminateProcess).
    std::chrono::milliseconds terminate_grace{500};
};

class IPtyProcess {
public:
    virtual ~IPtyProcess() = default;

    // Start the child. Returns false (see last_error()) if the pty could not be
    // created or the program could not be started -- including, on POSIX, an
    // exec failure such as a missing program. A process can be spawned once.
    virtual bool spawn(const PtyConfig& config) = 0;
    [[nodiscard]] virtual const std::string& last_error() const = 0;

    // Input. The queue towards the child is bounded by
    // PtyConfig::input_buffer_bytes; neither call ever blocks.
    //  write():      all or nothing -- returns data.size() if the whole of it
    //                was queued, 0 if it does not fit (or the child is gone).
    //                For units that must not be split (a key's encoding).
    //  write_some(): queues the prefix that fits and returns its length.
    // After a write that queued less than asked, the wakeup hook fires once
    // the queue has drained to half its capacity (or the child is gone), so
    // the host can retry from its event loop instead of polling.
    virtual size_t write(std::string_view data) = 0;
    virtual size_t write_some(std::string_view data) = 0;
    // Bytes queued but not yet delivered to the pty, and the room left.
    [[nodiscard]] virtual size_t pending_input() const = 0;
    [[nodiscard]] virtual size_t input_space() const = 0;

    // Output. read() blocks until data or EOF (returns 0 at EOF); read_timeout()
    // returns 0 on timeout too; read_nonblocking() returns what is buffered.
    // EOF means the output side closed: on Windows that happens only once the
    // child has exited and its console was closed (done automatically).
    virtual size_t read(void* dst, size_t max_bytes) = 0;
    virtual size_t read_timeout(void* dst, size_t max_bytes, std::chrono::milliseconds timeout) = 0;
    virtual size_t read_nonblocking(void* dst, size_t max_bytes) = 0;
    [[nodiscard]] virtual size_t available() const = 0;
    // True once the child has exited and every byte of its output has been read.
    [[nodiscard]] virtual bool eof() const = 0;

    virtual bool resize(const PtySize& size) = 0;
    bool resize(int cols, int rows) { return resize(PtySize{cols, rows, 0, 0}); }

    // Child state. exit_code() is empty while it runs; afterwards it is the
    // exit status (POSIX: 128 + signal number when killed by a signal).
    // Neither call reaps or otherwise consumes the status.
    //
    // POSIX status collection does not depend on the host leaving the child
    // alone (src/pty_posix_child.h has the details). Linux (x86-64, AArch64):
    // the child is forked by a small supervisor process that shares the
    // host's memory and has no exit signal, so the child is not the host's:
    // waitpid(-1) / wait() / a SIGCHLD handler never see it, SIGCHLD =
    // SIG_IGN does not auto-reap it, and its pid cannot be reused before
    // teardown. The supervisor appears in `ps` as a second instance of the
    // host program, as the child's parent. macOS: the status is taken from a
    // kqueue EVFILT_PROC NOTE_EXIT|NOTE_EXITSTATUS event, which the kernel
    // delivers whether or not something else reaps the child first. What
    // remains the host's obligation: on macOS, a host that reaps every child
    // lets the pid be recycled before bropty's waiter has noticed the exit (a
    // window of microseconds in which terminate() could signal a stranger);
    // on other POSIX systems and Linux architectures bropty falls back to
    // fork() + waitpid(pid), where such a host loses bropty the status
    // (reported as -1).
    [[nodiscard]] virtual bool is_running() const = 0;
    [[nodiscard]] virtual std::optional<int> exit_code() const = 0;
    [[nodiscard]] virtual int64_t pid() const = 0;

    // Wait for the child to exit (and its output to be fully collected).
    virtual void wait() = 0;
    virtual bool wait_for(std::chrono::milliseconds timeout) = 0;
    // Stop the child (escalating; see PtyConfig::terminate_grace) and release
    // everything. Idempotent; the destructor calls it. Output not yet read is
    // still readable afterwards. Bounded: every internal thread is joined
    // before it returns, even when the child cannot die (a process stuck in
    // uninterruptible sleep survives SIGKILL). Such a child is handed to one
    // process-wide reaper thread, which collects it whenever it finally exits
    // (see pty_detail::orphans_pending()).
    virtual void terminate() = 0;

    // Called from the reader thread whenever output arrives and when the child
    // exits: a hook to wake the host's event loop. It can fire often;
    // coalesce. Set it before spawn().
    virtual void set_wakeup(std::function<void()> fn) = 0;

    [[nodiscard]] virtual ByteRingBuffer& ring_buffer() = 0;
};

// Creates a platform-appropriate PTY instance (ConPTY on Windows, POSIX PTY on Linux/macOS)
[[nodiscard]] std::unique_ptr<IPtyProcess> create_pty();

// Building blocks, exposed for testing.
namespace pty_detail {
// One argument quoted so CommandLineToArgvW / the MSVC CRT parse it back verbatim.
std::string quote_windows_arg(std::string_view arg);
// command + args joined into a Windows command line.
std::string windows_command_line(std::string_view command, const std::vector<std::string>& args);
// Merge `base` (NAME=value strings) with the config's overrides, unsets and
// TERM/COLORTERM defaults. Result is sorted by name (case-insensitively when
// `case_insensitive`), as Windows environment blocks must be.
std::vector<std::string> build_environment(const std::vector<std::string>& base, const PtyConfig& config,
                                           bool case_insensitive);
// Children that outlived terminate() and still await collection by the
// process-wide reaper (POSIX; always 0 on Windows, where a process handle
// needs no reaping).
size_t orphans_pending();
// Test seam: when set, terminate() sends no signals (POSIX) / does not kill
// (Windows), which is how a child that cannot die is simulated in tests.
void set_test_suppress_kill(bool on);
} // namespace pty_detail

} // namespace bropty
