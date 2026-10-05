#pragma once
// Session: a Terminal attached to a PTY process. It owns the plumbing that
// Terminal deliberately lacks: draining PTY output into the emulator, writing
// the emulator's replies back to the PTY, and encoding keyboard / mouse /
// paste input according to the terminal's current modes.
//
// Embedder events (bell, title, clipboard, ...) are forwarded to an optional
// delegate TerminalHost.

#include "bropty/input.h"
#include "bropty/pty.h"
#include "bropty/terminal.h"

#include <chrono>
#include <functional>
#include <memory>
#include <string_view>

namespace bropty {

class Session final : public TerminalHost {
public:
    using OutputCallback = std::function<void(std::string_view)>;

    Session();
    explicit Session(const TerminalOptions& options);
    ~Session() override;

    [[nodiscard]] Terminal& terminal() noexcept { return term_; }
    [[nodiscard]] const Terminal& terminal() const noexcept { return term_; }

    void attach_pty(std::shared_ptr<IPtyProcess> pty);
    [[nodiscard]] const std::shared_ptr<IPtyProcess>& pty() const noexcept { return pty_; }
    void set_delegate(TerminalHost* host) noexcept { delegate_ = host; }
    // Observe (and, without a PTY, capture) every byte sent to the application.
    void set_output_callback(OutputCallback cb) { output_cb_ = std::move(cb); }

    // Feed pending PTY output into the terminal, bounded so that a fast
    // producer cannot stall the host's frame: update() stops after
    // `max_bytes` or once `max_time` has elapsed (checked every `slice`
    // bytes), whichever comes first. Unconsumed output stays in the PTY's
    // bounded buffer, which in turn stops reading from the child: the
    // producer is throttled to the rate the host consumes. Returns the bytes
    // processed; has_pending_output() says whether to come back sooner than
    // the next wakeup.
    struct UpdateBudget {
        size_t max_bytes{16u << 20};
        std::chrono::microseconds max_time{4000};
        size_t slice{16u << 10};
    };
    size_t update() { return update(UpdateBudget()); }
    size_t update(const UpdateBudget& budget);
    [[nodiscard]] bool has_pending_output() const { return pty_ && pty_->available() > 0; }
    // Feed bytes directly (no PTY).
    void feed(std::string_view bytes) { term_.feed(bytes); }

    // ---- input (input.h), encoded for the terminal's current modes and
    // written to the application. Each returns whether anything was sent.
    bool send_key(const KeyEvent& ev);
    // Typed text with no key behind it (an IME commit). Control characters are
    // dropped; under kitty flags 8+16 it is reported as CSI 0;;text u.
    bool send_text(std::string_view text);
    // Clipboard paste: bracketed (and sanitised) when ?2004 is set; see encode_paste().
    bool paste(std::string_view text);
    // Mouse report for the current tracking mode / encoding, with held-button
    // tracking and same-cell motion suppression (MouseReporter). With tracking
    // off, a wheel up/down press on the alternate screen under ?1007 sends
    // cursor Up / Down instead (honouring DECCKM).
    bool send_mouse(const MouseEvent& ev);
    // Focus in / out report, only when ?1004 is set.
    bool focus(bool focused);
    void resize(int cols, int rows);

    // TerminalHost
    void write_to_pty(std::string_view bytes) override;
    void bell() override;
    void title_changed(std::string_view t) override;
    void icon_name_changed(std::string_view n) override;
    void cwd_changed(std::string_view uri) override;
    void clipboard_write(std::string_view sel, std::string_view data) override;
    std::optional<std::string> clipboard_read(std::string_view sel) override;
    void notification(std::string_view title, std::string_view body) override;
    void progress(int state, int value) override;
    void semantic_mark(char kind, std::string_view params) override;
    void palette_changed() override;
    void apc(std::string_view payload) override;

private:
    Terminal term_;
    std::shared_ptr<IPtyProcess> pty_;
    TerminalHost* delegate_{nullptr};
    OutputCallback output_cb_;
    std::unique_ptr<char[]> read_buf_;
    MouseReporter mouse_;
};

} // namespace bropty
