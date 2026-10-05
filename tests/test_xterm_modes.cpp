// xterm extensions beyond the VT core: VS15 narrowing of emoji, DECCOLM under
// ?40 / DECNCSM, XTSAVE / XTRESTORE, DECCRA carrying grapheme cluster tails,
// and the XTMODKEYS / XTFMTKEYS resources with their effect on the legacy key
// encoder (xterm's modifyCursorKeys / modifyFunctionKeys / modifyKeypadKeys /
// formatOtherKeys).
#include "bropty/input.h"
#include "term_helpers.h"

using namespace bropty;
using th::T;
using th::utf8;

namespace {

struct ResizeHost : th::Capture {
    int calls = 0, cols = 0, rows = 0;
    void resized_by_application(int c, int r) override {
        ++calls;
        cols = c;
        rows = r;
    }
};

struct App {
    ResizeHost host;
    Terminal t;
    App(int cols, int rows) : t(th::opts(cols, rows)) { t.set_host(&host); }
    App& operator<<(std::string_view s) {
        t.feed(s);
        return *this;
    }
    std::string reply() {
        std::string r = host.out;
        host.out.clear();
        return r;
    }
};

void vs15_narrows_emoji() {
    // U+231A WATCH has emoji presentation (2 cells); VS15 asks for text (1 cell).
    {
        T t(10, 2);
        t << "\xe2\x8c\x9a";
        CHECK_EQ(t.ccol(), 2);
        CHECK(t.cell(0, 0).wide() == Wide::Lead);
        t << "\xef\xb8\x8e" "X";
        CHECK(t.cell(0, 0).wide() == Wide::Narrow);
        CHECK_EQ(utf8(t.cluster(0, 0)), std::string("\xe2\x8c\x9a\xef\xb8\x8e"));
        CHECK_EQ(utf8(t.cluster(0, 1)), std::string("X"));
        CHECK_EQ(t.ccol(), 2);
    }
    {
        // A text-default pictograph (U+2764) stays one cell with VS15, two with VS16.
        T t(10, 2);
        t << "\xe2\x9d\xa4\xef\xb8\x8e" "a\xe2\x9d\xa4\xef\xb8\x8f" "b";
        CHECK_EQ(t.row(0), std::string("\xe2\x9d\xa4\xef\xb8\x8e" "a\xe2\x9d\xa4\xef\xb8\x8f" "b"));
        CHECK_EQ(utf8(t.cluster(0, 1)), std::string("a"));
        CHECK(t.cell(0, 2).wide() == Wide::Lead);
        CHECK_EQ(utf8(t.cluster(0, 4)), std::string("b"));
    }
    {
        // Narrowing at the right margin: the emoji wrapped as a wide cell, then
        // VS15 leaves it one cell with the cursor right after it.
        T t(4, 2);
        t << "ab\xe2\x8c\x9a\xef\xb8\x8e" "c";
        CHECK_EQ(utf8(t.cluster(0, 2)), std::string("\xe2\x8c\x9a\xef\xb8\x8e"));
        CHECK_EQ(utf8(t.cluster(0, 3)), std::string("c"));
        CHECK_EQ(t.crow(), 0);
    }
}

void deccolm() {
    App a(80, 5);
    a << "\x1b[?3h";  // without ?40: ignored
    CHECK_EQ(a.t.cols(), 80);
    CHECK_EQ(a.host.calls, 0);
    a << "\x1b[?3$p";
    CHECK_EQ(a.reply(), std::string("\x1b[?3;2$y"));
    a << "\x1b[?40$p";
    CHECK_EQ(a.reply(), std::string("\x1b[?40;2$y"));

    a << "\x1b[?40habc\x1b[2;4r\x1b[3;3H\x1b[?3h";
    CHECK_EQ(a.t.cols(), 132);
    CHECK_EQ(a.t.rows(), 5);
    CHECK_EQ(a.host.calls, 1);
    CHECK_EQ(a.host.cols, 132);
    CHECK_EQ(a.host.rows, 5);
    CHECK_EQ(a.t.row_text(0), std::string(""));  // cleared
    CHECK_EQ(a.t.cursor().row, 0);
    CHECK_EQ(a.t.cursor().col, 0);
    a << "\x1b[?3$p";
    CHECK_EQ(a.reply(), std::string("\x1b[?3;1$y"));
    // Margins were reset: a line feed at the bottom scrolls the whole screen.
    a << "\x1b[5;1Hz\n";
    CHECK_EQ(a.t.row_text(3), std::string("z"));

    // DECNCSM keeps the screen; switching back reports 80 columns.
    a << "\x1b[?95h\x1b[Hxyz\x1b[?3l";
    CHECK_EQ(a.t.cols(), 80);
    CHECK_EQ(a.host.calls, 2);
    CHECK_EQ(a.host.cols, 80);
    CHECK_EQ(a.t.row_text(0), std::string("xyz"));
    a << "\x1b[?3l";  // already 80: no resize
    CHECK_EQ(a.host.calls, 2);

    // RIS returns a 132-column screen to 80.
    a << "\x1b[?3h";
    CHECK_EQ(a.t.cols(), 132);
    a << "\x1b" "c";
    CHECK_EQ(a.t.cols(), 80);
    CHECK_EQ(a.host.cols, 80);
    a << "\x1b[?40$p";
    CHECK_EQ(a.reply(), std::string("\x1b[?40;2$y"));
}

void xtsave_restore() {
    App a(20, 4);
    a << "\x1b[?2004h\x1b[?7l\x1b[?1000h";
    a << "\x1b[?2004;7;1000;12s";  // XTSAVE
    a << "\x1b[?2004l\x1b[?7h\x1b[?1000l\x1b[?12h";
    CHECK(!a.t.modes().bracketed_paste);
    CHECK(a.t.modes().autowrap);
    a << "\x1b[?2004;7;1000r";  // XTRESTORE (12 not listed: stays set)
    CHECK(a.t.modes().bracketed_paste);
    CHECK(!a.t.modes().autowrap);
    CHECK(a.t.modes().mouse_tracking == MouseTracking::Normal);
    CHECK(a.t.modes().cursor_blink);
    a << "\x1b[?12r";
    CHECK(!a.t.modes().cursor_blink);
    // A mode never saved is left alone.
    a << "\x1b[?1h\x1b[?1r";
    CHECK(a.t.modes().app_cursor_keys);
    // Restoring a mode already in its saved state has no side effects: DECOM
    // would re-home the cursor.
    a << "\x1b[?6s\x1b[3;4H\x1b[?6r";
    CHECK_EQ(a.t.cursor().row, 2);
    CHECK_EQ(a.t.cursor().col, 3);
    // RIS forgets saved modes.
    a << "\x1b" "c\x1b[?2004r";
    CHECK(!a.t.modes().bracketed_paste);
}

void deccra_clusters() {
    T t(10, 3);
    // e + combining acute, a regional-indicator flag (wide cluster), X.
    t << "e\xcc\x81\xf0\x9f\x87\xba\xf0\x9f\x87\xb8X";
    CHECK_EQ(utf8(t.cluster(0, 1)), std::string("\xf0\x9f\x87\xba\xf0\x9f\x87\xb8"));
    t << "\x1b[1;1;1;4;1;2;3;1$v";  // copy row 1 cols 1-4 to row 2 col 3
    CHECK_EQ(utf8(t.cluster(1, 2)), std::string("e\xcc\x81"));
    CHECK_EQ(utf8(t.cluster(1, 3)), std::string("\xf0\x9f\x87\xba\xf0\x9f\x87\xb8"));
    CHECK(t.cell(1, 3).wide() == Wide::Lead);
    CHECK(t.cell(1, 4).wide() == Wide::SpacerTail);
    CHECK_EQ(utf8(t.cluster(1, 5)), std::string("X"));
    // The source is unchanged.
    CHECK_EQ(t.row(0), std::string("e\xcc\x81\xf0\x9f\x87\xba\xf0\x9f\x87\xb8X"));
    // Overlapping copy onto itself shifted right by one.
    t << "\x1b[1;1;1;4;1;1;2;1$v";
    CHECK_EQ(utf8(t.cluster(0, 1)), std::string("e\xcc\x81"));
    CHECK_EQ(utf8(t.cluster(0, 2)), std::string("\xf0\x9f\x87\xba\xf0\x9f\x87\xb8"));
    CHECK_EQ(utf8(t.cluster(0, 4)), std::string("X"));
    // Copying over a cluster with plain cells drops its tail.
    t << "\x1b[3;1Hab\x1b[3;1;3;2;1;1;2;1$v";
    CHECK_EQ(utf8(t.cluster(0, 1)), std::string("a"));
}

void modkeys_resources() {
    App a(20, 3);
    a << "\x1b[?0;1;2;3;4;6;7;5m";  // 5 is not a resource: no reply
    CHECK_EQ(a.reply(), std::string("\x1b[>0;0m\x1b[>1;2m\x1b[>2;2m\x1b[>3;0m\x1b[>4;0m\x1b[>6;0m\x1b[>7;0m"));
    a << "\x1b[>1;3m\x1b[>2;0m\x1b[>3;2m\x1b[>4;2m";
    CHECK_EQ(a.t.modes().modify_cursor_keys, 3);
    CHECK_EQ(a.t.modes().modify_function_keys, 0);
    CHECK_EQ(a.t.modes().modify_keypad_keys, 2);
    a << "\x1b[>1;9m";  // out of range: ignored
    CHECK_EQ(a.t.modes().modify_cursor_keys, 3);
    a << "\x1b[>1m";  // omitted value: the initial one
    CHECK_EQ(a.t.modes().modify_cursor_keys, 2);
    a << "\x1b[>1n";  // disable
    CHECK_EQ(a.t.modes().modify_cursor_keys, -1);
    a << "\x1b[>n";  // disable with Pp omitted: modifyFunctionKeys
    CHECK_EQ(a.t.modes().modify_function_keys, -1);
    a << "\x1b[?1;2m";
    CHECK_EQ(a.reply(), std::string("\x1b[>1;-1m\x1b[>2;-1m"));

    // XTFMTKEYS
    a << "\x1b[?4f";
    CHECK_EQ(a.reply(), std::string("\x1b[>4;0f"));
    a << "\x1b[>4;1f";
    CHECK_EQ(a.t.modes().format_other_keys, 1);
    a << "\x1b[?4f";
    CHECK_EQ(a.reply(), std::string("\x1b[>4;1f"));
    a << "\x1b[>m";  // resets XTMODKEYS only
    CHECK_EQ(a.t.modes().modify_cursor_keys, 2);
    CHECK_EQ(a.t.modes().modify_other_keys, 0);
    CHECK_EQ(a.t.modes().format_other_keys, 1);
    a << "\x1b[>4f";
    CHECK_EQ(a.t.modes().format_other_keys, 0);

    // The encoder follows the terminal's state.
    a << "\x1b[>4;2m\x1b[>4;1f\x1b[>1;3m";
    KeyboardModes km = KeyboardModes::from(a.t);
    CHECK_EQ(encode_key(KeyEvent::character('a', Mod_Ctrl), km), std::string("\x1b[97;5u"));
    CHECK_EQ(encode_key(KeyEvent::functional(Key::Up, Mod_Ctrl), km), std::string("\x1b[>1;5A"));

    // DECSTR restores every resource.
    a << "\x1b[!p";
    CHECK_EQ(a.t.modes().modify_cursor_keys, 2);
    CHECK_EQ(a.t.modes().modify_other_keys, 0);
    CHECK_EQ(a.t.modes().format_other_keys, 0);
}

KeyboardModes levels(int cursor, int function, int keypad) {
    KeyboardModes m;
    m.modify_cursor_keys = cursor;
    m.modify_function_keys = function;
    m.modify_keypad_keys = keypad;
    return m;
}

std::string fk(Key k, KeyMods mods, const KeyboardModes& m) { return encode_key(KeyEvent::functional(k, mods), m); }

void modify_levels_encode() {
    // modifyCursorKeys
    CHECK_EQ(fk(Key::Up, Mod_Ctrl, levels(-1, 2, 0)), std::string("\x1b[A"));
    CHECK_EQ(fk(Key::Up, Mod_Ctrl, levels(0, 2, 0)), std::string("\x1b[5A"));
    CHECK_EQ(fk(Key::Up, Mod_Ctrl, levels(1, 2, 0)), std::string("\x1b[5A"));
    CHECK_EQ(fk(Key::Up, Mod_Ctrl, levels(2, 2, 0)), std::string("\x1b[1;5A"));
    CHECK_EQ(fk(Key::Up, Mod_Ctrl, levels(3, 2, 0)), std::string("\x1b[>1;5A"));
    KeyboardModes ckm = levels(0, 2, 0);
    ckm.app_cursor_keys = true;
    CHECK_EQ(fk(Key::Home, Mod_Shift, ckm), std::string("\x1bO2H"));
    ckm.modify_cursor_keys = 1;
    CHECK_EQ(fk(Key::Home, Mod_Shift, ckm), std::string("\x1b[2H"));
    ckm.modify_cursor_keys = -1;
    CHECK_EQ(fk(Key::Home, Mod_Shift, ckm), std::string("\x1bOH"));
    // Unmodified keys never change.
    CHECK_EQ(fk(Key::Up, Mod_None, levels(3, 3, 3)), std::string("\x1b[A"));
    CHECK_EQ(fk(Key::F1, Mod_None, levels(3, 3, 3)), std::string("\x1bOP"));

    // modifyFunctionKeys
    CHECK_EQ(fk(Key::F1, Mod_Shift, levels(2, 0, 0)), std::string("\x1bO2P"));
    CHECK_EQ(fk(Key::F1, Mod_Shift, levels(2, 1, 0)), std::string("\x1b[2P"));
    CHECK_EQ(fk(Key::F1, Mod_Shift, levels(2, 2, 0)), std::string("\x1b[1;2P"));
    CHECK_EQ(fk(Key::F1, Mod_Shift, levels(2, -1, 0)), std::string("\x1bOP"));
    CHECK_EQ(fk(Key::F5, Mod_Ctrl, levels(2, 0, 0)), std::string("\x1b[15;5~"));
    CHECK_EQ(fk(Key::F5, Mod_Ctrl, levels(2, 3, 0)), std::string("\x1b[>15;5~"));
    CHECK_EQ(fk(Key::Delete, Mod_Alt, levels(2, -1, 0)), std::string("\x1b[3~"));
    // Cursor and function levels are independent.
    CHECK_EQ(fk(Key::Up, Mod_Ctrl, levels(2, -1, 0)), std::string("\x1b[1;5A"));

    // modifyKeypadKeys (application keypad)
    KeyboardModes kp = levels(2, 2, 0);
    kp.app_keypad = true;
    CHECK_EQ(fk(Key::KpAdd, Mod_Ctrl, kp), std::string("\x1bO5k"));
    kp.modify_keypad_keys = 1;
    CHECK_EQ(fk(Key::KpAdd, Mod_Ctrl, kp), std::string("\x1b[5k"));
    kp.modify_keypad_keys = 2;
    CHECK_EQ(fk(Key::KpAdd, Mod_Ctrl, kp), std::string("\x1b[1;5k"));
    kp.modify_keypad_keys = 3;
    CHECK_EQ(fk(Key::KpAdd, Mod_Ctrl, kp), std::string("\x1b[>1;5k"));
    kp.modify_keypad_keys = -1;
    CHECK_EQ(fk(Key::KpAdd, Mod_Ctrl, kp), std::string("\x1bOk"));

    // formatOtherKeys
    KeyboardModes fo;
    fo.modify_other_keys = 2;
    fo.format_other_keys = 1;
    CHECK_EQ(encode_key(KeyEvent::character('a', Mod_Ctrl), fo), std::string("\x1b[97;5u"));
    CHECK_EQ(fk(Key::Tab, KeyMods(Mod_Shift | Mod_Ctrl), fo), std::string("\x1b[9;6u"));
    CHECK_EQ(fk(Key::Enter, Mod_Alt, fo), std::string("\x1b[13;3u"));
    fo.format_other_keys = 0;
    CHECK_EQ(fk(Key::Enter, Mod_Alt, fo), std::string("\x1b[27;3;13~"));
}

} // namespace

int main() {
    init_test();
    vs15_narrows_emoji();
    deccolm();
    xtsave_restore();
    deccra_clusters();
    modkeys_resources();
    modify_levels_encode();
    return check::finish("test_xterm_modes");
}
