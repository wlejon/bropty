#include "bropty/ring_buffer.h"
#include "test_common.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

int main() {
    init_test();
    std::cout << "[test_ring_buffer] Starting...\n";

    // Basic write and read
    bropty::ByteRingBuffer rb(16);
    assert(rb.capacity() == 16);
    assert(rb.size() == 0);
    assert(rb.empty());

    const char* hello = "Hello World!";
    size_t written = rb.write(hello, std::strlen(hello));
    assert(written == std::strlen(hello));
    assert(rb.size() == std::strlen(hello));
    assert(!rb.empty());

    char read_buf[32] = {0};
    size_t read_bytes = rb.read_nonblocking(read_buf, sizeof(read_buf));
    assert(read_bytes == std::strlen(hello));
    assert(std::strcmp(read_buf, hello) == 0);
    assert(rb.empty());

    // Wrap around
    // Write 10 bytes, read 10 bytes, write 12 bytes
    rb.write("0123456789", 10);
    rb.read(read_buf, 10);
    assert(rb.empty());

    rb.write("ABCDEFGHIJKL", 12);
    assert(rb.size() == 12);
    std::memset(read_buf, 0, sizeof(read_buf));
    rb.read(read_buf, 12);
    assert(std::strcmp(read_buf, "ABCDEFGHIJKL") == 0);

    // Concurrency test: Producer-consumer thread test
    bropty::ByteRingBuffer conc_rb(1024);
    constexpr size_t kTotalBytes = 100000;

    std::thread producer([&]() {
        std::vector<uint8_t> data(256);
        for (size_t i = 0; i < 256; ++i) data[i] = static_cast<uint8_t>(i);

        size_t sent = 0;
        while (sent < kTotalBytes) {
            size_t chunk = std::min<size_t>(data.size(), kTotalBytes - sent);
            conc_rb.write(data.data(), chunk);
            sent += chunk;
        }
        conc_rb.close();
    });

    std::thread consumer([&]() {
        uint8_t buf[128];
        size_t received = 0;
        uint8_t expected = 0;

        while (true) {
            size_t n = conc_rb.read(buf, sizeof(buf));
            if (n == 0) break;
            for (size_t i = 0; i < n; ++i) {
                assert(buf[i] == expected);
                expected = static_cast<uint8_t>(expected + 1);
            }
            received += n;
        }
        assert(received == kTotalBytes);
    });

    producer.join();
    consumer.join();

    std::cout << "[test_ring_buffer] PASSED\n";
    return 0;
}
