#include "bropty/ring_buffer.h"
#include "check.h"

#include <algorithm>
#include <cstring>
#include <thread>
#include <vector>

int main() {
    init_test();

    bropty::ByteRingBuffer rb(16);
    CHECK_EQ(rb.capacity(), size_t(16));
    CHECK_EQ(rb.size(), size_t(0));
    CHECK(rb.empty());

    const char* hello = "Hello World!";
    CHECK_EQ(rb.write(hello, std::strlen(hello)), std::strlen(hello));
    CHECK_EQ(rb.size(), std::strlen(hello));
    CHECK(!rb.empty());

    char read_buf[32] = {0};
    CHECK_EQ(rb.read_nonblocking(read_buf, sizeof(read_buf)), std::strlen(hello));
    CHECK_EQ(std::string(read_buf), std::string(hello));
    CHECK(rb.empty());

    // Wrap around.
    rb.write("0123456789", 10);
    rb.read(read_buf, 10);
    CHECK(rb.empty());
    rb.write("ABCDEFGHIJKL", 12);
    CHECK_EQ(rb.size(), size_t(12));
    std::memset(read_buf, 0, sizeof(read_buf));
    rb.read(read_buf, 12);
    CHECK_EQ(std::string(read_buf), std::string("ABCDEFGHIJKL"));

    // Producer / consumer.
    bropty::ByteRingBuffer conc(1024);
    constexpr size_t kTotal = 100000;
    std::thread producer([&]() {
        std::vector<uint8_t> data(256);
        for (size_t i = 0; i < 256; ++i) data[i] = uint8_t(i);
        size_t sent = 0;
        while (sent < kTotal) {
            size_t chunk = std::min<size_t>(data.size(), kTotal - sent);
            conc.write(data.data(), chunk);
            sent += chunk;
        }
        conc.close();
    });
    size_t received = 0;
    size_t mismatches = 0;
    uint8_t expected = 0;
    uint8_t buf[128];
    for (;;) {
        size_t n = conc.read(buf, sizeof buf);
        if (n == 0) break;
        for (size_t i = 0; i < n; ++i) {
            if (buf[i] != expected) ++mismatches;
            expected = uint8_t(expected + 1);
        }
        received += n;
    }
    producer.join();
    CHECK_EQ(received, kTotal);
    CHECK_EQ(mismatches, size_t(0));

    return check::finish("test_ring_buffer");
}
