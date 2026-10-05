// Mouse reports, following xterm (button.c: EditorButton / BtnCode /
// EmitButtonCode / EmitMousePosition).
//
// Tracking modes: X10 (?9) reports presses of buttons 1-3 only, without
// modifiers; normal (?1000) presses and releases, wheel included; button-event
// (?1002) adds motion while a button is held; any-event (?1003) all motion.
// Button codes: 0-2 buttons, 3 release (non-SGR encodings), 64-67 wheel
// up/down/left/right, 128-131 buttons 8-11; +4 shift, +8 meta (Alt or Meta),
// +16 ctrl; +32 motion. Wheel "buttons" have no release.
// Encodings: default CSI M Cb Cx Cy with each value + 32 in one byte (column /
// row up to 223); UTF-8 (?1005) the same with values >= 128 as two-byte UTF-8
// (up to 2015); SGR (?1006) CSI < b ; x ; y M|m, where a release keeps its
// button and ends in 'm'; SGR-pixels (?1016) the same with 1-based pixel
// coordinates; urxvt (?1015) CSI 32+b ; x ; y M. A position the encoding cannot
// represent drops the event (xterm clamps it to the last representable cell
// instead, which reports a click in the wrong place).
#include "input_internal.h"

#include <algorithm>

namespace bropty {

namespace {

int button_code(MouseButton b) {
    switch (b) {
    case MouseButton::Left: return 0;
    case MouseButton::Middle: return 1;
    case MouseButton::Right: return 2;
    case MouseButton::WheelUp: return 64;
    case MouseButton::WheelDown: return 65;
    case MouseButton::WheelLeft: return 66;
    case MouseButton::WheelRight: return 67;
    case MouseButton::Button8: return 128;
    case MouseButton::Button9: return 129;
    case MouseButton::Button10: return 130;
    case MouseButton::Button11: return 131;
    case MouseButton::None: break;
    }
    return 3;
}

bool is_wheel(MouseButton b) {
    return b == MouseButton::WheelUp || b == MouseButton::WheelDown || b == MouseButton::WheelLeft ||
           b == MouseButton::WheelRight;
}

void put_legacy_value(std::string& out, int value, bool utf8_ext) {
    // value already includes the +32 offset
    if (utf8_ext && value >= 0x80) {
        out.push_back(char(0xc0 | (value >> 6)));
        out.push_back(char(0x80 | (value & 0x3f)));
    } else {
        out.push_back(char(value));
    }
}

} // namespace

MouseModes MouseModes::from(const Terminal& t) noexcept {
    MouseModes m;
    m.tracking = t.modes().mouse_tracking;
    m.encoding = t.modes().mouse_encoding;
    return m;
}

std::string encode_mouse(const MouseEvent& ev, const MouseModes& modes, MouseButton held) {
    const MouseTracking tr = modes.tracking;
    if (tr == MouseTracking::None) return {};
    const bool x10 = tr == MouseTracking::X10;

    int cb = 0;
    bool release = false;
    switch (ev.action) {
    case MouseAction::Press:
        if (ev.button == MouseButton::None) return {};
        if (x10 && button_code(ev.button) > 2) return {};
        cb = button_code(ev.button);
        break;
    case MouseAction::Release:
        if (x10 || ev.button == MouseButton::None || is_wheel(ev.button)) return {};
        release = true;
        cb = button_code(ev.button);
        break;
    case MouseAction::Motion:
        if (tr == MouseTracking::X10 || tr == MouseTracking::Normal) return {};
        if (tr == MouseTracking::Button && held == MouseButton::None) return {};
        cb = button_code(held) + 32;
        break;
    }
    if (!x10) {
        if (ev.mods & Mod_Shift) cb += 4;
        if (ev.mods & (Mod_Alt | Mod_Meta)) cb += 8;
        if (ev.mods & Mod_Ctrl) cb += 16;
    }
    const bool sgr = modes.encoding == MouseEncoding::Sgr || modes.encoding == MouseEncoding::SgrPixels;
    if (release && !sgr) cb = (cb & ~3 & ~(64 | 128)) | 3;  // "some button released", modifiers kept

    int x = std::max(0, ev.col) + 1;
    int y = std::max(0, ev.row) + 1;
    if (modes.encoding == MouseEncoding::SgrPixels) {
        x = std::max(0, ev.x) + 1;
        y = std::max(0, ev.y) + 1;
    }

    std::string out = "\x1b[";
    switch (modes.encoding) {
    case MouseEncoding::Default:
    case MouseEncoding::Utf8: {
        const bool ext = modes.encoding == MouseEncoding::Utf8;
        const int limit = ext ? 2015 : 223;
        if (x > limit || y > limit) return {};
        out += 'M';
        put_legacy_value(out, cb + 32, ext);
        put_legacy_value(out, x + 32, ext);
        put_legacy_value(out, y + 32, ext);
        return out;
    }
    case MouseEncoding::Sgr:
    case MouseEncoding::SgrPixels:
        out += '<' + std::to_string(cb) + ';' + std::to_string(x) + ';' + std::to_string(y);
        out += release ? 'm' : 'M';
        return out;
    case MouseEncoding::Urxvt:
        out += std::to_string(cb + 32) + ';' + std::to_string(x) + ';' + std::to_string(y) + 'M';
        return out;
    }
    return {};
}

std::string MouseReporter::encode(const MouseEvent& ev, const MouseModes& modes) {
    const auto bit = [](MouseButton b) { return uint16_t(1u << unsigned(b)); };
    if (!is_wheel(ev.button) && ev.button != MouseButton::None) {
        if (ev.action == MouseAction::Press) held_ |= bit(ev.button);
        else if (ev.action == MouseAction::Release) held_ &= uint16_t(~bit(ev.button));
    }
    const bool pixels = modes.encoding == MouseEncoding::SgrPixels;
    const int px = pixels ? ev.x : ev.col;
    const int py = pixels ? ev.y : ev.row;
    if (ev.action == MouseAction::Motion && have_last_ && px == last_x_ && py == last_y_) return {};
    std::string out = encode_mouse(ev, modes, held_button());
    if (!out.empty()) {
        have_last_ = true;
        last_x_ = px;
        last_y_ = py;
    }
    return out;
}

void MouseReporter::reset() noexcept {
    held_ = 0;
    have_last_ = false;
}

MouseButton MouseReporter::held_button() const noexcept {
    // xterm reports the lowest-numbered held button.
    for (MouseButton b : {MouseButton::Left, MouseButton::Middle, MouseButton::Right, MouseButton::Button8,
                          MouseButton::Button9, MouseButton::Button10, MouseButton::Button11}) {
        if (held_ & (1u << unsigned(b))) return b;
    }
    return MouseButton::None;
}

// ---- paste / focus -----------------------------------------------------------

std::string encode_paste(std::string_view text, bool bracketed) {
    std::string out;
    out.reserve(text.size() + 12);
    if (!bracketed) {
        for (size_t i = 0; i < text.size(); ++i) {
            char c = text[i];
            if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
                out.push_back('\r');
                ++i;
            } else if (c == '\n') {
                out.push_back('\r');
            } else {
                out.push_back(c);
            }
        }
        return out;
    }
    out += "\x1b[200~";
    bool prev_cr = false;
    input_detail::for_each_code_point(text, [&](char32_t cp) {
        const bool was_cr = prev_cr;
        prev_cr = cp == '\r';
        if (cp == '\n') {
            if (!was_cr) out.push_back('\r');
            return;
        }
        if (cp == '\r' || cp == '\t') {
            out.push_back(char(cp));
            return;
        }
        if (input_detail::is_control(cp)) return;  // ESC, other C0, DEL, C1
        input_detail::append_utf8(out, cp);
    });
    out += "\x1b[201~";
    return out;
}

std::string encode_focus(bool focused, bool focus_events) {
    if (!focus_events) return {};
    return focused ? "\x1b[I" : "\x1b[O";
}

} // namespace bropty
