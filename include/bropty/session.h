#pragma once
// Session: a Terminal attached to a PTY process. It owns the plumbing that
// Terminal deliberately lacks: draining PTY output into the emulator, writing
// the emulator's replies back to the PTY, and encoding keyboard / mouse /
// paste input according to the terminal's current modes.
//
// Embedder events (bell, title, clipboard, ...) are forwarded to an optional
// delegate TerminalHost.
//
// Input backpressure. The pty's input queue is bounded
// (PtyConfig::input_buffer_bytes); when a child stops reading, input is
// refused rather than buffered without limit, and the host can see it:
//  * Key, text, mouse and focus events are all-or-nothing: send_*() returns
//    false when the event could not be queued (the child is not reading; it
//    is dropped, as a full tty input queue drops keystrokes).
//  * A paste larger than the free space is accepted and finished in the
//    background: what does not fit waits in the Session's outbox and update()
//    feeds it to the pty as the child reads. While it is pending,
//    input_blocked() is true and further input events and pastes are refused
//    (they would otherwise land inside the bracketed paste).
//  * Replies the terminal generates (DA, DSR, OSC queries) queue behind a
//    pending paste; at most kMaxReplyBacklog bytes of them, beyond which an
//    application flooding queries without reading their answers loses them.
// The pty's wakeup hook fires when a full queue drains, so a host that saw
// input_blocked() needs no polling to call update() again.

#include "bropty/input.h"
#include "bropty/pty.h"
#include "bropty/terminal.h"

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace bropty {

class Session final : public TerminalHost {
public:
    using OutputCallback = std::function<void(std::string_view)>;
    static constexpr size_t kMaxReplyBacklog = 64u << 10;

    Session();
    explicit Session(const TerminalOptions& options);
    ~Session() override;

    [[nodiscard]] Terminal& terminal() noexcept { return term_; }
    [[nodiscard]] const Terminal& terminal() const noexcept { return term_; }

    void attach_pty(std::shared_ptr<IPtyProcess> pty);
    [[nodiscard]] const std::shared_ptr<IPtyProcess>& pty() const noexcept { return pty_; }
    void set_delegate(TerminalHost* host) noexcept { delegate_ = host; }
    // Observe (and, without a PTY, capture) every byte sent to the
    // application, at the moment the Session accepts it.
    void set_output_callback(OutputCallback cb) { output_cb_ = std::move(cb); }

    // Feed pending PTY output into the terminal, bounded so that a fast
    // producer cannot stall the host's frame: update() stops after
    // `max_bytes` or once `max_time` has elapsed (checked every `slice`
    // bytes), whichever comes first. Unconsumed output stays in the PTY's
    // bounded buffer, which in turn stops reading from the child: the
    // producer is throttled to the rate the host consumes. Returns the bytes
    // processed; has_pending_output() says whether to come back sooner than
    // the next wakeup. update() also moves a pending paste on (see above).
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
    // written to the application. Each returns whether it was accepted.
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

    // Input not yet accepted by the pty (a paste in progress, replies behind it).
    [[nodiscard]] size_t pending_input_bytes() const noexcept { return outbox_.size() - outbox_head_; }
    // True while a paste is still being delivered: input events are refused.
    [[nodiscard]] bool input_blocked() const noexcept { return pending_input_bytes() > 0; }

    // Resize the terminal and the pty. The pty's pixel size (TIOCSWINSZ
    // ws_xpixel / ws_ypixel, which programs drawing sixel or kitty graphics
    // read) is the grid times the cell size from set_cell_pixel_size().
    void resize(int cols, int rows);
    // Cell size in pixels: answers XTWINOPS 14/16 and is passed to the pty.
    void set_cell_pixel_size(int width, int height);

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
    bool decode_image(std::string_view data, const ImageLimits& limits, DecodedImage& out) override;
    void resized_by_application(int cols, int rows) override;

private:
    bool send_event(const std::string& bytes);  // all-or-nothing input event
    void flush_outbox();
    void push_size();

    Terminal term_;
    std::shared_ptr<IPtyProcess> pty_;
    TerminalHost* delegate_{nullptr};
    OutputCallback output_cb_;
    std::unique_ptr<char[]> read_buf_;
    MouseReporter mouse_;
    std::string outbox_;      // accepted, not yet taken by the pty, in order
    size_t outbox_head_{0};   // bytes of outbox_ already handed to the pty
    size_t reply_backlog_{0}; // reply bytes queued behind a paste
};

} // namespace bropty
