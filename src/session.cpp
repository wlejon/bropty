#include "bropty/session.h"

namespace bropty {

Session::Session() : Session(TerminalOptions()) {}

Session::Session(const TerminalOptions& options) : term_(options) { term_.set_host(this); }

Session::~Session() = default;

void Session::attach_pty(std::shared_ptr<IPtyProcess> pty) { pty_ = std::move(pty); }

size_t Session::update() {
    if (!pty_) return 0;
    char buf[64 * 1024];
    size_t total = 0;
    for (;;) {
        size_t n = pty_->read_nonblocking(buf, sizeof buf);
        if (n == 0) break;
        term_.feed(std::string_view(buf, n));
        total += n;
    }
    return total;
}

void Session::send_key(Key key, uint32_t codepoint, uint8_t modifiers, KeyEventType event_type) {
    const Modes& m = term_.modes();
    write_to_pty(KeyEncoder::encode_key(key, codepoint, modifiers, m.app_cursor_keys,
                                        term_.kitty_keyboard_flags() != 0, event_type));
}

void Session::send_text(std::string_view text) {
    write_to_pty(KeyEncoder::encode_paste(text, term_.modes().bracketed_paste));
}

void Session::send_mouse(MouseButton button, MouseAction action, uint8_t modifiers, int col, int row) {
    if (term_.modes().mouse_tracking == MouseTracking::None) return;
    write_to_pty(KeyEncoder::encode_mouse_sgr(button, action, modifiers, col, row));
}

void Session::resize(int cols, int rows) {
    term_.resize(cols, rows);
    if (pty_) pty_->resize(term_.cols(), term_.rows());
}

void Session::write_to_pty(std::string_view bytes) {
    if (bytes.empty()) return;
    if (output_cb_) output_cb_(bytes);
    if (pty_) pty_->write(bytes);
}

void Session::bell() { if (delegate_) delegate_->bell(); }
void Session::title_changed(std::string_view t) { if (delegate_) delegate_->title_changed(t); }
void Session::icon_name_changed(std::string_view n) { if (delegate_) delegate_->icon_name_changed(n); }
void Session::cwd_changed(std::string_view uri) { if (delegate_) delegate_->cwd_changed(uri); }
void Session::clipboard_write(std::string_view sel, std::string_view data) {
    if (delegate_) delegate_->clipboard_write(sel, data);
}
std::optional<std::string> Session::clipboard_read(std::string_view sel) {
    return delegate_ ? delegate_->clipboard_read(sel) : std::nullopt;
}
void Session::notification(std::string_view title, std::string_view body) {
    if (delegate_) delegate_->notification(title, body);
}
void Session::progress(int state, int value) { if (delegate_) delegate_->progress(state, value); }
void Session::semantic_mark(char kind, std::string_view params) {
    if (delegate_) delegate_->semantic_mark(kind, params);
}
void Session::palette_changed() { if (delegate_) delegate_->palette_changed(); }
void Session::apc(std::string_view payload) { if (delegate_) delegate_->apc(payload); }

} // namespace bropty
