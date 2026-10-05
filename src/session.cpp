#include "bropty/session.h"

#include <algorithm>

namespace bropty {

Session::Session() : Session(TerminalOptions()) {}

Session::Session(const TerminalOptions& options) : term_(options) { term_.set_host(this); }

Session::~Session() = default;

void Session::attach_pty(std::shared_ptr<IPtyProcess> pty) { pty_ = std::move(pty); }

size_t Session::update(const UpdateBudget& budget) {
    if (!pty_) return 0;
    constexpr size_t kBuf = 64u << 10;
    if (!read_buf_) read_buf_ = std::make_unique<char[]>(kBuf);
    const size_t slice = std::clamp<size_t>(budget.slice, 256, kBuf);
    const auto deadline = std::chrono::steady_clock::now() + budget.max_time;
    size_t total = 0;
    while (total < budget.max_bytes) {
        size_t n = pty_->read_nonblocking(read_buf_.get(), std::min(slice, budget.max_bytes - total));
        if (n == 0) break;
        term_.feed(std::string_view(read_buf_.get(), n));
        total += n;
        if (std::chrono::steady_clock::now() >= deadline) break;
    }
    return total;
}

namespace {
bool send(Session& s, const std::string& bytes) {
    if (bytes.empty()) return false;
    s.write_to_pty(bytes);
    return true;
}
} // namespace

bool Session::send_key(const KeyEvent& ev) { return send(*this, encode_key(ev, KeyboardModes::from(term_))); }

bool Session::send_text(std::string_view text) {
    KeyEvent ev;
    ev.text = std::string(text);
    return send(*this, encode_key(ev, KeyboardModes::from(term_)));
}

bool Session::paste(std::string_view text) {
    if (text.empty()) return false;
    return send(*this, encode_paste(text, term_.modes().bracketed_paste));
}

bool Session::send_mouse(const MouseEvent& ev) {
    const MouseModes mm = MouseModes::from(term_);
    if (mm.tracking == MouseTracking::None) {
        (void)mouse_.encode(ev, mm);  // keep held-button state current
        const bool wheel_v = ev.button == MouseButton::WheelUp || ev.button == MouseButton::WheelDown;
        if (ev.action == MouseAction::Press && wheel_v && term_.modes().alternate_scroll &&
            term_.alt_screen_active()) {
            Key k = ev.button == MouseButton::WheelUp ? Key::Up : Key::Down;
            return send_key(KeyEvent::functional(k));
        }
        return false;
    }
    return send(*this, mouse_.encode(ev, mm));
}

bool Session::focus(bool focused) { return send(*this, encode_focus(focused, term_.modes().focus_events)); }

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
