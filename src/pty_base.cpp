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

namespace {
std::atomic<bool> g_suppress_kill{false};
}

void set_test_suppress_kill(bool on) { g_suppress_kill = on; }
bool test_suppress_kill() { return g_suppress_kill.load(); }

size_t PtyBase::enqueue(std::string_view data, bool partial) {
    if (data.empty()) return 0;
    size_t n;
    {
        std::lock_guard<std::mutex> lock(in_mu_);
        if (in_stopped_ || !spawned_) return 0;
        const size_t cap = std::max<size_t>(1, config_.input_buffer_bytes);
        const size_t used = in_buf_.size() - in_head_;
        const size_t room = used < cap ? cap - used : 0;
        n = partial ? std::min(room, data.size()) : (data.size() <= room ? data.size() : 0);
        if (n < data.size()) in_refused_ = true;
        if (n == 0) return 0;
        if (in_head_ > 0 && in_head_ >= in_buf_.size() / 2) {
            in_buf_.erase(0, in_head_);
            in_head_ = 0;
        }
        in_buf_.append(data.substr(0, n));
    }
    in_cv_.notify_one();
    return n;
}

size_t PtyBase::write(std::string_view data) { return enqueue(data, false); }
size_t PtyBase::write_some(std::string_view data) { return enqueue(data, true); }

size_t PtyBase::pending_input() const {
    std::lock_guard<std::mutex> lock(in_mu_);
    return in_buf_.size() - in_head_;
}

size_t PtyBase::input_space() const {
    std::lock_guard<std::mutex> lock(in_mu_);
    if (in_stopped_ || !spawned_) return 0;
    const size_t cap = std::max<size_t>(1, config_.input_buffer_bytes);
    const size_t used = in_buf_.size() - in_head_;
    return used < cap ? cap - used : 0;
}

bool PtyBase::take_input(std::string& out, size_t max_bytes) {
    bool wake = false;
    {
        std::unique_lock<std::mutex> lock(in_mu_);
        in_cv_.wait(lock, [&] { return in_stopped_ || in_buf_.size() > in_head_; });
        if (in_stopped_) return false;
        size_t n = std::min(max_bytes, in_buf_.size() - in_head_);
        out.append(in_buf_, in_head_, n);
        in_head_ += n;
        if (in_head_ == in_buf_.size()) {
            in_buf_.clear();
            in_head_ = 0;
            // Return the memory of a large burst once it has been delivered.
            if (in_buf_.capacity() > (256u << 10)) in_buf_.shrink_to_fit();
        }
        const size_t cap = std::max<size_t>(1, config_.input_buffer_bytes);
        if (in_refused_ && in_buf_.size() - in_head_ <= cap / 2) {
            in_refused_ = false;
            wake = true;
        }
    }
    if (wake) notify();
    return true;
}

void PtyBase::stop_input() {
    bool wake;
    {
        std::lock_guard<std::mutex> lock(in_mu_);
        in_stopped_ = true;
        in_buf_.clear();
        in_head_ = 0;
        wake = in_refused_;
        in_refused_ = false;
    }
    in_cv_.notify_all();
    if (wake) notify();
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
