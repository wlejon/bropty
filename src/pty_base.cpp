#include "pty_base.h"

#include <algorithm>

namespace bropty::pty_detail {

PtyBase::PtyBase() : ring_(std::make_unique<ByteRingBuffer>(1u << 20)) {}

bool PtyBase::begin_spawn(const PtyConfig& config) {
    if (spawned_) {
        error_ = "spawn: this pty already ran a process";
        return false;
    }
    config_ = config;
    error_.clear();
    if (config.output_buffer_bytes != ring_->capacity())
        ring_ = std::make_unique<ByteRingBuffer>(std::max<size_t>(4096, config.output_buffer_bytes));
    return true;
}

bool PtyBase::fail(std::string message) {
    error_ = std::move(message);
    return false;
}

size_t PtyBase::write(std::string_view data) {
    if (data.empty()) return 0;
    {
        std::lock_guard<std::mutex> lock(in_mu_);
        if (in_stopped_ || !spawned_) return 0;
        in_buf_.append(data);
    }
    in_cv_.notify_one();
    return data.size();
}

size_t PtyBase::pending_input() const {
    std::lock_guard<std::mutex> lock(in_mu_);
    return in_buf_.size();
}

bool PtyBase::take_input(std::string& out, size_t max_bytes) {
    std::unique_lock<std::mutex> lock(in_mu_);
    in_cv_.wait(lock, [&] { return in_stopped_ || !in_buf_.empty(); });
    if (in_stopped_) return false;
    size_t n = std::min(max_bytes, in_buf_.size());
    out.append(in_buf_, 0, n);
    in_buf_.erase(0, n);
    return true;
}

void PtyBase::stop_input() {
    {
        std::lock_guard<std::mutex> lock(in_mu_);
        in_stopped_ = true;
        in_buf_.clear();
    }
    in_cv_.notify_all();
}

void PtyBase::deliver_output(const void* data, size_t n) {
    // A closed ring (teardown) accepts nothing: the bytes are dropped, which
    // is what keeps the child and conhost draining instead of blocking.
    if (ring_->write(data, n) > 0) notify();
}

void PtyBase::output_finished() {
    output_done_ = true;
    ring_->close();
    notify();
}

std::optional<int> PtyBase::exit_code() const {
    std::lock_guard<std::mutex> lock(exit_->mu);
    if (!exit_->exited) return std::nullopt;
    return exit_->code;
}

void PtyBase::wait() {
    if (spawned_) exit_->wait();
}

bool PtyBase::wait_for(std::chrono::milliseconds timeout) {
    return !spawned_ || exit_->wait_for(timeout);
}

} // namespace bropty::pty_detail
