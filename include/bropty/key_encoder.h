#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace bropty {

enum class Key : uint16_t {
    Unknown = 0,
    Backspace,
    Tab,
    Enter,
    Escape,
    Space,
    Delete,
    Insert,
    Home,
    End,
    PageUp,
    PageDown,
    Up,
    Down,
    Left,
    Right,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    Char
};

enum KeyModifiers : uint8_t {
    Mod_None  = 0,
    Mod_Shift = 1 << 0,
    Mod_Alt   = 1 << 1,
    Mod_Ctrl  = 1 << 2,
    Mod_Super = 1 << 3
};

enum class KeyEventType : uint8_t {
    Press = 1,
    Repeat = 2,
    Release = 3
};

enum class MouseButton : uint8_t {
    Left = 0,
    Middle = 1,
    Right = 2,
    None = 3,
    WheelUp = 64,
    WheelDown = 65
};

enum class MouseAction : uint8_t {
    Press = 0,
    Drag = 1,
    Move = 2,
    Release = 3
};

class KeyEncoder {
public:
    KeyEncoder() = default;

    // Encodes a key event to standard VT escape sequence or Kitty sequence
    static std::string encode_key(Key key,
                                  uint32_t codepoint,
                                  uint8_t modifiers,
                                  bool application_cursor_keys = false,
                                  bool kitty_protocol = false,
                                  KeyEventType event_type = KeyEventType::Press);

    // Encodes mouse event into SGR 1006 format
    // col and row are 0-indexed terminal grid coordinates
    static std::string encode_mouse_sgr(MouseButton button,
                                        MouseAction action,
                                        uint8_t modifiers,
                                        int col,
                                        int row);

    // Wraps text in bracketed paste escape sequences if enabled
    static std::string encode_paste(std::string_view text, bool bracketed);

private:
    static std::string encode_kitty(Key key, uint32_t codepoint, uint8_t modifiers, KeyEventType event_type);
    static std::string encode_standard(Key key, uint32_t codepoint, uint8_t modifiers, bool application_cursor_keys);
};

} // namespace bropty
