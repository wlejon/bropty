// Spawns a short-lived program through the platform PTY and reads its output.
#include "bropty/pty.h"
#include "check.h"

#include <chrono>
#include <string>

int main() {
    init_test();
    auto pty = bropty::create_pty();
    CHECK(pty != nullptr);
    if (!pty) return check::finish("test_pty");

    bropty::PtyConfig config;
#if defined(_WIN32)
    config.command = "cmd.exe";
    config.args = {"/c", "echo", "HelloFromPty"};
#else
    config.command = "/bin/sh";
    config.args = {"-c", "echo HelloFromPty"};
#endif
    config.size = {80, 24, 0, 0};

    bool ok = pty->spawn(config);
    CHECK(ok);
    if (!ok) return check::finish("test_pty");
    pty->resize(120, 40);

    std::string acc;
    char buffer[512];
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(10)) {
        size_t n = pty->read_timeout(buffer, sizeof buffer, std::chrono::milliseconds(200));
        if (n > 0) {
            acc.append(buffer, n);
            if (acc.find("HelloFromPty") != std::string::npos) break;
        }
        if (!pty->is_running() && n == 0) break;
    }
    CHECK(acc.find("HelloFromPty") != std::string::npos);
    pty->wait();
    CHECK_EQ(pty->exit_code(), 0);
    return check::finish("test_pty");
}
