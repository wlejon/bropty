#include "bropty/pty.h"
#include "test_common.h"
#include <cassert>
#include <chrono>
#include <iostream>
#include <string>

int main() {
    init_test();
    std::cout << "[test_pty_posix] Starting...\n";

#if !defined(_WIN32)
    auto pty = bropty::create_pty();
    assert(pty != nullptr);

    bropty::PtyConfig config;
    config.command = "/bin/sh";
    config.args = {"-c", "echo HelloFromPty"};
    config.size = {80, 24, 0, 0};

    bool ok = pty->spawn(config);
    assert(ok);

    pty->resize(120, 40);

    std::string accumulated;
    char buffer[512];
    auto start = std::chrono::steady_clock::now();

    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(5)) {
        size_t n = pty->read_timeout(buffer, sizeof(buffer) - 1, std::chrono::milliseconds(200));
        if (n > 0) {
            buffer[n] = '\0';
            accumulated.append(buffer, n);
            if (accumulated.find("HelloFromPty") != std::string::npos) {
                break;
            }
        }
        if (!pty->is_running() && n == 0) {
            break;
        }
    }

    std::cout << "[test_pty_posix] Output: " << accumulated << "\n";
    assert(accumulated.find("HelloFromPty") != std::string::npos);

    pty->wait();
    assert(pty->exit_code() == 0);
#else
    std::cout << "[test_pty_posix] Skipped on Windows\n";
#endif

    std::cout << "[test_pty_posix] PASSED\n";
    return 0;
}
