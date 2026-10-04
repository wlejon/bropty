#include "bropty/key_encoder.h"
#include "test_common.h"
#include <cassert>
#include <iostream>

int main() {
    init_test();
    std::cout << "[test_key_encoder] Starting...\n";

    // 1. Standard key encoding
    assert(bropty::KeyEncoder::encode_key(bropty::Key::Enter, 0, bropty::Mod_None) == "\r");
    assert(bropty::KeyEncoder::encode_key(bropty::Key::Tab, 0, bropty::Mod_None) == "\t");
    assert(bropty::KeyEncoder::encode_key(bropty::Key::Tab, 0, bropty::Mod_Shift) == "\x1b[Z");
    assert(bropty::KeyEncoder::encode_key(bropty::Key::Backspace, 0, bropty::Mod_None) == "\x7f");
    assert(bropty::KeyEncoder::encode_key(bropty::Key::Escape, 0, bropty::Mod_None) == "\x1b");

    // Arrows: normal vs app keys vs modifiers
    assert(bropty::KeyEncoder::encode_key(bropty::Key::Up, 0, bropty::Mod_None, false) == "\x1b[A");
    assert(bropty::KeyEncoder::encode_key(bropty::Key::Up, 0, bropty::Mod_None, true) == "\x1bOA");
    assert(bropty::KeyEncoder::encode_key(bropty::Key::Up, 0, bropty::Mod_Shift) == "\x1b[1;2A");
    assert(bropty::KeyEncoder::encode_key(bropty::Key::Up, 0, bropty::Mod_Ctrl) == "\x1b[1;5A");

    // Ctrl+C
    assert(bropty::KeyEncoder::encode_key(bropty::Key::Char, 'c', bropty::Mod_Ctrl) == "\x03");
    assert(bropty::KeyEncoder::encode_key(bropty::Key::Char, 'C', bropty::Mod_Ctrl) == "\x03");

    // 2. Kitty keyboard protocol encoding
    // Standard letter 'a' with no modifier: \x1b[97u
    std::string kitty_a = bropty::KeyEncoder::encode_key(bropty::Key::Char, 'a', bropty::Mod_None, false, true);
    assert(kitty_a == "\x1b[97u");

    // Shift + 'A': codepoint 97 or 'a', mod = 2 -> \x1b[97;2u
    std::string kitty_shift_a = bropty::KeyEncoder::encode_key(bropty::Key::Char, 'a', bropty::Mod_Shift, false, true);
    assert(kitty_shift_a == "\x1b[97;2u");

    // Arrow Up with Release event (event_type = 3): Kitty codepoint 57362
    std::string kitty_up_rel = bropty::KeyEncoder::encode_key(
        bropty::Key::Up, 0, bropty::Mod_None, false, true, bropty::KeyEventType::Release);
    assert(kitty_up_rel == "\x1b[57362;1:3u");

    // 3. SGR 1006 Mouse encoding
    // Left click at col 10, row 5 (0-indexed -> 11, 6 1-indexed)
    std::string m_press = bropty::KeyEncoder::encode_mouse_sgr(
        bropty::MouseButton::Left, bropty::MouseAction::Press, bropty::Mod_None, 10, 5);
    assert(m_press == "\x1b[<0;11;6M");

    std::string m_release = bropty::KeyEncoder::encode_mouse_sgr(
        bropty::MouseButton::Left, bropty::MouseAction::Release, bropty::Mod_None, 10, 5);
    assert(m_release == "\x1b[<0;11;6m");

    // Drag with Shift (+4 +32 = 36)
    std::string m_drag = bropty::KeyEncoder::encode_mouse_sgr(
        bropty::MouseButton::Left, bropty::MouseAction::Drag, bropty::Mod_Shift, 20, 15);
    assert(m_drag == "\x1b[<36;21;16M");

    // Scroll wheel up (64)
    std::string m_wheel = bropty::KeyEncoder::encode_mouse_sgr(
        bropty::MouseButton::WheelUp, bropty::MouseAction::Press, bropty::Mod_None, 0, 0);
    assert(m_wheel == "\x1b[<64;1;1M");

    // 4. Bracketed paste
    assert(bropty::KeyEncoder::encode_paste("hello", false) == "hello");
    assert(bropty::KeyEncoder::encode_paste("hello", true) == "\x1b[200~hello\x1b[201~");

    std::cout << "[test_key_encoder] PASSED\n";
    return 0;
}
