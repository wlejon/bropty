#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <span>
#include <chrono>

namespace bropty {

class ByteRingBuffer {
public:
    explicit ByteRingBuffer(size_t capacity = 1024 * 1024);
    ~ByteRingBuffer() = default;

    ByteRingBuffer(const ByteRingBuffer&) = delete;
    ByteRingBuffer& operator=(const ByteRingBuffer&) = delete;
    ByteRingBuffer(ByteRingBuffer&&) = delete;
    ByteRingBuffer& operator=(ByteRingBuffer&&) = delete;

    // Writes data into the ring buffer. If buffer is full, blocks until space is available
    // or until the buffer is closed. Returns number of bytes written.
    size_t write(std::span<const uint8_t> data);
    size_t write(const void* data, size_t size);

    // Non-blocking write. Writes as much as will fit immediately.
    size_t write_nonblocking(const void* data, size_t size);

    // Reads up to max_bytes into dst. If buffer is empty, blocks until data arrives
    // or the buffer is closed. Returns number of bytes read (0 if closed & empty).
    size_t read(void* dst, size_t max_bytes);

    // Reads up to max_bytes with timeout. Returns 0 if timed out or closed & empty.
    size_t read_timeout(void* dst, size_t max_bytes, std::chrono::milliseconds timeout);

    // Non-blocking read. Returns available bytes immediately without waiting.
    size_t read_nonblocking(void* dst, size_t max_bytes);

    // Closes the buffer (signals EOF). Unblocks any waiting readers and writers.
    void close();

    // Reopens the buffer after closing.
    void reopen();

    // Clears all contents.
    void clear();

    [[nodiscard]] bool is_closed() const;
    [[nodiscard]] size_t size() const;
    [[nodiscard]] size_t capacity() const;
    [[nodiscard]] size_t available_read() const;
    [[nodiscard]] size_t available_write() const;
    [[nodiscard]] bool empty() const;

private:
    std::vector<uint8_t> buffer_;
    size_t capacity_{0};
    size_t head_{0}; // Write index
    size_t tail_{0}; // Read index
    size_t count_{0};
    bool closed_{false};

    mutable std::mutex mutex_;
    std::condition_variable cv_read_;
    std::condition_variable cv_write_;
};

} // namespace bropty
