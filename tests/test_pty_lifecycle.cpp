// Process lifetime and input-queue behaviour of the pty layer with real
// children:
//  * input backpressure the host can see: the bounded queue refuses, the
//    wakeup hook fires when it drains, a Session paste larger than the queue
//    completes in the background while input events are refused;
//  * the pty's pixel size follows Session::set_cell_pixel_size / resize;
//  * POSIX: the exit status survives a host SIGCHLD handler that reaps every
//    child, and SIGCHLD = SIG_IGN; a child that cannot be killed does not
//    keep terminate() waiting or leave a thread behind -- the process-wide
//    reaper collects it later;
//  * Windows: the job object takes grandchildren that detached from the
//    console, honours CREATE_BREAKAWAY_FROM_JOB, and ProcessTree::Console
//    restores the console-only policy.
//   test_pty_lifecycle <path-to-pty_child>
#include "pty_harness.h"

#include <condition_variable>
#include <mutex>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace bropty;
using namespace ph;

namespace {

bool process_gone(int64_t pid) {
#if defined(_WIN32)
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, DWORD(pid));
    if (!h) return true;
    bool gone = WaitForSingleObject(h, 0) == WAIT_OBJECT_0;
    CloseHandle(h);
    return gone;
#else
    return ::kill(pid_t(pid), 0) != 0 && errno == ESRCH;
#endif
}

bool wait_gone(int64_t pid, std::chrono::milliseconds limit) {
    auto end = Clock::now() + limit;
    while (Clock::now() < end) {
        if (process_gone(pid)) return true;
        std::this_thread::sleep_for(20ms);
    }
    return process_gone(pid);
}

void kill_pid(int64_t pid) {
#if defined(_WIN32)
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, DWORD(pid));
    if (h) {
        TerminateProcess(h, 1);
        CloseHandle(h);
    }
#else
    ::kill(pid_t(pid), SIGKILL);
#endif
}

// The pid printed after "GRANDCHILD ", or 0.
int64_t grandchild_pid(const std::string& screen) {
    size_t at = screen.find("GRANDCHILD ");
    if (at == std::string::npos) return 0;
    return std::atoll(screen.c_str() + at + 11);
}

#if !defined(_WIN32)
void reap_everything(int) {
    int saved = errno;
    while (waitpid(-1, nullptr, WNOHANG) > 0) {
    }
    errno = saved;
}
#endif

} // namespace

int main(int argc, char** argv) {
    init_test();
    if (argc < 2) {
        std::printf("usage: test_pty_lifecycle <pty_child>\n");
        return 2;
    }
    g_child = argv[1];
    start_watchdog("test_pty_lifecycle");

    {
        arm("bounded input: write_some + the drain wakeup move 4 MiB through a 256 KiB queue", 120);
        const size_t total = 4u << 20;
        PtyConfig c = child({"count", std::to_string(total)});
        c.input_buffer_bytes = 256u << 10;
        std::mutex mu;
        std::condition_variable cv;
        int wakeups = 0;
        auto pty = std::shared_ptr<IPtyProcess>(create_pty());
        pty->set_wakeup([&] {
            std::lock_guard<std::mutex> lock(mu);
            ++wakeups;
            cv.notify_all();
        });
        Session s(Run::opts(80, 24));
        CHECK(pty->spawn(c));
        s.attach_pty(pty);
        std::string out;
        auto pump = [&] {
            s.update();
            out = text(s.terminal());
        };
        auto deadline = Clock::now() + 60s;
        while (out.find("READY") == std::string::npos && Clock::now() < deadline) {
            pump();
            std::this_thread::sleep_for(5ms);
        }
        std::string payload(total, 'q');
        size_t sent = 0, short_writes = 0;
        while (sent < total && Clock::now() < deadline) {
            size_t n = pty->write_some(std::string_view(payload).substr(sent));
            CHECK(pty->pending_input() <= c.input_buffer_bytes);
            sent += n;
            if (sent < total) {
                ++short_writes;
                // Sleep until the drain wakeup (or output) arrives; no polling loop on the queue.
                std::unique_lock<std::mutex> lock(mu);
                int before = wakeups;
                cv.wait_for(lock, 2s, [&] { return wakeups != before; });
            }
            pump();
        }
        while (out.find("COUNT") == std::string::npos && Clock::now() < deadline) {
            pump();
            std::this_thread::sleep_for(5ms);
        }
        std::printf("  info: %zu short writes, %d wakeups\n", short_writes, wakeups);
        CHECK_EQ(sent, total);
        CHECK(short_writes > 0);
        CHECK(out.find("COUNT " + std::to_string(total)) != std::string::npos);
    }
    {
        arm("Session: a paste larger than the queue completes; input is refused meanwhile", 120);
        const size_t total = 2u << 20;
        PtyConfig c = child({"count", std::to_string(total + 1)});
        c.input_buffer_bytes = 128u << 10;
        Run r(c);
        CHECK(r.ok);
        CHECK(r.wait_for_text("READY"));
        CHECK(r.session.paste(std::string(total, 'p')));
        CHECK(r.session.input_blocked());
        CHECK(r.session.pending_input_bytes() > 0);
        CHECK(!r.session.send_key(KeyEvent::character('x')));  // would land inside the paste
        CHECK(!r.session.paste("more"));
        auto deadline = Clock::now() + 60s;
        while (r.session.input_blocked() && Clock::now() < deadline) {
            r.session.update();
            std::this_thread::sleep_for(2ms);
        }
        CHECK(!r.session.input_blocked());
        // The paste has left the Session but may still fill the pty's queue:
        // a key is accepted as soon as there is room for it.
        bool sent = false;
        while (!(sent = r.session.send_key(KeyEvent::character('x'))) && Clock::now() < deadline) {
            r.session.update();
            std::this_thread::sleep_for(2ms);
        }
        CHECK(sent);
        CHECK(r.wait_for_text("COUNT " + std::to_string(total + 1), 30000ms));
    }
#if !defined(_WIN32)
    {
        arm("the pty pixel size follows the cell size", 30);
        Run r(child({"pixels"}), 80, 24);
        CHECK(r.ok);
        CHECK(r.wait_for_text("PIXELS 0x0"));
        r.session.set_cell_pixel_size(9, 18);
        r.session.resize(100, 30);
        r.pty->write("x");
        CHECK(r.wait_for_text("PIXELS 900x540"));
        r.session.set_cell_pixel_size(10, 20);
        r.pty->write("x");
        CHECK(r.wait_for_text("PIXELS 1000x600"));
        r.pty->write("q");
        CHECK(r.drain());
    }
    {
        arm("a host SIGCHLD handler reaping every child does not take the exit status", 60);
        struct sigaction sa {}, old {};
        sa.sa_handler = reap_everything;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART;
        sigaction(SIGCHLD, &sa, &old);
        // The handler really reaps: an ordinary child of ours vanishes.
        pid_t plain = fork();
        if (plain == 0) _exit(3);
        CHECK(wait_gone(plain, 5000ms));
        for (int code : {42, 0, 7}) {
            Run r(child({"exit", std::to_string(code)}));
            CHECK(r.ok);
            CHECK(r.pty->wait_for(10s));
            CHECK(r.pty->exit_code() == std::optional<int>(code));
            CHECK(r.drain());
        }
        {
            Run r(child({"stubborn"}));
            CHECK(r.ok);
            CHECK(r.wait_for_text("READY"));
            ::kill(pid_t(r.pty->pid()), SIGKILL);
            CHECK(r.pty->wait_for(10s));
            CHECK(r.pty->exit_code() == std::optional<int>(128 + SIGKILL));
        }
        sigaction(SIGCHLD, &old, nullptr);
    }
    {
        arm("SIGCHLD = SIG_IGN (auto-reaping) does not lose the exit status", 60);
        struct sigaction sa {}, old {};
        sa.sa_handler = SIG_IGN;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGCHLD, &sa, &old);
        Run r(child({"exit", "9"}));
        CHECK(r.ok);
        CHECK(r.pty->wait_for(10s));
        CHECK(r.pty->exit_code() == std::optional<int>(9));
        sigaction(SIGCHLD, &old, nullptr);
    }
    {
        arm("a child that cannot be killed: terminate() is bounded, the reaper collects it", 60);
        PtyConfig c = child({"stubborn"});
        c.terminate_grace = std::chrono::milliseconds(200);
        auto pty = std::shared_ptr<IPtyProcess>(create_pty());
        CHECK(pty->spawn(c));
        Session s(Run::opts(80, 24));
        s.attach_pty(pty);
        auto deadline = Clock::now() + 10s;
        while (text(s.terminal()).find("READY") == std::string::npos && Clock::now() < deadline) {
            s.update();
            std::this_thread::sleep_for(5ms);
        }
        const int64_t pid = pty->pid();
        const size_t orphans_before = pty_detail::orphans_pending();
        pty_detail::set_test_suppress_kill(true);  // as if SIGKILL could not take effect
        auto t = Clock::now();
        pty->terminate();
        double ms = std::chrono::duration<double, std::milli>(Clock::now() - t).count();
        pty_detail::set_test_suppress_kill(false);
        std::printf("  info: terminate() with an unkillable child returned in %.0f ms\n", ms);
        CHECK(ms < 200 * 2 + 2000 + 1500);
        CHECK(pty->is_running());
        CHECK_EQ(pty_detail::orphans_pending(), orphans_before + 1);
        CHECK(!process_gone(pid));
        // The child finally dies; the reaper collects it and records the status.
        ::kill(pid_t(pid), SIGKILL);
        deadline = Clock::now() + 10s;
        while (pty_detail::orphans_pending() > orphans_before && Clock::now() < deadline)
            std::this_thread::sleep_for(10ms);
        CHECK_EQ(pty_detail::orphans_pending(), orphans_before);
        CHECK(pty->exit_code() == std::optional<int>(128 + SIGKILL));
        CHECK(wait_gone(pid, 5000ms));  // reaped: not even a zombie is left
        s.attach_pty(nullptr);
    }
    {
        arm("POSIX: a grandchild that left the session (setsid) is not chased", 30);
        Run r(child({"grandchild", "detached"}));
        CHECK(r.ok);
        CHECK(r.wait_for_text("GRANDCHILD "));
        int64_t g = grandchild_pid(r.screen());
        CHECK(g > 0);
        auto p = r.release();
        p.reset();
        std::this_thread::sleep_for(300ms);
        CHECK(!process_gone(g));
        kill_pid(g);
        CHECK(wait_gone(g, 5000ms));
    }
#else
    {
        arm("Windows: a grandchild that detached from the console dies with the session", 30);
        Run r(child({"grandchild", "detached"}));
        CHECK(r.ok);
        CHECK(r.wait_for_text("GRANDCHILD "));
        int64_t g = grandchild_pid(r.screen());
        CHECK(g > 0);
        CHECK(!process_gone(g));
        auto p = r.release();
        p.reset();
        CHECK(wait_gone(g, 5000ms));
        if (!process_gone(g)) kill_pid(g);
    }
    {
        arm("Windows: CREATE_BREAKAWAY_FROM_JOB is honoured", 30);
        Run r(child({"grandchild", "breakaway"}));
        CHECK(r.ok);
        CHECK(r.wait_for_text("GRANDCHILD"));
        CHECK(!r.has("GRANDCHILD-FAILED"));
        int64_t g = grandchild_pid(r.screen());
        CHECK(g > 0);
        auto p = r.release();
        p.reset();
        std::this_thread::sleep_for(300ms);
        CHECK(!process_gone(g));
        kill_pid(g);
        CHECK(wait_gone(g, 5000ms));
    }
    {
        arm("Windows: ProcessTree::Console leaves a detached grandchild running", 30);
        PtyConfig c = child({"grandchild", "detached"});
        c.process_tree = PtyConfig::ProcessTree::Console;
        Run r(c);
        CHECK(r.ok);
        CHECK(r.wait_for_text("GRANDCHILD "));
        int64_t g = grandchild_pid(r.screen());
        CHECK(g > 0);
        auto p = r.release();
        p.reset();
        std::this_thread::sleep_for(300ms);
        CHECK(!process_gone(g));
        kill_pid(g);
        CHECK(wait_gone(g, 5000ms));
    }
#endif

    g_deadline_ms = 0;
    return check::finish("test_pty_lifecycle");
}
