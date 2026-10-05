// Mouse reports in every tracking mode and encoding (xterm ctlseqs "Mouse
// Tracking" and button.c), plus MouseReporter's held-button tracking and
// same-cell motion suppression.
#include "bropty/input.h"
#include "check.h"

#include <string>

using namespace bropty;

namespace {

MouseModes mm(MouseTracking t, MouseEncoding e = MouseEncoding::Default) {
    MouseModes m;
    m.tracking = t;
    m.encoding = e;
    return m;
}
MouseEvent ev(MouseAction a, MouseButton b, int col, int row, KeyMods mods = Mod_None) {
    MouseEvent e;
    e.action = a;
    e.button = b;
    e.col = col;
    e.row = row;
    e.mods = mods;
    return e;
}
MouseEvent press(MouseButton b, int col = 0, int row = 0, KeyMods mods = Mod_None) {
    return ev(MouseAction::Press, b, col, row, mods);
}
MouseEvent release(MouseButton b, int col = 0, int row = 0, KeyMods mods = Mod_None) {
    return ev(MouseAction::Release, b, col, row, mods);
}
MouseEvent motion(int col, int row, KeyMods mods = Mod_None) {
    return ev(MouseAction::Motion, MouseButton::None, col, row, mods);
}
// Default-encoding report: CSI M then three bytes, each value + 32.
std::string x11(int cb, int x, int y) {
    std::string s = "\x1b[M";
    s.push_back(char(cb + 32));
    s.push_back(char(x + 32));
    s.push_back(char(y + 32));
    return s;
}

void tracking_modes() {
    const MouseModes x10 = mm(MouseTracking::X10);
    CHECK_EQ(encode_mouse(press(MouseButton::Left, 0, 0), x10), x11(0, 1, 1));
    CHECK_EQ(encode_mouse(press(MouseButton::Right, 4, 2), x10), x11(2, 5, 3));
    CHECK_EQ(encode_mouse(press(MouseButton::Left, 0, 0, Mod_Ctrl | Mod_Shift), x10), x11(0, 1, 1));  // no modifiers
    CHECK_EQ(encode_mouse(release(MouseButton::Left), x10), std::string());
    CHECK_EQ(encode_mouse(press(MouseButton::WheelUp), x10), std::string());
    CHECK_EQ(encode_mouse(press(MouseButton::Button8), x10), std::string());
    CHECK_EQ(encode_mouse(motion(3, 3), x10, MouseButton::Left), std::string());

    const MouseModes normal = mm(MouseTracking::Normal);
    CHECK_EQ(encode_mouse(press(MouseButton::Left), normal), x11(0, 1, 1));
    CHECK_EQ(encode_mouse(press(MouseButton::Middle), normal), x11(1, 1, 1));
    CHECK_EQ(encode_mouse(press(MouseButton::Right), normal), x11(2, 1, 1));
    CHECK_EQ(encode_mouse(release(MouseButton::Right), normal), x11(3, 1, 1));
    CHECK_EQ(encode_mouse(release(MouseButton::Left, 0, 0, Mod_Shift), normal), x11(3 + 4, 1, 1));
    CHECK_EQ(encode_mouse(press(MouseButton::WheelUp), normal), x11(64, 1, 1));
    CHECK_EQ(encode_mouse(press(MouseButton::WheelDown), normal), x11(65, 1, 1));
    CHECK_EQ(encode_mouse(press(MouseButton::WheelLeft), normal), x11(66, 1, 1));
    CHECK_EQ(encode_mouse(press(MouseButton::WheelRight), normal), x11(67, 1, 1));
    CHECK_EQ(encode_mouse(release(MouseButton::WheelUp), normal), std::string());
    CHECK_EQ(encode_mouse(press(MouseButton::Button8), normal), x11(128, 1, 1));
    CHECK_EQ(encode_mouse(press(MouseButton::Button11), normal), x11(131, 1, 1));
    CHECK_EQ(encode_mouse(release(MouseButton::Button9), normal), x11(3, 1, 1));
    CHECK_EQ(encode_mouse(press(MouseButton::Left, 0, 0, Mod_Shift), normal), x11(4, 1, 1));
    CHECK_EQ(encode_mouse(press(MouseButton::Left, 0, 0, Mod_Alt), normal), x11(8, 1, 1));
    CHECK_EQ(encode_mouse(press(MouseButton::Left, 0, 0, Mod_Meta), normal), x11(8, 1, 1));
    CHECK_EQ(encode_mouse(press(MouseButton::Left, 0, 0, Mod_Ctrl), normal), x11(16, 1, 1));
    CHECK_EQ(encode_mouse(press(MouseButton::WheelDown, 0, 0, Mod_Ctrl | Mod_Shift | Mod_Alt), normal),
             x11(65 + 28, 1, 1));
    CHECK_EQ(encode_mouse(press(MouseButton::Left, 0, 0, Mod_Super), normal), x11(0, 1, 1));
    CHECK_EQ(encode_mouse(motion(1, 1), normal, MouseButton::Left), std::string());
    CHECK_EQ(encode_mouse(press(MouseButton::None), normal), std::string());

    const MouseModes button = mm(MouseTracking::Button);
    CHECK_EQ(encode_mouse(motion(2, 3), button, MouseButton::Left), x11(32, 3, 4));
    CHECK_EQ(encode_mouse(motion(2, 3), button, MouseButton::Right), x11(34, 3, 4));
    CHECK_EQ(encode_mouse(motion(2, 3, Mod_Ctrl), button, MouseButton::Middle), x11(33 + 16, 3, 4));
    CHECK_EQ(encode_mouse(motion(2, 3), button, MouseButton::Button8), x11(32 + 128, 3, 4));
    CHECK_EQ(encode_mouse(motion(2, 3), button), std::string());
    CHECK_EQ(encode_mouse(press(MouseButton::Left), button), x11(0, 1, 1));

    const MouseModes any = mm(MouseTracking::Any);
    CHECK_EQ(encode_mouse(motion(2, 3), any), x11(35, 3, 4));
    CHECK_EQ(encode_mouse(motion(2, 3), any, MouseButton::Left), x11(32, 3, 4));
    CHECK_EQ(encode_mouse(motion(2, 3, Mod_Shift), any), x11(35 + 4, 3, 4));

    CHECK_EQ(encode_mouse(press(MouseButton::Left), mm(MouseTracking::None)), std::string());
}

void encodings() {
    // Default: one byte per value, positions up to 223; beyond that the event is dropped.
    const MouseModes def = mm(MouseTracking::Normal);
    CHECK_EQ(encode_mouse(press(MouseButton::Left, 222, 222), def), x11(0, 223, 223));
    CHECK_EQ(encode_mouse(press(MouseButton::Left, 223, 0), def), std::string());
    CHECK_EQ(encode_mouse(press(MouseButton::Left, 0, 223), def), std::string());
    CHECK_EQ(encode_mouse(press(MouseButton::Left, -5, -1), def), x11(0, 1, 1));

    // UTF-8 (?1005): values >= 128 as two-byte UTF-8, positions up to 2015.
    const MouseModes u8 = mm(MouseTracking::Normal, MouseEncoding::Utf8);
    CHECK_EQ(encode_mouse(press(MouseButton::Left, 0, 0), u8), x11(0, 1, 1));
    CHECK_EQ(encode_mouse(press(MouseButton::Left, 94, 0), u8), std::string("\x1b[M \x7f!"));
    CHECK_EQ(encode_mouse(press(MouseButton::Left, 95, 0), u8), std::string("\x1b[M \xc2\x80!"));
    CHECK_EQ(encode_mouse(press(MouseButton::Left, 300, 1), u8), std::string("\x1b[M \xc5\x8d\""));
    CHECK_EQ(encode_mouse(press(MouseButton::Left, 2014, 2014), u8), std::string("\x1b[M \xdf\xbf\xdf\xbf"));
    CHECK_EQ(encode_mouse(press(MouseButton::Left, 2015, 0), u8), std::string());
    CHECK_EQ(encode_mouse(press(MouseButton::Button8, 0, 0), u8), std::string("\x1b[M\xc2\xa0!!"));
    CHECK_EQ(encode_mouse(release(MouseButton::Left, 0, 0), u8), x11(3, 1, 1));

    // SGR (?1006): decimal, release keeps the button and ends in 'm'.
    const MouseModes sgr = mm(MouseTracking::Any, MouseEncoding::Sgr);
    CHECK_EQ(encode_mouse(press(MouseButton::Left, 0, 0), sgr), std::string("\x1b[<0;1;1M"));
    CHECK_EQ(encode_mouse(release(MouseButton::Left, 4, 2), sgr), std::string("\x1b[<0;5;3m"));
    CHECK_EQ(encode_mouse(release(MouseButton::Right, 4, 2, Mod_Ctrl), sgr), std::string("\x1b[<18;5;3m"));
    CHECK_EQ(encode_mouse(motion(0, 0), sgr, MouseButton::Right), std::string("\x1b[<34;1;1M"));
    CHECK_EQ(encode_mouse(motion(0, 0), sgr), std::string("\x1b[<35;1;1M"));
    CHECK_EQ(encode_mouse(press(MouseButton::WheelDown, 9, 9, Mod_Ctrl), sgr), std::string("\x1b[<81;10;10M"));
    CHECK_EQ(encode_mouse(press(MouseButton::WheelRight, 0, 0), sgr), std::string("\x1b[<67;1;1M"));
    CHECK_EQ(encode_mouse(press(MouseButton::Button10, 0, 0), sgr), std::string("\x1b[<130;1;1M"));
    CHECK_EQ(encode_mouse(release(MouseButton::Button10, 0, 0), sgr), std::string("\x1b[<130;1;1m"));
    CHECK_EQ(encode_mouse(press(MouseButton::Left, 300, 0), sgr), std::string("\x1b[<0;301;1M"));
    CHECK_EQ(encode_mouse(press(MouseButton::Left, 1233, 5677), sgr), std::string("\x1b[<0;1234;5678M"));
    CHECK_EQ(encode_mouse(press(MouseButton::Left, 0, 0, Mod_Shift), sgr), std::string("\x1b[<4;1;1M"));
    CHECK_EQ(encode_mouse(press(MouseButton::Left, 0, 0, Mod_Alt), sgr), std::string("\x1b[<8;1;1M"));
    CHECK_EQ(encode_mouse(press(MouseButton::Left, 0, 0, Mod_Ctrl), sgr), std::string("\x1b[<16;1;1M"));
    CHECK_EQ(encode_mouse(press(MouseButton::Right, 0, 0), mm(MouseTracking::X10, MouseEncoding::Sgr)),
             std::string("\x1b[<2;1;1M"));

    // SGR-pixels (?1016): 1-based pixel coordinates.
    const MouseModes px = mm(MouseTracking::Normal, MouseEncoding::SgrPixels);
    MouseEvent e = press(MouseButton::Left, 3, 1);
    e.x = 37;
    e.y = 21;
    CHECK_EQ(encode_mouse(e, px), std::string("\x1b[<0;38;22M"));
    e.action = MouseAction::Release;
    CHECK_EQ(encode_mouse(e, px), std::string("\x1b[<0;38;22m"));

    // urxvt (?1015): CSI 32+b ; x ; y M, release as button 3.
    const MouseModes ux = mm(MouseTracking::Any, MouseEncoding::Urxvt);
    CHECK_EQ(encode_mouse(press(MouseButton::Left, 0, 0), ux), std::string("\x1b[32;1;1M"));
    CHECK_EQ(encode_mouse(release(MouseButton::Left, 4, 2), ux), std::string("\x1b[35;5;3M"));
    CHECK_EQ(encode_mouse(press(MouseButton::WheelUp, 299, 0), ux), std::string("\x1b[96;300;1M"));
    CHECK_EQ(encode_mouse(motion(1, 1), ux), std::string("\x1b[67;2;2M"));
}

void reporter() {
    MouseReporter r;
    const MouseModes btn = mm(MouseTracking::Button, MouseEncoding::Sgr);
    CHECK_EQ(r.encode(motion(1, 1), btn), std::string());  // no button held
    CHECK_EQ(r.encode(press(MouseButton::Left, 1, 1), btn), std::string("\x1b[<0;2;2M"));
    CHECK(r.held_button() == MouseButton::Left);
    CHECK_EQ(r.encode(motion(1, 1), btn), std::string());  // same cell as the press
    CHECK_EQ(r.encode(motion(2, 1), btn), std::string("\x1b[<32;3;2M"));
    CHECK_EQ(r.encode(motion(2, 1), btn), std::string());
    CHECK_EQ(r.encode(motion(3, 1), btn), std::string("\x1b[<32;4;2M"));
    // Lowest held button is the one reported.
    CHECK_EQ(r.encode(press(MouseButton::Right, 3, 1), btn), std::string("\x1b[<2;4;2M"));
    CHECK_EQ(r.encode(motion(4, 1), btn), std::string("\x1b[<32;5;2M"));
    CHECK_EQ(r.encode(release(MouseButton::Left, 4, 1), btn), std::string("\x1b[<0;5;2m"));
    CHECK(r.held_button() == MouseButton::Right);
    CHECK_EQ(r.encode(motion(5, 1), btn), std::string("\x1b[<34;6;2M"));
    CHECK_EQ(r.encode(release(MouseButton::Right, 5, 1), btn), std::string("\x1b[<2;6;2m"));
    CHECK_EQ(r.encode(motion(6, 1), btn), std::string());
    // Wheel does not count as held.
    CHECK_EQ(r.encode(press(MouseButton::WheelUp, 6, 1), btn), std::string("\x1b[<64;7;2M"));
    CHECK(r.held_button() == MouseButton::None);

    // Any-event: every cell change, deduplicated.
    MouseReporter a;
    const MouseModes any = mm(MouseTracking::Any);
    CHECK_EQ(a.encode(motion(0, 0), any), x11(35, 1, 1));
    CHECK_EQ(a.encode(motion(0, 0), any), std::string());
    CHECK_EQ(a.encode(motion(0, 1), any), x11(35, 1, 2));

    // SGR-pixels: deduplicated per pixel, not per cell.
    MouseReporter p;
    const MouseModes px = mm(MouseTracking::Any, MouseEncoding::SgrPixels);
    MouseEvent m = motion(0, 0);
    m.x = 3;
    m.y = 4;
    CHECK_EQ(p.encode(m, px), std::string("\x1b[<35;4;5M"));
    CHECK_EQ(p.encode(m, px), std::string());
    m.x = 4;
    CHECK_EQ(p.encode(m, px), std::string("\x1b[<35;5;5M"));

    // Button state is tracked even while nothing is reported.
    MouseReporter q;
    CHECK_EQ(q.encode(press(MouseButton::Middle), mm(MouseTracking::None)), std::string());
    CHECK_EQ(q.encode(motion(3, 3), btn), std::string("\x1b[<33;4;4M"));
    q.reset();
    CHECK(q.held_button() == MouseButton::None);
}

} // namespace

int main() {
    init_test();
    tracking_modes();
    encodings();
    reporter();
    return check::finish("test_mouse");
}
