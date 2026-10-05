// Legacy key encoding (kitty keyboard flags == 0): xterm's behaviour.
//
// * Cursor keys, Home/End and KP_Begin: SS3 x under DECCKM, CSI x otherwise;
//   any modifier gives CSI 1;m x. F1-F4: SS3 P..S, modified CSI 1;m P..S.
//   Editing keys and F5-F12: CSI n ~ / CSI n;m ~. Menu: CSI 29 ~ (xterm F16).
//   The modifier parameter is xterm's: 1 + shift(1) + alt(2) + ctrl(4) +
//   meta(8); Super is folded into the meta bit, Hyper and the lock keys are not
//   representable and dropped.
// * Keys with no legacy encoding (F13-F35, PrintScreen, Pause, media keys) use
//   the kitty CSI u form, as kitty does in legacy mode; modifier and lock keys
//   send nothing.
// * Keypad: under DECKPAM, and without NumLock (xterm's realNumLock), the
//   SS3 p..y / n o j m k M X l forms, modified SS3 m x; otherwise the keypad
//   types its character (KP_Enter CR) with modifiers ignored, as xterm does;
//   keypad navigation keys are the main block's.
// * Text keys: ctrl applies the C0 mapping (the kitty spec's table, which is
//   X11's plus ctrl+? / ctrl+~), Alt / Meta prepend ESC when ?1039 / ?1036
//   allow it and otherwise set the 8th bit, sent as the UTF-8 encoding of
//   (c | 0x80) as xterm does in UTF-8 mode (only for single ASCII bytes;
//   anything else is sent unchanged). Super and Hyper are ignored, as in xterm.
//   A non-ASCII key with ctrl/alt falls back to its base-layout key when that
//   is an ASCII key (kitty does this; it keeps ctrl+C working on Cyrillic).
//   Unlike kitty's legacy mode, combinations outside the spec's table (e.g.
//   ctrl+shift+a) keep xterm's bytes (^A) rather than a CSI u form.
// * modifyOtherKeys (XTMODKEYS 4): levels 1 and 2 follow xterm's
//   ModifyOtherKeys() / allowedCharModifiers() (input.c), with the keysym being
//   the shifted key when shift is held and XLookupString's result being the
//   ctrl mapping above. Level 3 is treated as 2. Shift+Tab stays CSI Z except at
//   level 2 with another modifier (CSI 27;m;9~).
#include "input_internal.h"

namespace bropty::input_detail {

namespace {

constexpr unsigned XS = 1, XA = 2, XC = 4, XM = 8;  // xterm modifier bits

std::string num(unsigned v) { return std::to_string(v); }

struct Legacy {
    const NormalizedKey& k;
    const KeyboardModes& m;

    bool shift() const { return (k.mods & Mod_Shift) != 0; }
    bool ctrl() const { return (k.mods & Mod_Ctrl) != 0; }
    bool alt_esc() const {
        return ((k.mods & Mod_Alt) && (m.alt_sends_escape || m.meta_sends_escape)) ||
               ((k.mods & Mod_Meta) && m.meta_sends_escape);
    }
    bool eight_bit() const { return (k.mods & (Mod_Alt | Mod_Meta)) && !alt_esc(); }

    // xterm state for modifyOtherKeys (Super / Hyper are not X modifiers xterm reads).
    unsigned xstate() const {
        unsigned s = 0;
        if (k.mods & Mod_Shift) s |= XS;
        if (k.mods & Mod_Alt) s |= XA;
        if (k.mods & Mod_Ctrl) s |= XC;
        if (k.mods & Mod_Meta) s |= XM;
        return s;
    }
    // Modifier parameter for functional keys (0 = unmodified).
    unsigned fparam() const {
        unsigned s = xstate() & (XS | XA | XC);
        if (k.mods & (Mod_Meta | Mod_Super)) s |= XM;
        return s ? s + 1 : 0;
    }

    // One character with Alt applied.
    std::string with_alt(char32_t c) const {
        std::string out;
        if (alt_esc()) {
            out.push_back('\x1b');
            append_utf8(out, c);
        } else if (eight_bit() && c < 0x80) {
            append_utf8(out, c | 0x80);
        } else {
            append_utf8(out, c);
        }
        return out;
    }

    static std::string csi_letter(unsigned param, char final_char, bool ss3) {
        if (param == 0) return std::string(ss3 ? "\x1bO" : "\x1b[") + final_char;
        return "\x1b[1;" + num(param) + final_char;
    }
    static std::string csi_tilde(unsigned n, unsigned param) {
        if (param == 0) return "\x1b[" + num(n) + "~";
        return "\x1b[" + num(n) + ";" + num(param) + "~";
    }

    enum class Kind { Text, Return, Tab, Escape, Backspace };

    // xterm's modifyOtherKeys decision. Returns the CSI 27;m;code~ form, or
    // empty when the key keeps its ordinary encoding.
    std::string modify_other(Kind kind, char32_t ks) const {
        int level = m.modify_other_keys > 2 ? 2 : m.modify_other_keys;
        const unsigned state = xstate();
        if (level <= 0 || state == 0) return {};
        const bool alt_e = m.alt_sends_escape || m.meta_sends_escape;
        const bool meta_e = m.meta_sends_escape;
        const bool predefined = kind != Kind::Text;
        const bool ctrl_input = kind == Kind::Text && ks >= 0x40 && ks <= 0x7f;
        const bool ctrl_output = kind == Kind::Text && (ks < 0x20 || ks == 0x7f);
        const bool ret_tab = kind == Kind::Return || kind == Kind::Tab;
        char32_t strbuf = 0;
        switch (kind) {
        case Kind::Text: strbuf = (state & XC) ? ctrl_map(ks) : ks; break;
        case Kind::Return: strbuf = '\r'; break;
        case Kind::Tab: strbuf = '\t'; break;
        case Kind::Escape: strbuf = 0x1b; break;
        case Kind::Backspace: strbuf = 0x08; break;
        }
        const bool ctrl_alias = strbuf < 0x20 || strbuf == 0x7f;

        auto allowed = [&](unsigned r) {
            if (level >= 2) return r;
            if (ctrl_input && (r & ~XC) == 0) {
            } else if (ret_tab) {
            } else if (ctrl_alias) {
                if ((r & ~(XC | XS)) == 0) r = 0;
            } else if (!ctrl_output && !predefined) {
                if (!(r & XC)) r &= ~XS;
            }
            auto filter = [&](unsigned mask, bool sends_escape) {
                if (!(r & mask)) return;
                if (sends_escape) r &= ~mask;
                if ((r & ~mask) == 0) r &= ~mask;
                if ((ctrl_input || ctrl_output) && (r & XC)) r &= ~(mask | XC);
                if (ret_tab) r &= ~(mask | XC);
            };
            filter(XM, meta_e);
            filter(XA, alt_e);
            return r;
        };

        unsigned s = state;
        bool is_delete = false;
        if (kind == Kind::Backspace && (!m.backarrow_sends_bs) != ((s & XC) != 0)) {
            is_delete = true;  // the backarrow key sends DEL: xterm treats it as Delete
            s &= ~XC;
        }
        if (!predefined) s = allowed(s);
        if (s == 0) return {};

        bool result = false;
        if (level == 1) {
            switch (kind) {
            case Kind::Backspace: result = false; break;
            case Kind::Return:
            case Kind::Tab: result = true; break;
            default:
                if (ctrl_input) result = !(s == XC || s == XS);
                else if (ctrl_alias) result = s != XS && (s & ~XC) != 0;
                else result = true;
                break;
            }
        } else {
            switch (kind) {
            case Kind::Backspace: result = is_delete ? s != 0 : (s & ~XC) != 0; break;
            case Kind::Return:
            case Kind::Tab:
            case Kind::Escape: result = true; break;
            case Kind::Text: result = ctrl_input || (s == XS && ks == ' ') || (s & ~XS) != 0; break;
            }
        }
        if (!result) return {};
        unsigned fin = allowed(state);
        if (fin == 0) return {};
        unsigned code = 0;
        switch (kind) {
        case Kind::Text: code = unsigned(ks); break;
        case Kind::Return: code = 13; break;
        case Kind::Tab: code = 9; break;
        case Kind::Escape: code = 27; break;
        case Kind::Backspace: code = is_delete ? 127 : 8; break;
        }
        return "\x1b[27;" + num(fin + 1) + ";" + num(code) + "~";
    }

    std::string c0_key(Key key) const {
        switch (key) {
        case Key::Enter: {
            std::string s = modify_other(Kind::Return, 0);
            return s.empty() ? with_alt('\r') : s;
        }
        case Key::Escape: {
            std::string s = modify_other(Kind::Escape, 0);
            return s.empty() ? with_alt(0x1b) : s;
        }
        case Key::Backspace: {
            std::string s = modify_other(Kind::Backspace, 0);
            if (!s.empty()) return s;
            bool bs = m.backarrow_sends_bs != ctrl();
            return with_alt(bs ? 0x08 : 0x7f);
        }
        case Key::Tab: {
            if (shift()) {
                unsigned st = xstate();
                if (m.modify_other_keys >= 2 && (st & ~XS) != 0) {
                    return "\x1b[27;" + num(st + 1) + ";9~";
                }
                return alt_esc() ? std::string("\x1b\x1b[Z") : std::string("\x1b[Z");
            }
            std::string s = modify_other(Kind::Tab, 0);
            return s.empty() ? with_alt('\t') : s;
        }
        default: return {};
        }
    }

    std::string text_key(char32_t code, char32_t shifted, char32_t base) const {
        if (code >= 0x80 && (k.mods & (Mod_Ctrl | Mod_Alt | Mod_Meta)) && base && is_legacy_ascii_key(base)) {
            code = base;
            shifted = us_shifted(base);
        }
        char32_t ks = code;
        bool letter = code >= 'a' && code <= 'z';
        if (letter) {
            if (shift() != ((k.mods & Mod_CapsLock) != 0) && shifted) ks = shifted;
        } else if (shift() && shifted) {
            ks = shifted;
        }
        std::string s = modify_other(Kind::Text, ks);
        if (!s.empty()) return s;
        if (ctrl()) return with_alt(ctrl_map(ks));
        if (!(k.mods & (Mod_Alt | Mod_Meta))) return k.text.empty() ? utf8(ks) : k.text;
        return with_alt(ks);
    }

    std::string keypad(Key key) const {
        if (key == Key::KpBegin) return csi_letter(fparam(), 'E', m.app_cursor_keys);
        if (Key nk = keypad_to_normal(key); nk != Key::None && nk != Key::Enter) return functional(nk);
        if (m.app_keypad && !(k.mods & Mod_NumLock)) {
            char f = 0;
            uint32_t v = uint32_t(key);
            if (v >= uint32_t(Key::Kp0) && v <= uint32_t(Key::Kp9)) {
                f = char('p' + (v - uint32_t(Key::Kp0)));
            } else {
                switch (key) {
                case Key::KpDecimal: f = 'n'; break;
                case Key::KpDivide: f = 'o'; break;
                case Key::KpMultiply: f = 'j'; break;
                case Key::KpSubtract: f = 'm'; break;
                case Key::KpAdd: f = 'k'; break;
                case Key::KpEnter: f = 'M'; break;
                case Key::KpEqual: f = 'X'; break;
                case Key::KpSeparator: f = 'l'; break;
                default: return {};
                }
            }
            unsigned p = fparam();
            return p ? "\x1bO" + num(p) + f : std::string("\x1bO") + f;
        }
        // xterm writes kypd_num[] directly: no ctrl mapping, no Alt prefix.
        if (key == Key::KpEnter) return "\r";
        char32_t c = keypad_char(key);
        return c ? utf8(c) : std::string();
    }

    std::string functional(Key key) const {
        const unsigned p = fparam();
        const bool ckm = m.app_cursor_keys;
        switch (key) {
        case Key::Up: return csi_letter(p, 'A', ckm);
        case Key::Down: return csi_letter(p, 'B', ckm);
        case Key::Right: return csi_letter(p, 'C', ckm);
        case Key::Left: return csi_letter(p, 'D', ckm);
        case Key::Home: return csi_letter(p, 'H', ckm);
        case Key::End: return csi_letter(p, 'F', ckm);
        case Key::F1: return csi_letter(p, 'P', true);
        case Key::F2: return csi_letter(p, 'Q', true);
        case Key::F3: return csi_letter(p, 'R', true);
        case Key::F4: return csi_letter(p, 'S', true);
        case Key::Insert: return csi_tilde(2, p);
        case Key::Delete: return csi_tilde(3, p);
        case Key::PageUp: return csi_tilde(5, p);
        case Key::PageDown: return csi_tilde(6, p);
        case Key::F5: return csi_tilde(15, p);
        case Key::F6: return csi_tilde(17, p);
        case Key::F7: return csi_tilde(18, p);
        case Key::F8: return csi_tilde(19, p);
        case Key::F9: return csi_tilde(20, p);
        case Key::F10: return csi_tilde(21, p);
        case Key::F11: return csi_tilde(23, p);
        case Key::F12: return csi_tilde(24, p);
        case Key::Menu: return csi_tilde(29, p);
        case Key::Enter:
        case Key::Escape:
        case Key::Backspace:
        case Key::Tab: return c0_key(key);
        default: break;
        }
        if (is_modifier_key(key)) return {};
        if (is_keypad(key)) return keypad(key);
        // No legacy encoding: kitty's CSI u form, lock modifiers dropped.
        unsigned km = k.mods & ~unsigned(Mod_LockMask);
        std::string out = "\x1b[" + num(uint32_t(key));
        if (km) {
            out += ';';
            out += num(km + 1);
        }
        out += 'u';
        return out;
    }
};

} // namespace

std::string encode_legacy(const NormalizedKey& k, const KeyboardModes& m) {
    if (k.action == KeyAction::Release) return {};
    Legacy l{k, m};
    if (k.key != Key::None) return l.functional(k.key);
    if (k.code != 0) return l.text_key(k.code, k.shifted, k.base);
    return k.text;  // pure text event
}

} // namespace bropty::input_detail
