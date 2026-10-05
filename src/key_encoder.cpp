// Key encoding entry point and the helpers both encoders share.
#include "input_internal.h"

namespace bropty {

namespace input_detail {

void append_utf8(std::string& out, char32_t cp) {
    if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) cp = 0xfffd;
    if (cp < 0x80) {
        out.push_back(char(cp));
    } else if (cp < 0x800) {
        out.push_back(char(0xc0 | (cp >> 6)));
        out.push_back(char(0x80 | (cp & 0x3f)));
    } else if (cp < 0x10000) {
        out.push_back(char(0xe0 | (cp >> 12)));
        out.push_back(char(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(char(0x80 | (cp & 0x3f)));
    } else {
        out.push_back(char(0xf0 | (cp >> 18)));
        out.push_back(char(0x80 | ((cp >> 12) & 0x3f)));
        out.push_back(char(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(char(0x80 | (cp & 0x3f)));
    }
}

std::string utf8(char32_t cp) {
    std::string s;
    append_utf8(s, cp);
    return s;
}

char32_t us_shifted(char32_t cp) noexcept {
    if (cp >= 'a' && cp <= 'z') return cp - 'a' + 'A';
    switch (cp) {
    case '`': return '~';
    case '1': return '!';
    case '2': return '@';
    case '3': return '#';
    case '4': return '$';
    case '5': return '%';
    case '6': return '^';
    case '7': return '&';
    case '8': return '*';
    case '9': return '(';
    case '0': return ')';
    case '-': return '_';
    case '=': return '+';
    case '[': return '{';
    case ']': return '}';
    case '\\': return '|';
    case ';': return ':';
    case '\'': return '"';
    case ',': return '<';
    case '.': return '>';
    case '/': return '?';
    default: return 0;
    }
}

char32_t ctrl_map(char32_t cp) noexcept {
    if (cp >= 'A' && cp <= 'Z') cp = cp - 'A' + 'a';
    if (cp >= 'a' && cp <= 'z') return cp - 'a' + 1;
    switch (cp) {
    case ' ': return 0;
    case '/': return 31;
    case '2': return 0;
    case '3': return 27;
    case '4': return 28;
    case '5': return 29;
    case '6': return 30;
    case '7': return 31;
    case '8': return 127;
    case '?': return 127;
    case '@': return 0;
    case '[': return 27;
    case '\\': return 28;
    case ']': return 29;
    case '^': return 30;
    case '_': return 31;
    case '~': return 30;
    default: return cp;
    }
}

bool is_legacy_ascii_key(char32_t cp) noexcept {
    if ((cp >= 'a' && cp <= 'z') || (cp >= '0' && cp <= '9')) return true;
    switch (cp) {
    case '!': case '@': case '#': case '$': case '%': case '^': case '&': case '*':
    case '(': case ')': case '`': case '~': case '-': case '_': case '=': case '+':
    case '[': case '{': case ']': case '}': case '\\': case '|': case ';': case ':':
    case '\'': case '"': case ',': case '<': case '.': case '>': case '/': case '?':
    case ' ':
        return true;
    default:
        return false;
    }
}

char32_t keypad_char(Key k) noexcept {
    uint32_t v = uint32_t(k);
    if (v >= uint32_t(Key::Kp0) && v <= uint32_t(Key::Kp9)) return char32_t('0' + (v - uint32_t(Key::Kp0)));
    switch (k) {
    case Key::KpDecimal: return '.';
    case Key::KpDivide: return '/';
    case Key::KpMultiply: return '*';
    case Key::KpSubtract: return '-';
    case Key::KpAdd: return '+';
    case Key::KpEqual: return '=';
    case Key::KpSeparator: return ',';
    default: return 0;
    }
}

Key keypad_to_normal(Key k) noexcept {
    switch (k) {
    case Key::KpEnter: return Key::Enter;
    case Key::KpLeft: return Key::Left;
    case Key::KpRight: return Key::Right;
    case Key::KpUp: return Key::Up;
    case Key::KpDown: return Key::Down;
    case Key::KpPageUp: return Key::PageUp;
    case Key::KpPageDown: return Key::PageDown;
    case Key::KpHome: return Key::Home;
    case Key::KpEnd: return Key::End;
    case Key::KpInsert: return Key::Insert;
    case Key::KpDelete: return Key::Delete;
    default: return Key::None;
    }
}

NormalizedKey normalize(const KeyEvent& ev) {
    NormalizedKey k;
    k.key = ev.key;
    k.mods = ev.mods;
    k.action = ev.action;
    if (k.key == Key::None && ev.codepoint != 0) {
        char32_t cp = ev.codepoint;
        char32_t sh = ev.shifted;
        if (cp >= 'A' && cp <= 'Z') {
            if (sh == 0) sh = cp;
            cp = cp - 'A' + 'a';
        }
        if (sh == 0) sh = us_shifted(cp);
        k.code = cp;
        k.shifted = sh == cp ? 0 : sh;
        k.base = ev.base_layout == cp ? 0 : ev.base_layout;
    }
    if (k.action == KeyAction::Release) return k;
    if (k.mods & (Mod_Alt | Mod_Ctrl | Mod_Super | Mod_Hyper | Mod_Meta)) return k;

    auto keep = [&](char32_t c) {
        if (!is_control(c)) append_utf8(k.text, c);
    };
    if (!ev.text.empty()) {
        for_each_code_point(ev.text, keep);
    } else if (k.code != 0) {
        bool shift = (k.mods & Mod_Shift) != 0;
        bool letter = k.code >= 'a' && k.code <= 'z';
        char32_t c = k.code;
        if (letter) {
            if (shift != ((k.mods & Mod_CapsLock) != 0)) c = k.shifted ? k.shifted : c;
        } else if (shift && k.shifted) {
            c = k.shifted;
        }
        keep(c);
    } else if (char32_t c = keypad_char(k.key)) {
        keep(c);
    }
    return k;
}

} // namespace input_detail

KeyboardModes KeyboardModes::from(const Terminal& t) noexcept {
    const Modes& m = t.modes();
    KeyboardModes k;
    k.kitty_flags = t.kitty_keyboard_flags();
    k.app_cursor_keys = m.app_cursor_keys;
    k.app_keypad = m.app_keypad;
    k.backarrow_sends_bs = m.backarrow_sends_bs;
    k.modify_other_keys = m.modify_other_keys;
    k.alt_sends_escape = m.alt_sends_escape;
    k.meta_sends_escape = m.meta_sends_escape;
    return k;
}

std::string encode_key(const KeyEvent& ev, const KeyboardModes& modes) {
    input_detail::NormalizedKey k = input_detail::normalize(ev);
    if (modes.kitty_flags != 0) return input_detail::encode_kitty(k, modes);
    return input_detail::encode_legacy(k, modes);
}

} // namespace bropty
