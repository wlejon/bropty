// Session-level input: modes set by the application through feed() drive the
// encoding of send_key / send_text / paste / send_mouse / focus; the bytes are
// captured with set_output_callback (no PTY attached).
#include "bropty/session.h"
#include "check.h"

#include <string>

using namespace bropty;

namespace {

struct Harness {
    Session s;
    std::string out;
    Harness() { s.set_output_callback([this](std::string_view b) { out.append(b); }); }
    std::string take() {
        std::string r = out;
        out.clear();
        return r;
    }
    std::string key(const KeyEvent& e) {
        s.send_key(e);
        return take();
    }
    std::string mouse(const MouseEvent& e) {
        s.send_mouse(e);
        return take();
    }
};

KeyEvent fk(Key k, KeyMods m = Mod_None) { return KeyEvent::functional(k, m); }
KeyEvent ch(char32_t c, KeyMods m = Mod_None) { return KeyEvent::character(c, m); }
MouseEvent mev(MouseAction a, MouseButton b, int col, int row) {
    MouseEvent e;
    e.action = a;
    e.button = b;
    e.col = col;
    e.row = row;
    return e;
}

void keyboard_modes() {
    Harness h;
    CHECK_EQ(h.key(fk(Key::Up)), std::string("\x1b[A"));
    h.s.feed("\x1b[?1h");  // DECCKM
    CHECK_EQ(h.key(fk(Key::Up)), std::string("\x1bOA"));
    h.s.feed("\x1b[>1u");  // kitty: disambiguate
    CHECK_EQ(h.key(fk(Key::Up)), std::string("\x1b[A"));
    CHECK_EQ(h.key(fk(Key::Escape)), std::string("\x1b[27u"));
    CHECK_EQ(h.key(ch('c', Mod_Ctrl)), std::string("\x1b[99;5u"));
    h.s.feed("\x1b[=11;2u");  // add 2 and 8: report events, all keys
    CHECK_EQ(h.key(ch('a')), std::string("\x1b[97u"));
    KeyEvent rel = ch('a');
    rel.action = KeyAction::Release;
    CHECK_EQ(h.key(rel), std::string("\x1b[97;1:3u"));
    h.s.feed("\x1b[?u");
    CHECK_EQ(h.take(), std::string("\x1b[?11u"));
    h.s.feed("\x1b[<u");  // pop: back to legacy
    CHECK_EQ(h.key(fk(Key::Escape)), std::string("\x1b"));
    CHECK_EQ(h.key(ch('c', Mod_Ctrl)), std::string("\x03"));

    // Separate stacks for the main and alternate screens.
    h.s.feed("\x1b[>1u");
    h.s.feed("\x1b[?1049h");
    CHECK_EQ(h.key(fk(Key::Escape)), std::string("\x1b"));
    h.s.feed("\x1b[>8u");
    CHECK_EQ(h.key(ch('x')), std::string("\x1b[120u"));
    h.s.feed("\x1b[?1049l");
    CHECK_EQ(h.key(fk(Key::Escape)), std::string("\x1b[27u"));
    CHECK_EQ(h.key(ch('x')), std::string("x"));
    h.s.feed("\x1b[<u");

    // DECKPAM / DECKPNM, DECBKM, ?1036 / ?1039.
    h.s.feed("\x1b=");
    CHECK_EQ(h.key(fk(Key::Kp5)), std::string("\x1bOu"));
    h.s.feed("\x1b>");
    CHECK_EQ(h.key(fk(Key::Kp5)), std::string("5"));
    h.s.feed("\x1b[?66h");
    CHECK_EQ(h.key(fk(Key::KpEnter)), std::string("\x1bOM"));
    h.s.feed("\x1b[?66l");
    h.s.feed("\x1b[?67h");
    CHECK_EQ(h.key(fk(Key::Backspace)), std::string("\x08"));
    h.s.feed("\x1b[?67l");
    CHECK_EQ(h.key(fk(Key::Backspace)), std::string("\x7f"));
    h.s.feed("\x1b[?1036l\x1b[?1039l");
    CHECK_EQ(h.key(ch('a', Mod_Alt)), std::string("\xc3\xa1"));
    h.s.feed("\x1b[?1039h");
    CHECK_EQ(h.key(ch('a', Mod_Alt)), std::string("\x1b" "a"));

    // Nothing to send -> false, nothing written.
    KeyEvent shift = fk(Key::LeftShift, Mod_Shift);
    CHECK(!h.s.send_key(shift));
    CHECK_EQ(h.take(), std::string());
    CHECK(h.s.send_key(ch('q')));
    CHECK_EQ(h.take(), std::string("q"));
}

void modify_other_keys() {
    Harness h;
    CHECK_EQ(h.s.terminal().modes().modify_other_keys, 0);
    h.s.feed("\x1b[>4;2m");
    CHECK_EQ(h.s.terminal().modes().modify_other_keys, 2);
    CHECK_EQ(h.key(ch('a', Mod_Ctrl)), std::string("\x1b[27;5;97~"));
    h.s.feed("\x1b[?4m");  // XTQMODKEYS
    CHECK_EQ(h.take(), std::string("\x1b[>4;2m"));
    h.s.feed("\x1b[>4;1m");
    CHECK_EQ(h.key(ch('a', Mod_Ctrl)), std::string("\x01"));
    CHECK_EQ(h.key(ch(',', Mod_Ctrl)), std::string("\x1b[27;5;44~"));
    h.s.feed("\x1b[?4m");
    CHECK_EQ(h.take(), std::string("\x1b[>4;1m"));
    h.s.feed("\x1b[>4m");  // omitted value: reset to the initial 0
    CHECK_EQ(h.s.terminal().modes().modify_other_keys, 0);
    CHECK_EQ(h.key(ch(',', Mod_Ctrl)), std::string(","));
    h.s.feed("\x1b[>4;2m\x1b[>4;0m");
    CHECK_EQ(h.s.terminal().modes().modify_other_keys, 0);
    h.s.feed("\x1b[>4;2m\x1b[>m");  // no parameters: reset everything
    CHECK_EQ(h.s.terminal().modes().modify_other_keys, 0);
    h.s.feed("\x1b[>4;2m\x1b[>4n");  // XTMODKEYS disable
    CHECK_EQ(h.s.terminal().modes().modify_other_keys, 0);
    h.s.feed("\x1b[>4;2m\x1b[>1;2m");  // another resource: no effect on 4
    CHECK_EQ(h.s.terminal().modes().modify_other_keys, 2);
    h.s.feed("\x1b[!p");  // DECSTR resets it (xterm ReallyReset)
    CHECK_EQ(h.s.terminal().modes().modify_other_keys, 0);
    h.s.feed("\x1b[>4;2m\x1b" "c");  // RIS
    CHECK_EQ(h.s.terminal().modes().modify_other_keys, 0);
    h.s.feed("\x1b[?4m");
    CHECK_EQ(h.take(), std::string("\x1b[>4;0m"));
    // Plain SGR is unaffected by the new dispatch.
    h.s.feed("\x1b[4mX");
    CHECK(h.s.terminal().style(h.s.terminal().row(0).cells[0].style).underline != Underline::None);
}

void mouse() {
    Harness h;
    CHECK(!h.s.send_mouse(mev(MouseAction::Press, MouseButton::Left, 0, 0)));
    CHECK_EQ(h.take(), std::string());
    h.s.feed("\x1b[?1000h");
    CHECK(h.s.send_mouse(mev(MouseAction::Press, MouseButton::Left, 0, 0)));
    CHECK_EQ(h.take(), std::string("\x1b[M !!"));
    CHECK_EQ(h.mouse(mev(MouseAction::Motion, MouseButton::None, 3, 3)), std::string());
    CHECK_EQ(h.mouse(mev(MouseAction::Release, MouseButton::Left, 3, 3)), std::string("\x1b[M#$$"));
    h.s.feed("\x1b[?1006h");
    CHECK_EQ(h.mouse(mev(MouseAction::Press, MouseButton::Left, 2, 3)), std::string("\x1b[<0;3;4M"));
    h.s.feed("\x1b[?1002h");
    CHECK_EQ(h.mouse(mev(MouseAction::Motion, MouseButton::None, 2, 3)), std::string());  // same cell
    CHECK_EQ(h.mouse(mev(MouseAction::Motion, MouseButton::None, 5, 3)), std::string("\x1b[<32;6;4M"));
    CHECK_EQ(h.mouse(mev(MouseAction::Release, MouseButton::Left, 5, 3)), std::string("\x1b[<0;6;4m"));
    CHECK_EQ(h.mouse(mev(MouseAction::Motion, MouseButton::None, 6, 3)), std::string());
    h.s.feed("\x1b[?1003h");
    CHECK_EQ(h.mouse(mev(MouseAction::Motion, MouseButton::None, 7, 3)), std::string("\x1b[<35;8;4M"));
    h.s.feed("\x1b[?1016h");
    MouseEvent p = mev(MouseAction::Press, MouseButton::Right, 1, 1);
    p.x = 15;
    p.y = 30;
    CHECK_EQ(h.mouse(p), std::string("\x1b[<2;16;31M"));
    h.s.feed("\x1b[?1015h");
    CHECK_EQ(h.mouse(mev(MouseAction::Press, MouseButton::Left, 0, 0)), std::string("\x1b[32;1;1M"));
    h.s.feed("\x1b[?1005h");
    CHECK_EQ(h.mouse(mev(MouseAction::Press, MouseButton::Left, 95, 0)), std::string("\x1b[M \xc2\x80!"));
    h.s.feed("\x1b[?1005l\x1b[?1003l");
    CHECK_EQ(h.mouse(mev(MouseAction::Motion, MouseButton::None, 9, 9)), std::string());
    h.s.feed("\x1b[?9h");
    CHECK_EQ(h.mouse(mev(MouseAction::Press, MouseButton::Left, 0, 0)), std::string("\x1b[M !!"));
    CHECK_EQ(h.mouse(mev(MouseAction::Release, MouseButton::Left, 0, 0)), std::string());
    h.s.feed("\x1b[?9l");
    CHECK_EQ(h.mouse(mev(MouseAction::Press, MouseButton::Left, 0, 0)), std::string());
}

void alternate_scroll() {
    Harness h;
    MouseEvent up = mev(MouseAction::Press, MouseButton::WheelUp, 0, 0);
    MouseEvent down = mev(MouseAction::Press, MouseButton::WheelDown, 0, 0);
    h.s.feed("\x1b[?1007h");
    CHECK(!h.s.send_mouse(up));  // primary screen: nothing
    CHECK_EQ(h.take(), std::string());
    h.s.feed("\x1b[?1049h");
    CHECK(h.s.send_mouse(up));
    CHECK_EQ(h.take(), std::string("\x1b[A"));
    CHECK_EQ(h.mouse(down), std::string("\x1b[B"));
    h.s.feed("\x1b[?1h");
    CHECK_EQ(h.mouse(up), std::string("\x1bOA"));
    CHECK_EQ(h.mouse(mev(MouseAction::Release, MouseButton::WheelUp, 0, 0)), std::string());
    CHECK_EQ(h.mouse(mev(MouseAction::Press, MouseButton::Left, 0, 0)), std::string());
    h.s.feed("\x1b[?1000h");  // tracking wins
    CHECK_EQ(h.mouse(up), std::string("\x1b[M`!!"));
    h.s.feed("\x1b[?1000l\x1b[?1007l");
    CHECK_EQ(h.mouse(up), std::string());
}

void paste_text_focus() {
    Harness h;
    CHECK(h.s.paste("a\nb"));
    CHECK_EQ(h.take(), std::string("a\rb"));
    CHECK(!h.s.paste(""));
    h.s.feed("\x1b[?2004h");
    h.s.paste("hi\x1b[201~evil");
    std::string out = h.take();
    CHECK_EQ(out, std::string("\x1b[200~hi[201~evil\x1b[201~"));
    CHECK(out.find("\x1b[201~evil") == std::string::npos);
    h.s.feed("\x1b[?2004l");
    h.s.paste("hi\x1b[201~evil");
    CHECK_EQ(h.take(), std::string("hi\x1b[201~evil"));

    CHECK(h.s.send_text("h\xc3\xa9"));
    CHECK_EQ(h.take(), std::string("h\xc3\xa9"));
    CHECK(!h.s.send_text("\x03"));
    h.s.feed("\x1b[>24u");  // kitty 8 + 16
    CHECK(h.s.send_text("h\xc3\xa9"));
    CHECK_EQ(h.take(), std::string("\x1b[0;;104:233u"));
    h.s.feed("\x1b[<u");

    CHECK(!h.s.focus(true));
    CHECK_EQ(h.take(), std::string());
    h.s.feed("\x1b[?1004h");
    CHECK(h.s.focus(true));
    CHECK_EQ(h.take(), std::string("\x1b[I"));
    CHECK(h.s.focus(false));
    CHECK_EQ(h.take(), std::string("\x1b[O"));
    h.s.feed("\x1b[?1004l");
    CHECK(!h.s.focus(false));
}

void probe_cases() {
    // The Session checks from the former probe_keys.cpp.
    Harness h;
    h.s.feed("\x1b[>1u");
    h.s.send_key(fk(Key::Escape));
    CHECK_EQ(h.take(), std::string("\x1b[27u"));
    h.s.feed("\x1b[<u\x1b[?1000h");
    h.s.send_mouse(mev(MouseAction::Press, MouseButton::Left, 0, 0));
    CHECK_EQ(h.take(), std::string("\x1b[M") + char(32) + char(33) + char(33));
    h.s.feed("\x1b[?1006h");
    h.s.send_mouse(mev(MouseAction::Press, MouseButton::Left, 2, 3));
    CHECK_EQ(h.take(), std::string("\x1b[<0;3;4M"));
    h.s.feed("\x1b[?2004h");
    h.s.paste("hi\x1b[201~evil");
    CHECK(h.take().find("\x1b[201~evil") == std::string::npos);
}

} // namespace

int main() {
    init_test();
    keyboard_modes();
    modify_other_keys();
    mouse();
    alternate_scroll();
    paste_text_focus();
    probe_cases();
    return check::finish("test_input_session");
}
