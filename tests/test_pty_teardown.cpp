// Throughput-side and teardown behaviour of the pty layer with real children:
// a flood producer is throttled by backpressure (not buffered without bound),
// Session::update() stays within its time budget while the flood runs, and
// destroying a pty returns promptly whatever the child is doing -- flooding
// into a full buffer, ignoring SIGHUP/SIGTERM (or the console close event),
// never reading its input, or sitting in an interactive shell.
//   test_pty_teardown <path-to-pty_child>
#include "pty_harness.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <csignal>
#include <cerrno>
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

double ms_since(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

// Destroy, timed; the pty must also have taken its child with it.
void destroy_within(std::shared_ptr<IPtyProcess> p, double limit_ms, const char* what) {
    CHECK(p.use_count() == 1);
    int64_t pid = p->pid();
    auto t = Clock::now();
    p.reset();
    double ms = ms_since(t);
    std::printf("  info: %s: destroyed in %.0f ms\n", what, ms);
    CHECK(ms < limit_ms);
    CHECK(process_gone(pid));
}

} // namespace

int main(int argc, char** argv) {
    init_test();
    if (argc < 2) {
        std::printf("usage: test_pty_teardown <pty_child>\n");
        return 2;
    }
    g_child = argv[1];
    start_watchdog("test_pty_teardown");

    {
        arm("flood: update() stays within budget, all output arrives", 120);
        const long long kBytes = 24ll << 20;
        Run r(child({"flood", std::to_string(kBytes)}), 160, 50);
        CHECK(r.ok);
        Session::UpdateBudget budget;
        budget.max_time = std::chrono::microseconds(4000);
        double worst = 0, busy = 0;
        size_t bytes = 0;
        int frames = 0;
        auto start = Clock::now();
        while (r.ok && !r.pty->eof() && Clock::now() - start < 90s) {
            auto t = Clock::now();
            bytes += r.session.update(budget);
            double d = ms_since(t);
            worst = std::max(worst, d);
            busy += d;
            ++frames;
            std::this_thread::sleep_for(8ms);  // the rest of a frame
        }
        double wall = ms_since(start);
        std::printf("  info: flood %zu bytes in %.0f ms wall over %d frames; update() worst %.2f ms, busy %.0f ms\n",
                    bytes, wall, frames, worst, busy);
        CHECK(r.pty->eof());
        CHECK(r.has("FLOOD-DONE"));
        // 4 ms budget plus at most one 16 KiB slice past the deadline.
        CHECK(worst < 25.0);
        CHECK(r.pty->exit_code() == std::optional<int>(0));
    }
    {
        arm("backpressure: an unread flood blocks the producer", 60);
        PtyConfig c = child({"flood", std::to_string(64ll << 20)});
        c.output_buffer_bytes = 64 << 10;
        Run r(c);
        CHECK(r.ok);
        std::this_thread::sleep_for(1500ms);  // nobody reads
        // Unthrottled, 64 MiB would be written in well under a second.
        CHECK(r.pty->is_running());
        CHECK(r.pty->available() <= size_t(64 << 10));
        std::printf("  info: buffered while unread: %zu bytes\n", r.pty->available());
        // Reading lets it make progress again.
        size_t got = 0;
        auto t = Clock::now();
        while (got < (4u << 20) && Clock::now() - t < 20s) got += r.session.update();
        CHECK(got >= (4u << 20));
        destroy_within(r.release(), 6000, "flooding child, partially read");
    }
    {
        arm("destroy with a full output buffer", 60);
        PtyConfig c = child({"flood", "0"});
        c.output_buffer_bytes = 64 << 10;
        auto p = std::shared_ptr<IPtyProcess>(create_pty());
        CHECK(p->spawn(c));
        auto t = Clock::now();
        while (p->ring_buffer().available_write() > 0 && Clock::now() - t < 10s) std::this_thread::sleep_for(10ms);
        CHECK_EQ(p->ring_buffer().available_write(), size_t(0));
        destroy_within(std::move(p), 6000, "endless flood, buffer full");
    }
    {
        arm("destroy a child that ignores hangup/terminate", 60);
        PtyConfig c = child({"stubborn"});
        c.terminate_grace = std::chrono::milliseconds(300);
        Run r(c);
        CHECK(r.ok);
        CHECK(r.wait_for_text("READY"));
        destroy_within(r.release(), 6000, "stubborn child");
    }
    {
        arm("terminate() then read what was left", 30);
        Run r(child({"args", "left", "behind"}));
        CHECK(r.ok);
        CHECK(r.pty->wait_for(10s));
        std::this_thread::sleep_for(300ms);
        r.pty->terminate();
        r.pty->terminate();  // idempotent
        CHECK(r.drain(3000ms));
        CHECK(r.has("[left]"));
        CHECK(!r.pty->is_running());
        CHECK_EQ(r.pty->write("late"), size_t(0));
    }
    {
        arm("write() never blocks on a child that does not read; the queue is bounded", 60);
        PtyConfig c = child({"stubborn"});
        c.input_buffer_bytes = 1u << 20;
        Run r(c);
        CHECK(r.ok);
        CHECK(r.wait_for_text("READY"));
        std::string block(64 << 10, 'z');
        auto t = Clock::now();
        size_t queued = 0, refused = 0;
        for (int i = 0; i < 256; ++i) {  // 16 MiB offered
            size_t n = r.pty->write(block);
            CHECK(n == 0 || n == block.size());  // all or nothing
            if (n) queued += n;
            else ++refused;
        }
        double ms = ms_since(t);
        std::printf("  info: offered 16 MiB in %.1f ms: %zu queued, %zu blocks refused, %zu pending\n", ms, queued,
                    refused, r.pty->pending_input());
        CHECK(ms < 1000);
        CHECK(refused > 0);
        CHECK(r.pty->pending_input() <= c.input_buffer_bytes);
        CHECK(r.pty->pending_input() + r.pty->input_space() == c.input_buffer_bytes);
        destroy_within(r.release(), 6000, "child with 16 MiB of unread input");
    }
    {
        arm("destroy an interactive shell", 60);
#if defined(_WIN32)
        PtyConfig c;
        c.command = "cmd.exe";
        c.args = {"/d", "/q", "/k"};
#else
        // An interactive shell ignores SIGTERM; hangup is what ends it.
        PtyConfig c;
        c.command = "/bin/sh";
        c.args = {"-i"};
#endif
        Run r(c);
        CHECK(r.ok);
        std::this_thread::sleep_for(500ms);
        r.session.update();
        destroy_within(r.release(), 6000, "interactive shell");
    }
    {
        arm("destroy while the child is still starting", 30);
        for (int i = 0; i < 5; ++i) {
            auto p = std::shared_ptr<IPtyProcess>(create_pty());
            CHECK(p->spawn(child({"stubborn"})));
            destroy_within(std::move(p), 6000, "just-spawned child");
        }
    }

    g_deadline_ms = 0;
    return check::finish("test_pty_teardown");
}
