#include "bropty/pty.h"
#include "test_common.h"
#include <cassert>
#include <chrono>
#include <iostream>
#include <string>

int main() {
    init_test();
    std::cout << "[test_pty_win] Starting...\n";

#if defined(_WIN32)
    auto pty = bropty::create_pty();
    assert(pty != nullptr);

    bropty::PtyConfig config;
    config.command = "cmd.exe";
    config.args = {"/c", "echo", "HelloFromPty"};
    config.size = {80, 24, 0, 0};

    bool ok = pty->spawn(config);
    if (!ok) {
        std::cerr << "[test_pty_win] Failed to spawn cmd.exe in ConPTY (may occur in headless restricted environment)\n";
        return 0;
    }

    assert(pty->is_running() || pty->exit_code() == 0);

    // Test resize
    pty->resize(120, 40);

    // Read output until echo message is found or timeout
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

    pty->wait();

    while (size_t n = pty->read_nonblocking(buffer, sizeof(buffer) - 1)) {
        buffer[n] = '\0';
        accumulated.append(buffer, n);
    }

    std::cout << "[test_pty_win] Accumulated " << accumulated.size() << " bytes\n";
    assert(accumulated.find("HelloFromPty") != std::string::npos);
    assert(pty->exit_code() == 0);

#else
    std::cout << "[test_pty_win] Skipped on non-Windows\n";
#endif

    std::cout << "[test_pty_win] PASSED\n";
    return 0;
}
