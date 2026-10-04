#pragma once

#include "bropty/ring_buffer.h"
#include <chrono>
#include <cstdint>
#include <memory>
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
    std::string command;
    std::vector<std::string> args;
    std::string cwd;
    std::vector<std::pair<std::string, std::string>> env;
    PtySize size{80, 24, 0, 0};
};

class IPtyProcess {
public:
    virtual ~IPtyProcess() = default;

    virtual bool spawn(const PtyConfig& config) = 0;
    virtual size_t write(std::string_view data) = 0;
    virtual size_t read(void* dst, size_t max_bytes) = 0;
    virtual size_t read_timeout(void* dst, size_t max_bytes, std::chrono::milliseconds timeout) = 0;
    virtual size_t read_nonblocking(void* dst, size_t max_bytes) = 0;
    virtual bool resize(int cols, int rows) = 0;
    [[nodiscard]] virtual bool is_running() const = 0;
    [[nodiscard]] virtual int exit_code() const = 0;
    virtual void terminate() = 0;
    virtual void wait() = 0;

    [[nodiscard]] virtual ByteRingBuffer& ring_buffer() = 0;
};

// Creates a platform-appropriate PTY instance (ConPTY on Windows, POSIX PTY on Linux/macOS)
[[nodiscard]] std::unique_ptr<IPtyProcess> create_pty();

} // namespace bropty
