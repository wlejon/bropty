#include "bropty/session.h"

#include <algorithm>

namespace bropty {

Session::Session() : Session(TerminalOptions()) {}

Session::Session(const TerminalOptions& options) : term_(options) { term_.set_host(this); }

Session::~Session() = default;

void Session::attach_pty(std::shared_ptr<IPtyProcess> pty) {
    pty_ = std::move(pty);
    outbox_.clear();
    outbox_head_ = 0;
    reply_backlog_ = 0;
}

size_t Session::update(const UpdateBudget& budget) {
    if (!pty_) return 0;
    flush_outbox();
    constexpr size_t kBuf = 64u << 10;
    if (!read_buf_) read_buf_ = std::make_unique<char[]>(kBuf);
    const size_t slice = std::clamp<size_t>(budget.slice, 256, kBuf);
    const auto deadline = std::chrono::steady_clock::now() + budget.max_time;
    size_t total = 0;
    while (total < budget.max_bytes) {
        size_t n = pty_->read_nonblocking(read_buf_.get(), std::min(slice, budget.max_bytes - total));
        if (n == 0) break;
        const std::string_view chunk(read_buf_.get(), n);
        if (feed_tap_) feed_tap_(chunk);
        term_.feed(chunk);
        total += n;
        if (std::chrono::steady_clock::now() >= deadline) break;
    }
    return total;
}

void Session::flush_outbox() {
    if (!pty_ || outbox_head_ == outbox_.size()) return;
    outbox_head_ += pty_->write_some(std::string_view(outbox_).substr(outbox_head_));
    if (outbox_head_ == outbox_.size()) {
        outbox_.clear();
        outbox_.shrink_to_fit();
        outbox_head_ = 0;
        reply_backlog_ = 0;
    } else if (!pty_->is_running() && pty_->input_space() == 0) {
        // The child is gone: nothing will ever take the rest.
        outbox_.clear();
        outbox_head_ = 0;
        reply_backlog_ = 0;
    }
}

bool Session::send_event(const std::string& bytes) {
    if (bytes.empty()) return false;
    if (pty_) {
        flush_outbox();
        if (input_blocked() || pty_->write(bytes) != bytes.size()) return false;
    }
    if (output_cb_) output_cb_(bytes);
    return true;
}

bool Session::send_key(const KeyEvent& ev) { return send_event(encode_key(ev, KeyboardModes::from(term_))); }

bool Session::send_text(std::string_view text) {
    KeyEvent ev;
    ev.text = std::string(text);
    return send_event(encode_key(ev, KeyboardModes::from(term_)));
}

bool Session::paste(std::string_view text) {
    if (text.empty()) return false;
    std::string bytes = encode_paste(text, term_.modes().bracketed_paste);
    if (bytes.empty()) return false;
    if (pty_) {
        flush_outbox();
        if (input_blocked()) return false;
        size_t n = pty_->write_some(bytes);
        if (n == 0 && !pty_->is_running()) return false;
        if (n < bytes.size()) {
            outbox_.assign(bytes, n, std::string::npos);
            outbox_head_ = 0;
        }
    }
    if (output_cb_) output_cb_(bytes);
    return true;
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
    return send_event(mouse_.encode(ev, mm));
}

bool Session::focus(bool focused) { return send_event(encode_focus(focused, term_.modes().focus_events)); }

void Session::push_size() {
    if (!pty_) return;
    PtySize s;
    s.cols = term_.cols();
    s.rows = term_.rows();
    s.pixel_width = term_.cols() * std::max(0, term_.cell_pixel_width());
    s.pixel_height = term_.rows() * std::max(0, term_.cell_pixel_height());
    pty_->resize(s);
}

void Session::resize(int cols, int rows) {
    term_.resize(cols, rows);
    push_size();
}

void Session::set_cell_pixel_size(int width, int height) {
    if (width == term_.cell_pixel_width() && height == term_.cell_pixel_height()) return;
    term_.set_cell_pixel_size(width, height);
    push_size();
}

// Replies from the terminal: in order behind a pending paste, bounded.
void Session::write_to_pty(std::string_view bytes) {
    if (bytes.empty()) return;
    if (pty_) {
        flush_outbox();
        if (input_blocked() || pty_->write(bytes) != bytes.size()) {
            if (reply_backlog_ + bytes.size() > kMaxReplyBacklog) return;
            reply_backlog_ += bytes.size();
            outbox_.append(bytes);
        }
    }
    if (output_cb_) output_cb_(bytes);
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
bool Session::clipboard_read_async(uint64_t request, std::string_view sel) {
    return delegate_ && delegate_->clipboard_read_async(request, sel);
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
bool Session::decode_image(std::string_view data, const ImageLimits& limits, DecodedImage& out) {
    return delegate_ && delegate_->decode_image(data, limits, out);
}
void Session::resized_by_application(int cols, int rows) {
    push_size();
    if (delegate_) delegate_->resized_by_application(cols, rows);
}
void Session::pointer_shape_changed(std::string_view name) {
    if (delegate_) delegate_->pointer_shape_changed(name);
}

} // namespace bropty
