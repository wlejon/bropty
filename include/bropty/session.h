#pragma once
// Session: a Terminal attached to a PTY process. It owns the plumbing that
// Terminal deliberately lacks: draining PTY output into the emulator, writing
// the emulator's replies back to the PTY, and encoding keyboard / mouse /
// paste input according to the terminal's current modes.
//
// Embedder events (bell, title, clipboard, ...) are forwarded to an optional
// delegate TerminalHost.
//
// NOTE: key/mouse encoding and the PTY layer are being reworked; this class is
// the seam they plug into (Terminal::modes(), Terminal::kitty_keyboard_flags()).

#include "bropty/key_encoder.h"
#include "bropty/pty.h"
#include "bropty/terminal.h"

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

    // Drain pending PTY output into the terminal; returns bytes processed.
    size_t update();
    // Feed bytes directly (no PTY).
    void feed(std::string_view bytes) { term_.feed(bytes); }

    void send_key(Key key, uint32_t codepoint = 0, uint8_t modifiers = Mod_None,
                  KeyEventType event_type = KeyEventType::Press);
    void send_text(std::string_view text);
    void send_mouse(MouseButton button, MouseAction action, uint8_t modifiers, int col, int row);
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
};

} // namespace bropty
