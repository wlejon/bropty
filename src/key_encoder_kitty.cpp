// Kitty keyboard protocol (kitty_flags != 0). Follows the spec
// (sw.kovidgoyal.net/kitty/keyboard-protocol) and, where the prose leaves a
// case open, kitty's reference encoder (kitty/key_encoding.c):
//
//   flag 1  disambiguate: Esc, and every text key with a modifier other than
//           shift (and the locks), become CSI u; unmodified / shift-only text
//           stays text; Enter, Tab, Backspace keep their legacy bytes unless
//           modified; F1-F4 and the cursor keys use CSI 1;m x (no SS3, DECCKM
//           ignored); keypad keys that type nothing use their CSI u numbers.
//   flag 2  report event types: repeat ":2", release ":3". Releases of
//           unmodified Enter / Tab / Backspace are only sent under flag 8.
//   flag 4  report alternate keys: code:shifted:base-layout on CSI u forms of
//           text keys (shifted only while shift is held, "::base" when only
//           the base-layout key is known).
//   flag 8  report all keys as escape codes, including text keys, Enter / Tab /
//           Backspace and the modifier keys themselves, with lock modifiers.
//   flag 16 report associated text (only together with 8): ;mods;cp:cp...
//           for press / repeat, control characters excluded; a text event
//           with no key is CSI 0;;cps u.
//
// Deviations: a release is never sent as legacy bytes (kitty re-sends a bare
// ESC for an Escape release under flag 2 without 1); under flag 8 without 16 a
// pure text event (no key, e.g. an IME commit) is sent as plain text rather
// than kitty's information-free CSI 0 u.
#include "input_internal.h"

namespace bropty::input_detail {

namespace {

constexpr unsigned kShift = Mod_Shift, kAlt = Mod_Alt, kCtrl = Mod_Ctrl;

struct Fields {
    uint32_t key{0};
    uint32_t shifted{0};
    uint32_t alternate{0};
    bool add_alternates{false};
    bool has_mods{false};
    bool add_actions{false};
    bool add_text{false};
    unsigned mods{0};
    KeyAction action{KeyAction::Press};
    std::string_view text;
};

std::string serialize(const Fields& d, char trailer) {
    std::string o = "\x1b[";
    const bool second = d.has_mods || d.add_actions;
    const bool third = d.add_text;
    if (d.key != 1 || d.add_alternates || second || third) o += std::to_string(d.key);
    if (d.add_alternates) {
        o += ':';
        if (d.shifted) o += std::to_string(d.shifted);
        if (d.alternate) {
            o += ':';
            o += std::to_string(d.alternate);
        }
    }
    if (second || third) {
        o += ';';
        if (second) o += std::to_string(d.mods + 1);
        if (d.add_actions) {
            o += ':';
            o += std::to_string(int(d.action));
        }
    }
    if (third) {
        bool first = true;
        for_each_code_point(d.text, [&](char32_t cp) {
            o += first ? ';' : ':';
            first = false;
            o += std::to_string(uint32_t(cp));
        });
    }
    o += trailer;
    return o;
}

struct Kitty {
    const NormalizedKey& k;
    const KeyboardModes& m;
    bool disambiguate, events, alternates, all, embed;

    Kitty(const NormalizedKey& key, const KeyboardModes& modes)
        : k(key), m(modes),
          disambiguate(modes.kitty_flags & 1), events(modes.kitty_flags & 2),
          alternates(modes.kitty_flags & 4), all(modes.kitty_flags & 8),
          embed((modes.kitty_flags & 16) && (modes.kitty_flags & 8)) {}

    bool release() const { return k.action == KeyAction::Release; }

    Fields fields(uint32_t key, char32_t shifted, char32_t base) const {
        Fields d;
        d.key = key;
        d.mods = k.mods;
        d.action = k.action;
        d.add_actions = events && k.action != KeyAction::Press;
        d.has_mods = k.mods != 0;
        bool shift = (k.mods & Mod_Shift) != 0;
        d.add_alternates = alternates && ((shifted && shift) || base);
        if (d.add_alternates) {
            d.shifted = shift ? uint32_t(shifted) : 0;
            d.alternate = uint32_t(base);
        }
        d.add_text = embed && !k.text.empty();
        d.text = k.text;
        return d;
    }

    char backspace_byte(bool ctrl) const { return (m.backarrow_sends_bs != ctrl) ? '\x08' : '\x7f'; }

    // kitty's legacy_functional_key_encoding_with_modifiers (flags such as 4
    // alone keep legacy encoding).
    std::string legacy_c0_with_mods(Key key) const {
        std::string prefix = (k.mods & kAlt) ? "\x1b" : "";
        switch (key) {
        case Key::Enter: return prefix + "\r";
        case Key::Escape: return prefix + "\x1b";
        case Key::Backspace: return prefix + backspace_byte(k.mods & kCtrl);
        case Key::Tab:
            if (k.mods & kShift) return ((k.mods & kAlt) ? "\x1b\x1b" : "\x1b") + std::string("[Z");
            return prefix + "\t";
        default: return {};
        }
    }

    std::string c0_plain(Key key) const {
        switch (key) {
        case Key::Enter: return "\r";
        case Key::Backspace: return std::string(1, backspace_byte(false));
        case Key::Tab: return "\t";
        default: return {};
        }
    }
    static bool is_enter_tab_bs(Key key) { return key == Key::Enter || key == Key::Tab || key == Key::Backspace; }

    std::string function_key(Key key) const {
        const bool legacy_mode = !events && !disambiguate && !all;
        const unsigned mods = k.mods;
        if (m.app_cursor_keys && legacy_mode && !mods) {
            switch (key) {
            case Key::Up: return "\x1bOA";
            case Key::Down: return "\x1bOB";
            case Key::Right: return "\x1bOC";
            case Key::Left: return "\x1bOD";
            case Key::KpBegin: return "\x1bOE";
            case Key::End: return "\x1bOF";
            case Key::Home: return "\x1bOH";
            default: break;
            }
        }
        if (!mods) {
            if (!disambiguate && !all && key == Key::Escape && !release()) return "\x1b";
            if (legacy_mode) {
                switch (key) {
                case Key::F1: return "\x1bOP";
                case Key::F2: return "\x1bOQ";
                case Key::F3: return "\x1bOR";
                case Key::F4: return "\x1bOS";
                default: break;
                }
            }
        } else if (legacy_mode) {
            std::string s = legacy_c0_with_mods(key);
            if (!s.empty()) return s;
        }
        if (!(mods & ~unsigned(Mod_LockMask)) && !all && is_enter_tab_bs(key)) {
            return release() ? std::string() : c0_plain(key);
        }

        uint32_t number = uint32_t(key);
        char trailer = 'u';
        switch (key) {
        case Key::Escape: number = 27; break;
        case Key::Enter: number = 13; break;
        case Key::Tab: number = 9; break;
        case Key::Backspace: number = 127; break;
        case Key::Insert: number = 2; trailer = '~'; break;
        case Key::Delete: number = 3; trailer = '~'; break;
        case Key::Left: number = 1; trailer = 'D'; break;
        case Key::Right: number = 1; trailer = 'C'; break;
        case Key::Up: number = 1; trailer = 'A'; break;
        case Key::Down: number = 1; trailer = 'B'; break;
        case Key::PageUp: number = 5; trailer = '~'; break;
        case Key::PageDown: number = 6; trailer = '~'; break;
        case Key::Home: number = 1; trailer = 'H'; break;
        case Key::End: number = 1; trailer = 'F'; break;
        case Key::F1: number = 1; trailer = 'P'; break;
        case Key::F2: number = 1; trailer = 'Q'; break;
        case Key::F3: number = 13; trailer = '~'; break;
        case Key::F4: number = 1; trailer = 'S'; break;
        case Key::F5: number = 15; trailer = '~'; break;
        case Key::F6: number = 17; trailer = '~'; break;
        case Key::F7: number = 18; trailer = '~'; break;
        case Key::F8: number = 19; trailer = '~'; break;
        case Key::F9: number = 20; trailer = '~'; break;
        case Key::F10: number = 21; trailer = '~'; break;
        case Key::F11: number = 23; trailer = '~'; break;
        case Key::F12: number = 24; trailer = '~'; break;
        case Key::KpBegin: number = 1; trailer = 'E'; break;
        case Key::Menu:
            if (legacy_mode) { number = 29; trailer = '~'; }
            break;
        default: break;
        }
        Fields d = fields(number, 0, 0);
        d.add_alternates = false;
        return serialize(d, trailer);
    }

    // kitty's encode_printable_ascii_key_legacy: used when neither flag 1 nor 8 is set.
    std::string printable_legacy(char32_t code, char32_t shifted) const {
        unsigned mods = k.mods;
        char32_t key = code;
        if (mods & kShift) {
            if (shifted && shifted != key && (!(mods & kCtrl) || key < 'a' || key > 'z')) {
                key = shifted;
                mods &= ~kShift;
            }
        }
        auto esc = [](char32_t c) {
            std::string s = "\x1b";
            append_utf8(s, c);
            return s;
        };
        if (k.mods == kShift) return utf8(key);
        if (mods == kAlt) return esc(key);
        if (mods == kCtrl) return utf8(ctrl_map(key));
        if (mods == (kCtrl | kAlt)) return esc(ctrl_map(key));
        if (key == ' ') {
            if (mods == (kCtrl | kShift)) return utf8(ctrl_map(key));
            if (mods == (kAlt | kShift)) return esc(key);
        }
        return {};
    }

    std::string text_key(char32_t code, char32_t shifted, char32_t base) const {
        Fields d = fields(uint32_t(code), shifted, base);
        const bool simple = !d.add_actions && !d.add_alternates && !d.add_text;
        if (simple) {
            if (!d.has_mods) return all ? serialize(d, 'u') : utf8(code);
            if (!disambiguate && !all) {
                if (is_legacy_ascii_key(code) || (shifted && is_legacy_ascii_key(shifted))) {
                    std::string s = printable_legacy(code, shifted);
                    if (!s.empty()) return s;
                }
                unsigned mods = k.mods;
                if ((mods == kCtrl || mods == kAlt || mods == (kCtrl | kAlt)) && base &&
                    !is_legacy_ascii_key(code) && is_legacy_ascii_key(base)) {
                    std::string s = printable_legacy(base, 0);
                    if (!s.empty()) return s;
                }
            }
        }
        return serialize(d, 'u');
    }

    std::string encode() const {
        if (!events && release()) return {};
        Key key = k.key;
        char32_t code = k.code, shifted = k.shifted, base = k.base;
        if (key != Key::None && is_modifier_key(key) && !all) return {};
        if (key != Key::None && is_keypad(key) && !disambiguate && !all) {
            // Without disambiguation the keypad reports as the main-block keys.
            if (char32_t c = keypad_char(key)) {
                key = Key::None;
                code = c;
                shifted = 0;
                base = 0;
            } else if (Key nk = keypad_to_normal(key); nk != Key::None) {
                key = nk;
            }
        }
        if (key == Key::None && code == 0) {  // pure text event
            if (k.text.empty()) return {};
            if (embed) {
                Fields d = fields(0, 0, 0);
                return serialize(d, 'u');
            }
            return k.text;
        }
        if (!all && !k.text.empty() && !release()) return k.text;
        if (key != Key::None) return function_key(key);
        return text_key(code, shifted, base);
    }
};

} // namespace

std::string encode_kitty(const NormalizedKey& k, const KeyboardModes& m) { return Kitty(k, m).encode(); }

} // namespace bropty::input_detail
