// Characterization of the current key / mouse / paste encoder. The kitty
// keyboard protocol rework will replace the kitty expectations here (see
// probe_keys.cpp for the spec-derived ones).
#include "bropty/key_encoder.h"
#include "check.h"

using namespace bropty;

int main() {
    init_test();

    CHECK_EQ(KeyEncoder::encode_key(Key::Enter, 0, Mod_None), std::string("\r"));
    CHECK_EQ(KeyEncoder::encode_key(Key::Tab, 0, Mod_None), std::string("\t"));
    CHECK_EQ(KeyEncoder::encode_key(Key::Tab, 0, Mod_Shift), std::string("\x1b[Z"));
    CHECK_EQ(KeyEncoder::encode_key(Key::Backspace, 0, Mod_None), std::string("\x7f"));
    CHECK_EQ(KeyEncoder::encode_key(Key::Escape, 0, Mod_None), std::string("\x1b"));

    CHECK_EQ(KeyEncoder::encode_key(Key::Up, 0, Mod_None, false), std::string("\x1b[A"));
    CHECK_EQ(KeyEncoder::encode_key(Key::Up, 0, Mod_None, true), std::string("\x1bOA"));
    CHECK_EQ(KeyEncoder::encode_key(Key::Up, 0, Mod_Shift), std::string("\x1b[1;2A"));
    CHECK_EQ(KeyEncoder::encode_key(Key::Up, 0, Mod_Ctrl), std::string("\x1b[1;5A"));

    CHECK_EQ(KeyEncoder::encode_key(Key::Char, 'c', Mod_Ctrl), std::string("\x03"));
    CHECK_EQ(KeyEncoder::encode_key(Key::Char, 'C', Mod_Ctrl), std::string("\x03"));

    CHECK_EQ(KeyEncoder::encode_key(Key::Char, 'a', Mod_None, false, true), std::string("\x1b[97u"));
    CHECK_EQ(KeyEncoder::encode_key(Key::Char, 'a', Mod_Shift, false, true), std::string("\x1b[97;2u"));
    CHECK_EQ(KeyEncoder::encode_key(Key::Up, 0, Mod_None, false, true, KeyEventType::Release),
             std::string("\x1b[57362;1:3u"));

    CHECK_EQ(KeyEncoder::encode_mouse_sgr(MouseButton::Left, MouseAction::Press, Mod_None, 10, 5),
             std::string("\x1b[<0;11;6M"));
    CHECK_EQ(KeyEncoder::encode_mouse_sgr(MouseButton::Left, MouseAction::Release, Mod_None, 10, 5),
             std::string("\x1b[<0;11;6m"));
    CHECK_EQ(KeyEncoder::encode_mouse_sgr(MouseButton::Left, MouseAction::Drag, Mod_Shift, 20, 15),
             std::string("\x1b[<36;21;16M"));
    CHECK_EQ(KeyEncoder::encode_mouse_sgr(MouseButton::WheelUp, MouseAction::Press, Mod_None, 0, 0),
             std::string("\x1b[<64;1;1M"));

    CHECK_EQ(KeyEncoder::encode_paste("hello", false), std::string("hello"));
    CHECK_EQ(KeyEncoder::encode_paste("hello", true), std::string("\x1b[200~hello\x1b[201~"));

    return check::finish("test_key_encoder");
}
