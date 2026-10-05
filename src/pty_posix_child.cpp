// POSIX child creation, exit collection and the orphan reaper; see
// pty_posix_child.h.
#if !defined(_WIN32)

#include "pty_posix_child.h"

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <sys/event.h>
#include <sys/time.h>
#endif

#if defined(__linux__)
#include <sched.h>
#endif

#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
#define BROPTY_SUPERVISOR 1
#endif

namespace bropty::pty_detail {

namespace {

void close_fd(int& fd) {
    if (fd >= 0) ::close(fd);
    fd = -1;
}

// Read exactly 4 bytes (EINTR-safe). False on EOF / error.
bool read_i32(int fd, int32_t& v) {
    char* p = reinterpret_cast<char*>(&v);
    size_t got = 0;
    while (got < sizeof v) {
        ssize_t n = ::read(fd, p + got, sizeof v - got);
        if (n > 0) got += size_t(n);
        else if (n < 0 && errno == EINTR) continue;
        else return false;
    }
    return true;
}

#if defined(BROPTY_SUPERVISOR)
// ---- the Linux supervisor ---------------------------------------------------
// It shares the host's address space (and, without CLONE_SETTLS, the TLS of
// the thread that created it), so it must not touch errno or any libc state:
// raw system calls only.

inline long raw_syscall(long n, long a = 0, long b = 0, long c = 0, long d = 0, long e = 0) {
#if defined(__x86_64__)
    long ret;
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8)
                     : "rcx", "r11", "memory");
    return ret;
#else
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = d;
    register long x4 __asm__("x4") = e;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4) : "memory", "cc");
    return x0;
#endif
}

#ifndef SYS_close_range
#define SYS_close_range 436
#endif
#ifndef __WCLONE
#define __WCLONE 0x80000000
#endif

struct KernelSigaction {  // struct sigaction as rt_sigaction takes it (x86-64, AArch64)
    unsigned long handler;
    unsigned long flags;
    unsigned long restorer;
    unsigned long mask;
};

struct SuperArgs {
    ChildMain fn;
    void* arg;
    int status_w;
    int release_r;
    int max_fd;
};

int32_t code_from_siginfo(const siginfo_t& si) {
    switch (si.si_code) {
    case CLD_EXITED: return si.si_status;
    case CLD_KILLED:
    case CLD_DUMPED: return 128 + si.si_status;
    default: return -1;
    }
}

void raw_write_i32(int fd, int32_t v) { raw_syscall(SYS_write, fd, long(&v), sizeof v); }

void raw_close_except(int keep1, int keep2, int max_fd) {
    int lo = keep1 < keep2 ? keep1 : keep2;
    int hi = keep1 < keep2 ? keep2 : keep1;
    auto range = [&](int first, int last) {  // [first, last]
        if (first > last) return;
        if (raw_syscall(SYS_close_range, first, last, 0) == 0) return;
        for (int fd = first; fd <= last && fd < max_fd; ++fd) raw_syscall(SYS_close, fd);
    };
    range(0, lo - 1);
    range(lo + 1, hi - 1);
    range(hi + 1, 0x7FFFFFFF);
}

int supervisor_main(void* p) {
    const SuperArgs a = *static_cast<const SuperArgs*>(p);
    // The host may ignore SIGCHLD (which would auto-reap the pty child):
    // this process's copy of the disposition goes back to the default.
    KernelSigaction dfl{};
    raw_syscall(SYS_rt_sigaction, SIGCHLD, long(&dfl), 0, 8);
    long pid = raw_syscall(SYS_clone, SIGCHLD, 0, 0, 0, 0);  // fork()
    if (pid == 0) a.fn(a.arg);                               // the pty child; never returns
    raw_write_i32(a.status_w, int32_t(pid));                 // pid, or -errno
    raw_close_except(a.status_w, a.release_r, a.max_fd);
    if (pid < 0) raw_syscall(SYS_exit, 1);
    siginfo_t si;
    for (;;) {
        std::memset(&si, 0, sizeof si);
        long r = raw_syscall(SYS_waitid, P_PID, pid, long(&si), WEXITED | WNOWAIT, 0);
        if (r == 0 && si.si_pid == pid) break;
        if (r < 0 && r != -EINTR) {
            si.si_code = 0;
            break;
        }
    }
    raw_write_i32(a.status_w, code_from_siginfo(si));
    char b;
    while (raw_syscall(SYS_read, a.release_r, long(&b), 1) == -EINTR) {
    }
    raw_syscall(SYS_wait4, pid, 0, 0, 0);  // the pid may be reused from here on
    raw_syscall(SYS_exit, 0);
    return 0;
}

constexpr size_t kSuperStack = 256u << 10;

// Returns true with c.pid set (or c.pid = -1, errno) when the supervisor ran;
// false when it could not be started (fall back to fork()).
bool spawn_supervised(ChildProc& c, ChildMain fn, void* arg) {
    int sp[2], rp[2];
    if (pipe2(sp, O_CLOEXEC) != 0) return false;
    if (pipe2(rp, O_CLOEXEC) != 0) {
        ::close(sp[0]);
        ::close(sp[1]);
        return false;
    }
    void* stack = mmap(nullptr, kSuperStack, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (stack == MAP_FAILED) {
        for (int fd : {sp[0], sp[1], rp[0], rp[1]}) ::close(fd);
        return false;
    }
    SuperArgs a{fn, arg, sp[1], rp[0], 65536};
    // No exit signal: the host is never told about it and wait(-1) skips it.
    int tid = ::clone(supervisor_main, static_cast<char*>(stack) + kSuperStack, CLONE_VM, &a);
    ::close(sp[1]);
    ::close(rp[0]);
    if (tid < 0) {
        ::close(sp[0]);
        ::close(rp[1]);
        munmap(stack, kSuperStack);
        return false;
    }
    c.super = tid;
    c.status_fd = sp[0];
    c.release_fd = rp[1];
    c.stack = stack;
    c.stack_size = kSuperStack;
    int32_t pid = -1;
    if (!read_i32(c.status_fd, pid) || pid < 0) {
        int e = pid < 0 ? -pid : EAGAIN;
        release_exited(c);
        c.pid = -1;
        errno = e;
        return true;
    }
    c.pid = pid;
    return true;
}
#endif  // BROPTY_SUPERVISOR

// Non-blocking: has the child exited? Records the status when it has.
// `reap`: also collect the zombie (fork/kqueue paths).
bool check_exit(ChildProc& c, ExitState& st, bool reap) {
    (void)reap;  // only the kqueue path can defer reaping
    if (c.status_fd >= 0) {
        pollfd p{c.status_fd, POLLIN, 0};
        if (poll(&p, 1, 0) <= 0) return false;
        int32_t code = -1;
        if (!read_i32(c.status_fd, code)) code = -1;  // supervisor gone without a word
        st.set(code);
        return true;
    }
#if defined(__APPLE__)
    if (c.kq >= 0) {
        struct kevent ev {};
        struct timespec zero {};
        int n = kevent(c.kq, nullptr, 0, &ev, 1, &zero);
        if (n == 1 && ev.filter == EVFILT_PROC && (ev.fflags & NOTE_EXIT)) {
            st.set(decode_wait_status(int(ev.data)));
            if (reap) {
                int status;
                while (waitpid(c.pid, &status, 0) < 0 && errno == EINTR) {
                }
            }
            return true;
        }
        return false;
    }
#endif
    int status = 0;
    pid_t r;
    do {
        r = waitpid(c.pid, &status, WNOHANG);
    } while (r < 0 && errno == EINTR);
    if (r == c.pid) {
        st.set(decode_wait_status(status));
        return true;
    }
    if (r < 0) {  // someone else reaped it: the status is lost
        st.set(-1);
        return true;
    }
    return false;
}

// The fd that becomes readable when the child exits, or -1 (polled fallback).
int exit_fd(const ChildProc& c) {
    if (c.status_fd >= 0) return c.status_fd;
    if (c.kq >= 0) return c.kq;
    return -1;
}

// ---- the process-wide reaper ------------------------------------------------

struct Orphan {
    ChildProc c;
    std::shared_ptr<ExitState> st;
};

class Reaper {
public:
    static Reaper& get() {
        static Reaper* r = new Reaper();  // never destroyed: its thread may outlive statics
        return *r;
    }
    void add(Orphan o) {
        std::lock_guard<std::mutex> lock(mu_);
        list_.push_back(std::move(o));
        if (!running_) {
            running_ = true;
            // Runs only while there are orphans, then exits on its own.
            std::thread([this] { run(); }).detach();
        } else {
            char b = 1;
            ssize_t r = ::write(wake_[1], &b, 1);
            (void)r;
        }
    }
    size_t pending() {
        std::lock_guard<std::mutex> lock(mu_);
        return list_.size();
    }

private:
    Reaper() {
        if (::pipe(wake_) == 0) {
            for (int fd : wake_) {
                fcntl(fd, F_SETFD, FD_CLOEXEC);
                fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
            }
        }
    }
    void run() {
        for (;;) {
            std::vector<pollfd> fds;
            bool polled_fallback = false;
            {
                std::lock_guard<std::mutex> lock(mu_);
                for (size_t i = 0; i < list_.size();) {
                    Orphan& o = list_[i];
                    if (check_exit(o.c, *o.st, true)) {
                        release_exited(o.c);
                        list_.erase(list_.begin() + long(i));
                        continue;
                    }
                    int fd = exit_fd(o.c);
                    if (fd >= 0) fds.push_back({fd, POLLIN, 0});
                    else polled_fallback = true;
                    ++i;
                }
                if (list_.empty()) {
                    running_ = false;
                    return;
                }
            }
            fds.push_back({wake_[0], POLLIN, 0});
            poll(fds.data(), nfds_t(fds.size()), polled_fallback ? 100 : 5000);
            char buf[64];
            while (::read(wake_[0], buf, sizeof buf) > 0) {
            }
        }
    }

    std::mutex mu_;
    std::vector<Orphan> list_;
    bool running_{false};
    int wake_[2]{-1, -1};
};

} // namespace

int decode_wait_status(int status) {
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

pid_t spawn_child(ChildProc& c, ChildMain fn, void* arg) {
    c = ChildProc{};
#if defined(BROPTY_SUPERVISOR)
    if (spawn_supervised(c, fn, arg)) return c.pid;
#endif
#if defined(__APPLE__)
    // The child waits on `gate` until the exit watch is armed, so it cannot
    // exit (and be reaped by someone else) before the kqueue knows of it.
    int gate[2] = {-1, -1};
    if (::pipe(gate) == 0) {
        for (int fd : gate) fcntl(fd, F_SETFD, FD_CLOEXEC);
#if defined(F_SETNOSIGPIPE)
        fcntl(gate[1], F_SETNOSIGPIPE, 1);
#endif
    }
    pid_t p = fork();
    if (p == 0) {
        if (gate[0] >= 0) {
            ::close(gate[1]);  // a parent that died must not leave this read blocked forever
            char b;
            while (::read(gate[0], &b, 1) < 0 && errno == EINTR) {
            }
        }
        fn(arg);
    }
    int e = errno;
    if (p > 0) {
        c.pid = p;
        c.kq = kqueue();
        if (c.kq >= 0) {
            fcntl(c.kq, F_SETFD, FD_CLOEXEC);
            struct kevent ev;
            EV_SET(&ev, uintptr_t(p), EVFILT_PROC, EV_ADD, NOTE_EXIT | NOTE_EXITSTATUS, 0, nullptr);
            if (kevent(c.kq, &ev, 1, nullptr, 0, nullptr) < 0) close_fd(c.kq);
        }
        if (gate[1] >= 0) {
            char b = 1;
            ssize_t r = ::write(gate[1], &b, 1);
            (void)r;
        }
    }
    close_fd(gate[0]);
    close_fd(gate[1]);
    errno = e;
    return p;
#else
    pid_t p = fork();
    if (p == 0) fn(arg);
    if (p > 0) c.pid = p;
    return p;
#endif
}

bool wait_exit(ChildProc& c, int wake_fd, ExitState& st) {
    const int fd = exit_fd(c);
    for (;;) {
        pollfd fds[2] = {{wake_fd, POLLIN, 0}, {fd, POLLIN, 0}};
        int r = poll(fds, fd >= 0 ? 2 : 1, fd >= 0 ? -1 : 50);
        if (r < 0 && errno != EINTR) return false;
        if (fds[0].revents) return false;
        // The kqueue path keeps the zombie until teardown releases it (the
        // supervisor does the same on its side); plain waitpid reaps.
        if (check_exit(c, st, c.kq < 0)) return true;
    }
}

void reap_blocking(ChildProc& c) {
    if (c.status_fd >= 0) {
        int32_t code;
        (void)read_i32(c.status_fd, code);
        release_exited(c);
        return;
    }
    int status;
    while (waitpid(c.pid, &status, 0) < 0 && errno == EINTR) {
    }
    close_fd(c.kq);
}

void release_exited(ChildProc& c) {
#if defined(BROPTY_SUPERVISOR)
    if (c.super > 0) {
        close_fd(c.release_fd);  // the supervisor reaps the pty child and exits
        siginfo_t si{};
        while (waitid(P_PID, id_t(c.super), &si, WEXITED | __WCLONE) < 0 && errno == EINTR) {
        }
        // Gone (or reaped by a host __WALL reaper, which also means gone):
        // nothing runs on its stack any more.
        munmap(c.stack, c.stack_size);
        c.stack = nullptr;
        c.super = -1;
    }
#endif
    close_fd(c.status_fd);
    close_fd(c.release_fd);
#if defined(__APPLE__)
    if (c.kq >= 0) {
        int status;
        while (waitpid(c.pid, &status, 0) < 0 && errno == EINTR) {
        }
    }
#endif
    close_fd(c.kq);
}

void hand_to_reaper(ChildProc c, std::shared_ptr<ExitState> st) { Reaper::get().add(Orphan{c, std::move(st)}); }

size_t orphans_pending() { return Reaper::get().pending(); }

} // namespace bropty::pty_detail

#endif // !_WIN32
