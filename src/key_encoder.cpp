#include "bropty/key_encoder.h"
#include <string>

namespace bropty {

namespace {

// Converts UTF-32 codepoint to UTF-8 string
std::string to_utf8(uint32_t cp) {
    std::string out;
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0x10FFFF) {
        out.push_back(static_cast<char>(0xF0 | ((cp >> 18) & 0x07)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    return out;
}

int get_xterm_mod(uint8_t modifiers) {
    int mod = 1;
    if (modifiers & Mod_Shift) mod += 1;
    if (modifiers & Mod_Alt)   mod += 2;
    if (modifiers & Mod_Ctrl)  mod += 4;
    if (modifiers & Mod_Super) mod += 8;
    return mod;
}

uint32_t key_to_kitty_codepoint(Key key, uint32_t codepoint) {
    switch (key) {
        case Key::Escape: return 27;
        case Key::Enter: return 13;
        case Key::Tab: return 9;
        case Key::Backspace: return 127;
        case Key::Insert: return 57358;
        case Key::Delete: return 57359;
        case Key::Left: return 57360;
        case Key::Right: return 57361;
        case Key::Up: return 57362;
        case Key::Down: return 57363;
        case Key::PageUp: return 57364;
        case Key::PageDown: return 57365;
        case Key::Home: return 57366;
        case Key::End: return 57367;
        case Key::F1: return 57376;
        case Key::F2: return 57377;
        case Key::F3: return 57378;
        case Key::F4: return 57379;
        case Key::F5: return 57380;
        case Key::F6: return 57381;
        case Key::F7: return 57382;
        case Key::F8: return 57383;
        case Key::F9: return 57384;
        case Key::F10: return 57385;
        case Key::F11: return 57386;
        case Key::F12: return 57387;
        case Key::Space: return 32;
        default: return codepoint;
    }
}

} // namespace

std::string KeyEncoder::encode_key(Key key,
                                   uint32_t codepoint,
                                   uint8_t modifiers,
                                   bool application_cursor_keys,
                                   bool kitty_protocol,
                                   KeyEventType event_type) {
    if (kitty_protocol) {
        return encode_kitty(key, codepoint, modifiers, event_type);
    }
    if (event_type == KeyEventType::Release) {
        return ""; // Standard VT ignores release events
    }
    return encode_standard(key, codepoint, modifiers, application_cursor_keys);
}

std::string KeyEncoder::encode_kitty(Key key, uint32_t codepoint, uint8_t modifiers, KeyEventType event_type) {
    uint32_t cp = key_to_kitty_codepoint(key, codepoint);
    int mod = get_xterm_mod(modifiers);
    int ev = static_cast<int>(event_type);

    std::string out = "\x1b[";
    out += std::to_string(cp);

    if (mod != 1 || ev != 1) {
        out += ";";
        out += std::to_string(mod);
        if (ev != 1) {
            out += ":";
            out += std::to_string(ev);
        }
    }
    out += "u";
    return out;
}

std::string KeyEncoder::encode_standard(Key key, uint32_t codepoint, uint8_t modifiers, bool application_cursor_keys) {
    int mod = get_xterm_mod(modifiers);

    // Ctrl + A..Z
    if ((modifiers & Mod_Ctrl) && !(modifiers & (Mod_Alt | Mod_Super))) {
        if (codepoint >= 'a' && codepoint <= 'z') {
            char ctrl = static_cast<char>(codepoint - 'a' + 1);
            return std::string(1, ctrl);
        }
        if (codepoint >= 'A' && codepoint <= 'Z') {
            char ctrl = static_cast<char>(codepoint - 'A' + 1);
            return std::string(1, ctrl);
        }
        if (codepoint == ' ') {
            return std::string(1, '\0');
        }
    }

    // Special Keys
    switch (key) {
        case Key::Enter:
            if (modifiers & Mod_Alt) return "\x1b\r";
            return "\r";
        case Key::Tab:
            if (modifiers & Mod_Shift) return "\x1b[Z"; // Backtab CBT
            if (modifiers & Mod_Alt) return "\x1b\t";
            return "\t";
        case Key::Backspace:
            if (modifiers & Mod_Alt) return "\x1b\x7f";
            return "\x7f";
        case Key::Escape:
            if (modifiers & Mod_Alt) return "\x1b\x1b";
            return "\x1b";
        case Key::Space:
            if (modifiers & Mod_Alt) return "\x1b ";
            return " ";

        // Cursor Arrows
        case Key::Up:
        case Key::Down:
        case Key::Right:
        case Key::Left: {
            char dir = 'A';
            if (key == Key::Up) dir = 'A';
            else if (key == Key::Down) dir = 'B';
            else if (key == Key::Right) dir = 'C';
            else if (key == Key::Left) dir = 'D';

            if (mod > 1) {
                return "\x1b[1;" + std::to_string(mod) + dir;
            }
            if (application_cursor_keys) {
                std::string s = "\x1bO";
                s.push_back(dir);
                return s;
            }
            std::string s = "\x1b[";
            s.push_back(dir);
            return s;
        }

        // Home / End
        case Key::Home:
            if (mod > 1) return "\x1b[1;" + std::to_string(mod) + "H";
            return "\x1b[H";
        case Key::End:
            if (mod > 1) return "\x1b[1;" + std::to_string(mod) + "F";
            return "\x1b[F";

        // Navigation
        case Key::PageUp:
            if (mod > 1) return "\x1b[5;" + std::to_string(mod) + "~";
            return "\x1b[5~";
        case Key::PageDown:
            if (mod > 1) return "\x1b[6;" + std::to_string(mod) + "~";
            return "\x1b[6~";
        case Key::Insert:
            if (mod > 1) return "\x1b[2;" + std::to_string(mod) + "~";
            return "\x1b[2~";
        case Key::Delete:
            if (mod > 1) return "\x1b[3;" + std::to_string(mod) + "~";
            return "\x1b[3~";

        // Function Keys F1-F4
        case Key::F1: case Key::F2: case Key::F3: case Key::F4: {
            char f = 'P' + static_cast<int>(key) - static_cast<int>(Key::F1);
            if (mod > 1) return "\x1b[1;" + std::to_string(mod) + f;
            std::string s = "\x1bO";
            s.push_back(f);
            return s;
        }

        // Function Keys F5-F12
        case Key::F5: case Key::F6: case Key::F7: case Key::F8:
        case Key::F9: case Key::F10: case Key::F11: case Key::F12: {
            static const int f_codes[] = {15, 17, 18, 19, 20, 21, 23, 24};
            int code = f_codes[static_cast<int>(key) - static_cast<int>(Key::F5)];
            if (mod > 1) return "\x1b[" + std::to_string(code) + ";" + std::to_string(mod) + "~";
            return "\x1b[" + std::to_string(code) + "~";
        }

        case Key::Char:
        default: {
            std::string utf8 = to_utf8(codepoint);
            if (modifiers & Mod_Alt) {
                return "\x1b" + utf8;
            }
            return utf8;
        }
    }
}

std::string KeyEncoder::encode_mouse_sgr(MouseButton button,
                                         MouseAction action,
                                         uint8_t modifiers,
                                         int col,
                                         int row) {
    int b = 0;
    if (button == MouseButton::WheelUp || button == MouseButton::WheelDown) {
        b = static_cast<int>(button);
    } else {
        b = static_cast<int>(button);
    }

    if (action == MouseAction::Drag || action == MouseAction::Move) {
        b += 32;
    }

    if (modifiers & Mod_Shift) b += 4;
    if (modifiers & Mod_Alt)   b += 8;
    if (modifiers & Mod_Ctrl)  b += 16;

    char trailer = (action == MouseAction::Release) ? 'm' : 'M';

    std::string out = "\x1b[<";
    out += std::to_string(b);
    out += ";";
    out += std::to_string(col + 1); // 1-indexed for terminal
    out += ";";
    out += std::to_string(row + 1); // 1-indexed for terminal
    out.push_back(trailer);
    return out;
}

std::string KeyEncoder::encode_paste(std::string_view text, bool bracketed) {
    if (!bracketed) {
        return std::string(text);
    }
    std::string out = "\x1b[200~";
    out.append(text);
    out.append("\x1b[201~");
    return out;
}

} // namespace bropty
