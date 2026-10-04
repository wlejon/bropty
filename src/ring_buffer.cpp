#include "bropty/ring_buffer.h"
#include <algorithm>
#include <cstring>

namespace bropty {

ByteRingBuffer::ByteRingBuffer(size_t capacity)
    : buffer_(std::max<size_t>(capacity, 1)),
      capacity_(buffer_.size()) {}

size_t ByteRingBuffer::write(std::span<const uint8_t> data) {
    return write(data.data(), data.size());
}

size_t ByteRingBuffer::write(const void* data, size_t size) {
    if (!data || size == 0) return 0;
    const auto* src = static_cast<const uint8_t*>(data);
    size_t total_written = 0;

    std::unique_lock<std::mutex> lock(mutex_);
    while (total_written < size && !closed_) {
        cv_write_.wait(lock, [this]() {
            return closed_ || count_ < capacity_;
        });

        if (closed_) break;

        size_t can_write = std::min(size - total_written, capacity_ - count_);
        // Write in up to two chunks (wrap around)
        size_t first_chunk = std::min(can_write, capacity_ - head_);
        std::memcpy(buffer_.data() + head_, src + total_written, first_chunk);
        head_ = (head_ + first_chunk) % capacity_;

        size_t second_chunk = can_write - first_chunk;
        if (second_chunk > 0) {
            std::memcpy(buffer_.data() + head_, src + total_written + first_chunk, second_chunk);
            head_ = (head_ + second_chunk) % capacity_;
        }

        count_ += can_write;
        total_written += can_write;

        lock.unlock();
        cv_read_.notify_one();
        lock.lock();
    }

    return total_written;
}

size_t ByteRingBuffer::write_nonblocking(const void* data, size_t size) {
    if (!data || size == 0) return 0;
    const auto* src = static_cast<const uint8_t*>(data);

    std::unique_lock<std::mutex> lock(mutex_);
    if (closed_ || count_ == capacity_) return 0;

    size_t can_write = std::min(size, capacity_ - count_);
    size_t first_chunk = std::min(can_write, capacity_ - head_);
    std::memcpy(buffer_.data() + head_, src, first_chunk);
    head_ = (head_ + first_chunk) % capacity_;

    size_t second_chunk = can_write - first_chunk;
    if (second_chunk > 0) {
        std::memcpy(buffer_.data() + head_, src + first_chunk, second_chunk);
        head_ = (head_ + second_chunk) % capacity_;
    }

    count_ += can_write;
    lock.unlock();
    cv_read_.notify_one();

    return can_write;
}

size_t ByteRingBuffer::read(void* dst, size_t max_bytes) {
    if (!dst || max_bytes == 0) return 0;
    auto* out = static_cast<uint8_t*>(dst);

    std::unique_lock<std::mutex> lock(mutex_);
    cv_read_.wait(lock, [this]() {
        return closed_ || count_ > 0;
    });

    if (count_ == 0) {
        return 0; // Closed and empty
    }

    size_t can_read = std::min(max_bytes, count_);
    size_t first_chunk = std::min(can_read, capacity_ - tail_);
    std::memcpy(out, buffer_.data() + tail_, first_chunk);
    tail_ = (tail_ + first_chunk) % capacity_;

    size_t second_chunk = can_read - first_chunk;
    if (second_chunk > 0) {
        std::memcpy(out + first_chunk, buffer_.data() + tail_, second_chunk);
        tail_ = (tail_ + second_chunk) % capacity_;
    }

    count_ -= can_read;
    lock.unlock();
    cv_write_.notify_one();

    return can_read;
}

size_t ByteRingBuffer::read_timeout(void* dst, size_t max_bytes, std::chrono::milliseconds timeout) {
    if (!dst || max_bytes == 0) return 0;
    auto* out = static_cast<uint8_t*>(dst);

    std::unique_lock<std::mutex> lock(mutex_);
    bool signaled = cv_read_.wait_for(lock, timeout, [this]() {
        return closed_ || count_ > 0;
    });

    if (!signaled || count_ == 0) {
        return 0;
    }

    size_t can_read = std::min(max_bytes, count_);
    size_t first_chunk = std::min(can_read, capacity_ - tail_);
    std::memcpy(out, buffer_.data() + tail_, first_chunk);
    tail_ = (tail_ + first_chunk) % capacity_;

    size_t second_chunk = can_read - first_chunk;
    if (second_chunk > 0) {
        std::memcpy(out + first_chunk, buffer_.data() + tail_, second_chunk);
        tail_ = (tail_ + second_chunk) % capacity_;
    }

    count_ -= can_read;
    lock.unlock();
    cv_write_.notify_one();

    return can_read;
}

size_t ByteRingBuffer::read_nonblocking(void* dst, size_t max_bytes) {
    if (!dst || max_bytes == 0) return 0;
    auto* out = static_cast<uint8_t*>(dst);

    std::unique_lock<std::mutex> lock(mutex_);
    if (count_ == 0) return 0;

    size_t can_read = std::min(max_bytes, count_);
    size_t first_chunk = std::min(can_read, capacity_ - tail_);
    std::memcpy(out, buffer_.data() + tail_, first_chunk);
    tail_ = (tail_ + first_chunk) % capacity_;

    size_t second_chunk = can_read - first_chunk;
    if (second_chunk > 0) {
        std::memcpy(out + first_chunk, buffer_.data() + tail_, second_chunk);
        tail_ = (tail_ + second_chunk) % capacity_;
    }

    count_ -= can_read;
    lock.unlock();
    cv_write_.notify_one();

    return can_read;
}

void ByteRingBuffer::close() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
    }
    cv_read_.notify_all();
    cv_write_.notify_all();
}

void ByteRingBuffer::reopen() {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = false;
}

void ByteRingBuffer::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    head_ = 0;
    tail_ = 0;
    count_ = 0;
    cv_write_.notify_all();
}

bool ByteRingBuffer::is_closed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
}

size_t ByteRingBuffer::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return count_;
}

size_t ByteRingBuffer::capacity() const {
    return capacity_;
}

size_t ByteRingBuffer::available_read() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return count_;
}

size_t ByteRingBuffer::available_write() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_ ? 0 : (capacity_ - count_);
}

bool ByteRingBuffer::empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return count_ == 0;
}

} // namespace bropty
