#pragma once
// A child process attached to a pseudo terminal: ConPTY on Windows, a POSIX
// pty elsewhere.
//
// Threading: an internal reader thread copies the child's output into a
// bounded ring buffer; when it is full the reader stops reading, the OS pipe
// fills and the child blocks in write() -- backpressure reaches the producer
// instead of memory growing without bound. Input written with write() is
// queued and delivered by an internal writer thread, so write() never blocks
// the caller (a child that stops reading its input cannot stall the host).
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

    // Queue bytes for the child's input. Never blocks; returns data.size()
    // (or 0 if the child is gone).
    virtual size_t write(std::string_view data) = 0;
    // Bytes queued but not yet delivered to the pty.
    [[nodiscard]] virtual size_t pending_input() const = 0;

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
    [[nodiscard]] virtual bool is_running() const = 0;
    [[nodiscard]] virtual std::optional<int> exit_code() const = 0;
    [[nodiscard]] virtual int64_t pid() const = 0;

    // Wait for the child to exit (and its output to be fully collected).
    virtual void wait() = 0;
    virtual bool wait_for(std::chrono::milliseconds timeout) = 0;
    // Stop the child (escalating; see PtyConfig::terminate_grace) and release
    // everything. Idempotent; the destructor calls it. Output not yet read is
    // still readable afterwards.
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
} // namespace pty_detail

} // namespace bropty
