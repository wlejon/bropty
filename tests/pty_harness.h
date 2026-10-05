#pragma once
// Fixtures for tests that run real programs through the platform pty. A
// watchdog turns any hang into a reported failure naming the phase, instead
// of a ctest timeout.

#include "bropty/pty.h"
#include "bropty/session.h"
#include "check.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace ph {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

inline std::string g_child;  // path of the pty_child helper (argv[1])
inline std::atomic<const char*> g_phase{"init"};
inline std::atomic<long long> g_deadline_ms{0};

inline long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

inline void arm(const char* phase, int seconds) {
    std::printf("-- %s\n", phase);
    std::fflush(stdout);
    g_phase = phase;
    g_deadline_ms = now_ms() + seconds * 1000LL;
}

inline void start_watchdog(const char* test_name) {
    std::thread([test_name] {
        for (;;) {
            std::this_thread::sleep_for(100ms);
            long long d = g_deadline_ms.load();
            if (d && now_ms() > d) {
                std::printf("FAIL HANG in phase '%s' (watchdog)\n", g_phase.load());
                check::g_failures++;
                check::finish(test_name);
                std::fflush(stdout);
                std::_Exit(1);
            }
        }
    }).detach();
}

inline bropty::PtyConfig child(std::vector<std::string> args) {
    bropty::PtyConfig c;
    c.command = g_child;
    c.args = std::move(args);
    return c;
}

inline bropty::PtyConfig shell(const std::string& script) {
    bropty::PtyConfig c;
#if defined(_WIN32)
    c.command = "cmd.exe";
    c.windows_command_line = "cmd.exe /d /c " + script;
#else
    c.command = "/bin/sh";
    c.args = {"-c", script};
#endif
    return c;
}

// History then screen; rows joined with '~' (soft wrap) or '|'.
inline std::string text(const bropty::Terminal& t) {
    std::string out;
    for (size_t i = 0; i < t.history_rows(); ++i) {
        out += t.history_text(i);
        out += t.history_row(i).wrapped() ? "~" : "|";
    }
    for (int r = 0; r < t.rows(); ++r) {
        out += t.row_text(r);
        out += t.row(r).wrapped() ? "~" : "|";
    }
    while (!out.empty() && out.back() == '|') out.pop_back();
    return out;
}

// A Session on a spawned pty.
struct Run {
    std::shared_ptr<bropty::IPtyProcess> pty;
    bropty::Session session;
    bool ok{false};

    explicit Run(const bropty::PtyConfig& cfg, int cols = 80, int rows = 24) : session(opts(cols, rows)) {
        pty = std::shared_ptr<bropty::IPtyProcess>(bropty::create_pty());
        bropty::PtyConfig c = cfg;
        c.size = {cols, rows, 0, 0};
        ok = pty->spawn(c);
        if (!ok) std::printf("  spawn failed: %s\n", pty->last_error().c_str());
        session.attach_pty(pty);
    }
    static bropty::TerminalOptions opts(int cols, int rows) {
        bropty::TerminalOptions o;
        o.cols = cols;
        o.rows = rows;
        return o;
    }
    // Detach the pty from the session and hand over the last reference.
    std::shared_ptr<bropty::IPtyProcess> release() {
        session.attach_pty(nullptr);
        return std::move(pty);
    }
    std::string screen() const { return text(session.terminal()); }
    bool has(const std::string& needle) const { return screen().find(needle) != std::string::npos; }

    // Pump output into the terminal until `needle` shows up.
    bool wait_for_text(const std::string& needle, std::chrono::milliseconds limit = 10000ms) {
        auto end = Clock::now() + limit;
        while (Clock::now() < end) {
            size_t n = session.update();
            if (has(needle)) return true;
            if (n == 0) {
                if (pty->eof()) return has(needle) || report_crash();
                std::this_thread::sleep_for(5ms);
            }
        }
        return has(needle);
    }
    // A child that crashed or never started (0xC0000142 ...) says so: with the
    // error mode init_test() sets it exits with the status instead of waiting
    // on a dialog. Always false (the wait failed).
    bool report_crash() const {
        const std::optional<int> code = pty->exit_code();
        if (code && is_crash_status(*code))
            std::printf("  child crashed or failed to start: exit status 0x%08X\n", unsigned(*code));
        return false;
    }
    // Pump until the child exits and its output has all arrived.
    bool drain(std::chrono::milliseconds limit = 10000ms) {
        auto end = Clock::now() + limit;
        while (Clock::now() < end) {
            size_t n = session.update();
            if (n == 0) {
                if (pty->eof()) return true;
                std::this_thread::sleep_for(5ms);
            }
        }
        return false;
    }
};

} // namespace ph
