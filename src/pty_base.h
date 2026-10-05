#pragma once
// What the ConPTY and POSIX implementations share: the bounded output ring
// (backpressure), the input queue drained by the writer thread, the exit
// state shared with the waiter thread, and the wakeup hook.

#include "bropty/pty.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>

namespace bropty::pty_detail {

// Exit state, shared (by shared_ptr) with the thread that waits for the
// child so that, in the worst case, that thread can outlive the PtyProcess.
struct ExitState {
    std::mutex mu;
    std::condition_variable cv;
    bool exited{false};
    int code{0};

    void set(int c) {
        {
            std::lock_guard<std::mutex> lock(mu);
            exited = true;
            code = c;
        }
        cv.notify_all();
    }
    bool wait_for(std::chrono::milliseconds t) {
        std::unique_lock<std::mutex> lock(mu);
        return cv.wait_for(lock, t, [&] { return exited; });
    }
    void wait() {
        std::unique_lock<std::mutex> lock(mu);
        cv.wait(lock, [&] { return exited; });
    }
    bool done() {
        std::lock_guard<std::mutex> lock(mu);
        return exited;
    }
};

class PtyBase : public IPtyProcess {
public:
    PtyBase();

    const std::string& last_error() const override { return error_; }
    size_t write(std::string_view data) override;
    size_t write_some(std::string_view data) override;
    size_t pending_input() const override;
    size_t input_space() const override;
    size_t read(void* dst, size_t max_bytes) override { return ring_->read(dst, max_bytes); }
    size_t read_timeout(void* dst, size_t max_bytes, std::chrono::milliseconds timeout) override {
        return ring_->read_timeout(dst, max_bytes, timeout);
    }
    size_t read_nonblocking(void* dst, size_t max_bytes) override { return ring_->read_nonblocking(dst, max_bytes); }
    size_t available() const override { return ring_->available_read(); }
    bool eof() const override { return output_done_.load() && ring_->empty(); }

    bool is_running() const override { return spawned_ && !exit_->done(); }
    std::optional<int> exit_code() const override;
    void wait() override;
    bool wait_for(std::chrono::milliseconds timeout) override;

    void set_wakeup(std::function<void()> fn) override { wakeup_ = std::move(fn); }
    ByteRingBuffer& ring_buffer() override { return *ring_; }

protected:
    // spawn() helpers.
    bool begin_spawn(const PtyConfig& config);  // false if already spawned
    bool fail(std::string message);

    // Reader thread: blocks while the ring is full (that is the backpressure);
    // returns immediately, discarding, once teardown closed the ring.
    void deliver_output(const void* data, size_t n);
    void output_finished();
    void notify() {
        if (wakeup_) wakeup_();
    }

    // Writer thread: wait for queued input (appended to `out`, at most
    // `max_bytes`); false once input is stopped.
    bool take_input(std::string& out, size_t max_bytes);
    void stop_input();

    void set_exited(int code) {
        exit_->set(code);
        notify();
    }

    PtyConfig config_;
    std::string error_;
    std::unique_ptr<ByteRingBuffer> ring_;
    std::shared_ptr<ExitState> exit_ = std::make_shared<ExitState>();
    std::atomic<bool> output_done_{false};
    bool spawned_{false};
    std::function<void()> wakeup_;

private:
    size_t enqueue(std::string_view data, bool partial);

    mutable std::mutex in_mu_;
    std::condition_variable in_cv_;
    std::string in_buf_;
    size_t in_head_{0};  // consumed prefix of in_buf_ (compacted lazily)
    bool in_stopped_{false};
    bool in_refused_{false};  // a write was cut short: wake the host when room returns
};

// Test seam (pty_detail::set_test_suppress_kill).
bool test_suppress_kill();

} // namespace bropty::pty_detail
