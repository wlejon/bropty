// POSIX pty backend.
//
// Spawning: everything the child needs (argv, envp, the resolved program
// path, the fd limit) is prepared before fork(); between fork() and execve()
// the child only makes async-signal-safe calls -- no allocation, no setenv,
// no PATH search -- because the host is multi-threaded. Exec failure is
// reported back through a close-on-exec pipe, so spawn() fails on a missing
// program instead of leaving a child that printed nothing and exited 127.
//
// Threads: a reader (master -> ring, poll()ing a wake pipe too), a writer
// (input queue -> non-blocking master) and a waiter that alone observes the
// child's exit (pty_posix_child.h: through a supervisor on Linux, a kqueue on
// macOS) -- so is_running()/exit_code() never reap, and a host reaping
// children of its own cannot take the status.
//
// Teardown: SIGHUP to the child's process group (it is a session leader),
// then SIGTERM, then SIGKILL, each after PtyConfig::terminate_grace; the
// wake pipe unblocks the reader, writer and waiter whatever the child is
// doing, so all three are joined. A child that survives SIGKILL (stuck in
// uninterruptible sleep) goes to the process-wide reaper.
#if !defined(_WIN32)

#include "pty_base.h"
#include "pty_posix_child.h"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <crt_externs.h>
#define BROPTY_ENVIRON (*_NSGetEnviron())
#else
extern char** environ;
#define BROPTY_ENVIRON environ
#endif

namespace bropty {

namespace {

std::string errno_text(const char* what, int e = errno) { return std::string(what) + ": " + std::strerror(e); }

[[maybe_unused]] void set_cloexec(int fd) {  // where O_CLOEXEC / pipe2 are unavailable
    int f = fcntl(fd, F_GETFD);
    if (f >= 0) fcntl(fd, F_SETFD, f | FD_CLOEXEC);
}

bool make_pipe(int fds[2]) {
#if defined(__linux__)
    return pipe2(fds, O_CLOEXEC) == 0;
#else
    if (pipe(fds) != 0) return false;
    set_cloexec(fds[0]);
    set_cloexec(fds[1]);
    return true;
#endif
}

void close_fd(int& fd) {
    if (fd >= 0) ::close(fd);
    fd = -1;
}

std::string env_value(const std::vector<std::string>& env, std::string_view name) {
    for (const std::string& e : env) {
        if (e.size() > name.size() && e.compare(0, name.size(), name) == 0 && e[name.size()] == '=')
            return e.substr(name.size() + 1);
    }
    return std::string();
}

// execvp's lookup, done in the parent against the child's PATH.
std::string resolve_program(const std::string& command, const std::vector<std::string>& env) {
    if (command.find('/') != std::string::npos) return command;
    const std::string from_env = env_value(env, "PATH");
    const std::string path = from_env.empty() ? std::string("/usr/local/bin:/usr/bin:/bin") : from_env;
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find(':', start);
        if (end == std::string::npos) end = path.size();
        // An empty PATH element means the current directory.
        std::string candidate = end > start ? path.substr(start, end - start) : std::string(1, '.');
        candidate += '/';
        candidate += command;
        struct stat st {};
        if (::stat(candidate.c_str(), &st) == 0 && S_ISREG(st.st_mode) && ::access(candidate.c_str(), X_OK) == 0)
            return candidate;
        start = end + 1;
    }
    return std::string();
}

struct winsize winsize_of(const PtySize& s) {
    struct winsize ws {};
    ws.ws_col = static_cast<unsigned short>(std::clamp(s.cols, 1, 65535));
    ws.ws_row = static_cast<unsigned short>(std::clamp(s.rows, 1, 65535));
    ws.ws_xpixel = static_cast<unsigned short>(std::clamp(s.pixel_width, 0, 65535));
    ws.ws_ypixel = static_cast<unsigned short>(std::clamp(s.pixel_height, 0, 65535));
    return ws;
}

// Child-side failure report: which step, and errno. Fixed-size, written with
// one write(2).
struct ChildError {
    int step;
    int err;
};
enum ChildStep : int { Step_Setsid = 1, Step_Ctty, Step_Dup, Step_Chdir, Step_Exec };

const char* step_name(int s) {
    switch (s) {
    case Step_Setsid: return "setsid";
    case Step_Ctty: return "TIOCSCTTY";
    case Step_Dup: return "dup2";
    case Step_Chdir: return "chdir";
    default: return "exec";
    }
}

[[noreturn]] void child_fail(int fd, int step) {
    ChildError e{step, errno};
    ssize_t r = ::write(fd, &e, sizeof e);
    (void)r;
    _exit(127);
}

// Everything the child needs, prepared by the parent.
struct ChildArgs {
    const char* program;
    char* const* argv;
    char* const* envp;
    const char* cwd;
    int slave;
    int errpipe;
    int max_fd;
};

// The child, between fork and exec: async-signal-safe calls only.
[[noreturn]] void child_main(void* p) {
    const ChildArgs& a = *static_cast<const ChildArgs*>(p);
    for (int s = 1; s < NSIG; ++s) {
        struct sigaction sa {};
        sa.sa_handler = SIG_DFL;
        sigaction(s, &sa, nullptr);  // fails harmlessly for SIGKILL/SIGSTOP/invalid
    }
    if (setsid() < 0) child_fail(a.errpipe, Step_Setsid);
    if (ioctl(a.slave, TIOCSCTTY, 0) < 0) child_fail(a.errpipe, Step_Ctty);
    if (dup2(a.slave, 0) < 0 || dup2(a.slave, 1) < 0 || dup2(a.slave, 2) < 0) child_fail(a.errpipe, Step_Dup);
    // Nothing else of the host's leaks into the child (errpipe is already
    // close-on-exec; marking rather than closing keeps it usable here).
    bool marked = false;
#if defined(__linux__) && defined(SYS_close_range)
    marked = syscall(SYS_close_range, 3u, ~0u, 4u /* CLOSE_RANGE_CLOEXEC */) == 0;
#endif
    if (!marked) {
        for (int fd = 3; fd < a.max_fd; ++fd) {
            int f = fcntl(fd, F_GETFD);
            if (f >= 0 && !(f & FD_CLOEXEC)) fcntl(fd, F_SETFD, f | FD_CLOEXEC);
        }
    }
    if (a.cwd && chdir(a.cwd) != 0) child_fail(a.errpipe, Step_Chdir);
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, nullptr);
    execve(a.program, a.argv, a.envp);
    child_fail(a.errpipe, Step_Exec);
}

} // namespace

class PtyPosix final : public pty_detail::PtyBase {
public:
    PtyPosix() = default;
    ~PtyPosix() override { terminate(); }

    bool spawn(const PtyConfig& config) override;
    bool resize(const PtySize& size) override;
    int64_t pid() const override { return pid_; }
    void terminate() override;

private:
    void reader_main();
    void writer_main();
    bool signal_and_wait(int sig, std::chrono::milliseconds grace);

    int master_{-1};
    int wake_[2]{-1, -1};
    pid_t pid_{-1};
    pty_detail::ChildProc child_;
    std::thread reader_, writer_, waiter_;
    std::once_flag teardown_;
};

bool PtyPosix::spawn(const PtyConfig& config) {
    if (!begin_spawn(config)) return false;

    // ---- everything the child needs, prepared while allocation is allowed
    std::vector<std::string> base;
    for (char** e = BROPTY_ENVIRON; e && *e; ++e) base.emplace_back(*e);
    std::vector<std::string> env = pty_detail::build_environment(base, config, false);

    std::string command = config.command;
    if (command.empty()) {
        command = env_value(env, "SHELL");
        if (command.empty()) command = "/bin/sh";
    }
    std::string program = resolve_program(command, env);
    if (program.empty()) return fail("spawn: " + command + ": command not found");

    std::vector<std::string> argv_store;
    argv_store.push_back(command);
    argv_store.insert(argv_store.end(), config.args.begin(), config.args.end());
    std::vector<char*> argv;
    for (std::string& a : argv_store) argv.push_back(a.data());
    argv.push_back(nullptr);
    std::vector<char*> envp;
    for (std::string& e : env) envp.push_back(e.data());
    envp.push_back(nullptr);
    const char* cwd = config.cwd.empty() ? nullptr : config.cwd.c_str();

    struct rlimit rl {};
    int max_fd = (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY) ? int(rl.rlim_cur) : 4096;
    max_fd = std::min(max_fd, 65536);

    // ---- the pty pair, both ends close-on-exec (the child dup2s the slave)
#if defined(__linux__)
    master_ = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
#else
    master_ = posix_openpt(O_RDWR | O_NOCTTY);
    if (master_ >= 0) set_cloexec(master_);
#endif
    if (master_ < 0) return fail(errno_text("posix_openpt"));
    if (grantpt(master_) != 0 || unlockpt(master_) != 0) {
        int e = errno;
        close_fd(master_);
        return fail(errno_text("grantpt/unlockpt", e));
    }
    std::string slave_name;
    {
#if defined(__linux__)
        char name[128];
        if (ptsname_r(master_, name, sizeof name) == 0) slave_name = name;
#elif defined(__APPLE__) && defined(TIOCPTYGNAME)
        // What Darwin's ptsname() does internally, minus its static buffer
        // (which another library in the host may be using concurrently).
        char name[128] = {};
        if (ioctl(master_, TIOCPTYGNAME, name) == 0) slave_name = name;
#else
        static std::mutex ptsname_mu;  // ptsname() uses a static buffer
        std::lock_guard<std::mutex> lock(ptsname_mu);
        if (const char* n = ptsname(master_)) slave_name = n;
#endif
    }
    if (slave_name.empty()) {
        close_fd(master_);
        return fail("ptsname failed");
    }
    int slave = ::open(slave_name.c_str(), O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (slave < 0) {
        int e = errno;
        close_fd(master_);
        return fail(errno_text("open slave", e));
    }
    struct termios tio {};
    if (tcgetattr(slave, &tio) == 0) {
#if defined(IUTF8)
        tio.c_iflag |= IUTF8;  // canonical-mode erase removes whole UTF-8 characters
#endif
        tcsetattr(slave, TCSANOW, &tio);
    }
    struct winsize ws = winsize_of(config.size);
    ioctl(slave, TIOCSWINSZ, &ws);

    int errpipe[2];
    if (!make_pipe(errpipe) || !make_pipe(wake_)) {
        int e = errno;
        ::close(slave);
        close_fd(master_);
        return fail(errno_text("pipe", e));
    }

    sigset_t all, old;
    sigfillset(&all);
    // Block signals across fork so no host handler runs in the child before
    // dispositions are reset.
    pthread_sigmask(SIG_SETMASK, &all, &old);
    ChildArgs args{program.c_str(), argv.data(), envp.data(), cwd, slave, errpipe[1], max_fd};
    pid_t pid = pty_detail::spawn_child(child_, &child_main, &args);
    int fork_errno = errno;
    pthread_sigmask(SIG_SETMASK, &old, nullptr);
    ::close(slave);
    ::close(errpipe[1]);
    if (pid < 0) {
        ::close(errpipe[0]);
        close_fd(master_);
        close_fd(wake_[0]);
        close_fd(wake_[1]);
        return fail(errno_text("fork", fork_errno));
    }

    ChildError ce{};
    ssize_t got;
    do {
        got = ::read(errpipe[0], &ce, sizeof ce);
    } while (got < 0 && errno == EINTR);
    ::close(errpipe[0]);
    if (got == ssize_t(sizeof ce)) {
        pty_detail::reap_blocking(child_);
        close_fd(master_);
        close_fd(wake_[0]);
        close_fd(wake_[1]);
        return fail(errno_text(step_name(ce.step), ce.err) + " (" + command + ")");
    }

    pid_ = pid;
    fcntl(master_, F_SETFL, fcntl(master_, F_GETFL) | O_NONBLOCK);
    spawned_ = true;
    reader_ = std::thread(&PtyPosix::reader_main, this);
    writer_ = std::thread(&PtyPosix::writer_main, this);
    // Woken through wake_ at teardown, so it is always joined.
    waiter_ = std::thread([this] {
        if (pty_detail::wait_exit(child_, wake_[0], *exit_)) notify();
    });
    return true;
}

void PtyPosix::reader_main() {
    std::vector<char> buf(64 * 1024);
    for (;;) {
        struct pollfd fds[2] = {{master_, POLLIN, 0}, {wake_[0], POLLIN, 0}};
        int r = poll(fds, 2, -1);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (fds[1].revents) break;
        if (!fds[0].revents) continue;
        ssize_t n = ::read(master_, buf.data(), buf.size());
        if (n > 0) {
            deliver_output(buf.data(), size_t(n));
        } else if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
            continue;
        } else {
            break;  // EOF / EIO: every slave descriptor is closed
        }
    }
    output_finished();
}

void PtyPosix::writer_main() {
    std::string chunk;
    while (take_input(chunk, 64 * 1024)) {
        size_t off = 0;
        while (off < chunk.size()) {
            ssize_t n = ::write(master_, chunk.data() + off, chunk.size() - off);
            if (n > 0) {
                off += size_t(n);
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && errno == EAGAIN) {
                struct pollfd fds[2] = {{master_, POLLOUT, 0}, {wake_[0], POLLIN, 0}};
                if (poll(fds, 2, -1) < 0 && errno != EINTR) return;
                if (fds[1].revents) return;
                continue;
            }
            stop_input();
            return;
        }
        chunk.clear();
    }
}

bool PtyPosix::resize(const PtySize& size) {
    if (master_ < 0 || exit_->done()) return false;
    struct winsize ws = winsize_of(size);
    return ioctl(master_, TIOCSWINSZ, &ws) == 0;  // the kernel sends SIGWINCH
}

bool PtyPosix::signal_and_wait(int sig, std::chrono::milliseconds grace) {
    if (exit_->done()) return true;
    if (pty_detail::test_suppress_kill()) return exit_->wait_for(grace);
    // The child leads its own session and process group; signal the group so
    // jobs it started go too, and the child itself in case it moved groups.
    ::kill(-pid_, sig);
    ::kill(pid_, sig);
    return exit_->wait_for(grace);
}

void PtyPosix::terminate() {
    if (!spawned_) return;
    std::call_once(teardown_, [this] {
        ring_->close();  // unblock a reader waiting for ring space; drop further output
        stop_input();
        // SIGKILL normally takes effect at once; a child still there after
        // the last wait is stuck in the kernel and is left to the reaper.
        if (!signal_and_wait(SIGHUP, config_.terminate_grace) && !signal_and_wait(SIGTERM, config_.terminate_grace))
            signal_and_wait(SIGKILL, std::max<std::chrono::milliseconds>(config_.terminate_grace,
                                                                         std::chrono::milliseconds(2000)));
        char b = 1;
        ssize_t r = ::write(wake_[1], &b, 1);
        (void)r;
        if (reader_.joinable()) reader_.join();
        if (writer_.joinable()) writer_.join();
        if (waiter_.joinable()) waiter_.join();
        if (exit_->done()) pty_detail::release_exited(child_);
        else pty_detail::hand_to_reaper(child_, exit_);
        child_ = pty_detail::ChildProc{};
        output_done_ = true;
        close_fd(master_);
        close_fd(wake_[0]);
        close_fd(wake_[1]);
    });
}

std::unique_ptr<IPtyProcess> create_pty_posix() { return std::make_unique<PtyPosix>(); }

} // namespace bropty

#endif // !_WIN32
