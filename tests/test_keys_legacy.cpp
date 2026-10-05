// Legacy (kitty flags 0) key encoding: xterm's function / cursor / keypad keys
// (ctlseqs "PC-Style Function Keys", "VT220-Style Function Keys"), the kitty
// spec's legacy tables (C0 controls, legacy text keys, ctrl mapping), Alt as ESC
// or the 8th bit, and xterm's modifyOtherKeys levels 1 and 2 (input.c).
#include "bropty/input.h"
#include "check.h"

#include <string>

using namespace bropty;

namespace {

KeyboardModes base_modes() { return KeyboardModes(); }

std::string enc(const KeyEvent& ev, const KeyboardModes& m = base_modes()) { return encode_key(ev, m); }
KeyEvent fk(Key k, KeyMods m = Mod_None, KeyAction a = KeyAction::Press) { return KeyEvent::functional(k, m, a); }
KeyEvent ch(char32_t c, KeyMods m = Mod_None, KeyAction a = KeyAction::Press) { return KeyEvent::character(c, m, a); }
KeyboardModes ckm() {
    KeyboardModes m;
    m.app_cursor_keys = true;
    return m;
}
KeyboardModes kpam() {
    KeyboardModes m;
    m.app_keypad = true;
    return m;
}
KeyboardModes mok(int level) {
    KeyboardModes m;
    m.modify_other_keys = level;
    return m;
}
std::string s1(char c) { return std::string(1, c); }

// The spec's "Legacy ctrl mapping of ASCII keys" table (unlisted keys unchanged).
int spec_ctrl(char k) {
    if (k >= 'a' && k <= 'z') return k - 'a' + 1;
    switch (k) {
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
    default: return k;
    }
}
std::string esc(char c) { return std::string("\x1b") + c; }

void cursor_and_function_keys() {
    struct Row {
        Key key;
        const char* normal;  // DECCKM reset
        const char* app;     // DECCKM set
        const char* mod;     // with "%" standing for the modifier parameter
    };
    const Row rows[] = {
        {Key::Up, "\x1b[A", "\x1bOA", "\x1b[1;%A"},
        {Key::Down, "\x1b[B", "\x1bOB", "\x1b[1;%B"},
        {Key::Right, "\x1b[C", "\x1bOC", "\x1b[1;%C"},
        {Key::Left, "\x1b[D", "\x1bOD", "\x1b[1;%D"},
        {Key::Home, "\x1b[H", "\x1bOH", "\x1b[1;%H"},
        {Key::End, "\x1b[F", "\x1bOF", "\x1b[1;%F"},
        {Key::KpBegin, "\x1b[E", "\x1bOE", "\x1b[1;%E"},
        {Key::F1, "\x1bOP", "\x1bOP", "\x1b[1;%P"},
        {Key::F2, "\x1bOQ", "\x1bOQ", "\x1b[1;%Q"},
        {Key::F3, "\x1bOR", "\x1bOR", "\x1b[1;%R"},
        {Key::F4, "\x1bOS", "\x1bOS", "\x1b[1;%S"},
        {Key::Insert, "\x1b[2~", "\x1b[2~", "\x1b[2;%~"},
        {Key::Delete, "\x1b[3~", "\x1b[3~", "\x1b[3;%~"},
        {Key::PageUp, "\x1b[5~", "\x1b[5~", "\x1b[5;%~"},
        {Key::PageDown, "\x1b[6~", "\x1b[6~", "\x1b[6;%~"},
        {Key::F5, "\x1b[15~", "\x1b[15~", "\x1b[15;%~"},
        {Key::F6, "\x1b[17~", "\x1b[17~", "\x1b[17;%~"},
        {Key::F7, "\x1b[18~", "\x1b[18~", "\x1b[18;%~"},
        {Key::F8, "\x1b[19~", "\x1b[19~", "\x1b[19;%~"},
        {Key::F9, "\x1b[20~", "\x1b[20~", "\x1b[20;%~"},
        {Key::F10, "\x1b[21~", "\x1b[21~", "\x1b[21;%~"},
        {Key::F11, "\x1b[23~", "\x1b[23~", "\x1b[23;%~"},
        {Key::F12, "\x1b[24~", "\x1b[24~", "\x1b[24;%~"},
        {Key::Menu, "\x1b[29~", "\x1b[29~", "\x1b[29;%~"},
    };
    struct Mod {
        KeyMods mods;
        const char* param;
    };
    const Mod mods[] = {
        {Mod_Shift, "2"}, {Mod_Alt, "3"}, {Mod_Shift | Mod_Alt, "4"}, {Mod_Ctrl, "5"},
        {Mod_Ctrl | Mod_Shift, "6"}, {Mod_Ctrl | Mod_Alt, "7"}, {Mod_Ctrl | Mod_Alt | Mod_Shift, "8"},
        {Mod_Meta, "9"}, {Mod_Super, "9"}, {Mod_Meta | Mod_Ctrl, "13"},
    };
    for (const Row& r : rows) {
        CHECK_EQ(enc(fk(r.key)), std::string(r.normal));
        CHECK_EQ(enc(fk(r.key), ckm()), std::string(r.app));
        CHECK_EQ(enc(fk(r.key, Mod_None, KeyAction::Repeat)), std::string(r.normal));
        CHECK_EQ(enc(fk(r.key, Mod_None, KeyAction::Release)), std::string());
        // Lock and hyper modifiers are not representable and do not change anything.
        CHECK_EQ(enc(fk(r.key, Mod_NumLock | Mod_CapsLock | Mod_Hyper)), std::string(r.normal));
        for (const Mod& m : mods) {
            std::string want = r.mod;
            want.replace(want.find('%'), 1, m.param);
            CHECK_EQ(enc(fk(r.key, m.mods)), want);
            CHECK_EQ(enc(fk(r.key, m.mods), ckm()), want);  // modified: always CSI
        }
    }
}

void keys_without_legacy_form() {
    CHECK_EQ(enc(fk(Key::F13)), std::string("\x1b[57376u"));
    CHECK_EQ(enc(fk(Key::F35, Mod_Ctrl)), std::string("\x1b[57398;5u"));
    CHECK_EQ(enc(fk(Key::PrintScreen)), std::string("\x1b[57361u"));
    CHECK_EQ(enc(fk(Key::Pause)), std::string("\x1b[57362u"));
    CHECK_EQ(enc(fk(Key::MediaPlayPause)), std::string("\x1b[57430u"));
    CHECK_EQ(enc(fk(Key::RaiseVolume, Mod_NumLock)), std::string("\x1b[57439u"));
    for (Key k : {Key::LeftShift, Key::RightControl, Key::LeftSuper, Key::IsoLevel3Shift, Key::CapsLock,
                  Key::NumLock, Key::ScrollLock}) {
        CHECK_EQ(enc(fk(k)), std::string());
    }
}

void keypad() {
    // Numeric keypad mode: the characters (main-block keys for navigation).
    CHECK_EQ(enc(fk(Key::Kp5)), std::string("5"));
    CHECK_EQ(enc(fk(Key::KpAdd)), std::string("+"));
    CHECK_EQ(enc(fk(Key::KpDecimal)), std::string("."));
    CHECK_EQ(enc(fk(Key::KpEnter)), std::string("\r"));
    // xterm sends the keypad character as is, whatever the modifiers.
    CHECK_EQ(enc(fk(Key::KpEnter, Mod_Alt)), std::string("\r"));
    CHECK_EQ(enc(fk(Key::Kp5, Mod_Alt)), std::string("5"));
    CHECK_EQ(enc(fk(Key::Kp5, Mod_Ctrl)), std::string("5"));
    CHECK_EQ(enc(fk(Key::KpMultiply, Mod_Shift)), std::string("*"));
    CHECK_EQ(enc(fk(Key::KpUp)), std::string("\x1b[A"));
    CHECK_EQ(enc(fk(Key::KpUp), ckm()), std::string("\x1bOA"));
    CHECK_EQ(enc(fk(Key::KpHome, Mod_Ctrl)), std::string("\x1b[1;5H"));
    CHECK_EQ(enc(fk(Key::KpInsert)), std::string("\x1b[2~"));
    CHECK_EQ(enc(fk(Key::KpDelete)), std::string("\x1b[3~"));
    CHECK_EQ(enc(fk(Key::KpPageDown)), std::string("\x1b[6~"));
    // Application keypad (DECKPAM): SS3 forms (xterm's kypd_apl).
    struct Row {
        Key key;
        char final_char;
    };
    const Row rows[] = {
        {Key::Kp0, 'p'}, {Key::Kp1, 'q'}, {Key::Kp2, 'r'}, {Key::Kp3, 's'}, {Key::Kp4, 't'},
        {Key::Kp5, 'u'}, {Key::Kp6, 'v'}, {Key::Kp7, 'w'}, {Key::Kp8, 'x'}, {Key::Kp9, 'y'},
        {Key::KpDecimal, 'n'}, {Key::KpDivide, 'o'}, {Key::KpMultiply, 'j'}, {Key::KpSubtract, 'm'},
        {Key::KpAdd, 'k'}, {Key::KpEnter, 'M'}, {Key::KpEqual, 'X'}, {Key::KpSeparator, 'l'},
    };
    for (const Row& r : rows) {
        CHECK_EQ(enc(fk(r.key), kpam()), std::string("\x1bO") + r.final_char);
        CHECK_EQ(enc(fk(r.key, Mod_Ctrl), kpam()), std::string("\x1bO5") + r.final_char);
    }
    // NumLock on forces numeric output (xterm's realNumLock).
    CHECK_EQ(enc(fk(Key::Kp5, Mod_NumLock), kpam()), std::string("5"));
    CHECK_EQ(enc(fk(Key::KpEnter, Mod_NumLock), kpam()), std::string("\r"));
    // DECKPAM does not affect the navigation keys.
    CHECK_EQ(enc(fk(Key::KpLeft), kpam()), std::string("\x1b[D"));
}

void c0_keys() {
    // Spec "C0 controls" table: no mods, ctrl, alt, shift, ctrl+shift, alt+shift, ctrl+alt.
    struct Row {
        Key key;
        const char* v[7];
    };
    const Row rows[] = {
        {Key::Enter, {"\r", "\r", "\x1b\r", "\r", "\r", "\x1b\r", "\x1b\r"}},
        {Key::Escape, {"\x1b", "\x1b", "\x1b\x1b", "\x1b", "\x1b", "\x1b\x1b", "\x1b\x1b"}},
        {Key::Backspace, {"\x7f", "\x08", "\x1b\x7f", "\x7f", "\x08", "\x1b\x7f", "\x1b\x08"}},
        {Key::Tab, {"\t", "\t", "\x1b\t", "\x1b[Z", "\x1b[Z", "\x1b\x1b[Z", "\x1b\t"}},
    };
    const KeyMods cols[7] = {Mod_None, Mod_Ctrl, Mod_Alt, Mod_Shift, Mod_Ctrl | Mod_Shift,
                             Mod_Alt | Mod_Shift, Mod_Ctrl | Mod_Alt};
    for (const Row& r : rows) {
        for (int i = 0; i < 7; ++i) CHECK_EQ(enc(fk(r.key, cols[i])), std::string(r.v[i]));
    }
    // Space is a text key: 0x20, NUL, ESC 0x20, 0x20, NUL, ESC 0x20, ESC NUL.
    const std::string nul(1, '\0');
    const std::string space_want[7] = {" ", nul, "\x1b ", " ", nul, "\x1b ", "\x1b" + nul};
    for (int i = 0; i < 7; ++i) CHECK_EQ(enc(ch(' ', cols[i])), space_want[i]);
    CHECK_EQ(enc(ch(' ', Mod_Ctrl | Mod_NumLock | Mod_CapsLock)), nul);
    // DECBKM: Backspace sends BS, ctrl+Backspace DEL.
    KeyboardModes bkm;
    bkm.backarrow_sends_bs = true;
    CHECK_EQ(enc(fk(Key::Backspace), bkm), std::string("\x08"));
    CHECK_EQ(enc(fk(Key::Backspace, Mod_Ctrl), bkm), std::string("\x7f"));
    CHECK_EQ(enc(fk(Key::Backspace, Mod_Alt), bkm), std::string("\x1b\x08"));
    CHECK_EQ(enc(fk(Key::Enter, Mod_None, KeyAction::Release)), std::string());
}

void text_keys() {
    // Spec "legacy text keys": for every a-z key and every punctuation / digit
    // key: plain, shift, alt, shift+alt, ctrl, ctrl+alt (kitty_tests/keys.py).
    const char* keys = "`1234567890-=[]\\;',./abcdefghijklmnopqrstuvwxyz";
    const char* shifted = "~!@#$%^&*()_+{}|:\"<>?ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    for (int i = 0; keys[i]; ++i) {
        char k = keys[i], s = shifted[i];
        char32_t c = char32_t(uint8_t(k));
        std::string ctrl = std::string(1, char(spec_ctrl(k)));
        CHECK_EQ(enc(ch(c)), s1(k));
        CHECK_EQ(enc(ch(c, Mod_Shift)), s1(s));
        CHECK_EQ(enc(ch(c, Mod_Alt)), esc(k));
        CHECK_EQ(enc(ch(c, Mod_Shift | Mod_Alt)), esc(s));
        CHECK_EQ(enc(ch(c, Mod_Ctrl)), ctrl);
        CHECK_EQ(enc(ch(c, Mod_Ctrl | Mod_Alt)), "\x1b" + ctrl);
    }
    // ctrl+shift keeps xterm's bytes (kitty's legacy mode would send CSI 105;6u here).
    CHECK_EQ(enc(ch('i', Mod_Ctrl | Mod_Shift)), std::string("\t"));
    CHECK_EQ(enc(ch('a', Mod_Ctrl | Mod_Shift)), std::string("\x01"));
    CHECK_EQ(enc(ch('2', Mod_Ctrl | Mod_Shift)), std::string(1, '\0'));  // ctrl+@
    CHECK_EQ(enc(ch('/', Mod_Ctrl | Mod_Shift)), std::string("\x7f"));   // ctrl+?
    // Odd layouts: the shifted key is whatever the layout says.
    KeyEvent e = ch(':', Mod_Shift | Mod_Alt);
    e.shifted = '/';
    CHECK_EQ(enc(e), std::string("\x1b/"));
    for (char c : std::string("~!@#$%^&*()_+{}|:\"<>?")) CHECK_EQ(enc(ch(char32_t(c), Mod_Alt)), esc(c));
    // Upper-case code points are normalised (the probe's Ctrl+'C').
    CHECK_EQ(enc(ch('C', Mod_Ctrl)), std::string("\x03"));
    CHECK_EQ(enc(ch('c', Mod_Ctrl)), std::string("\x03"));
    // Caps lock.
    CHECK_EQ(enc(ch('a', Mod_CapsLock)), std::string("A"));
    CHECK_EQ(enc(ch('a', Mod_CapsLock | Mod_Shift)), std::string("a"));
    CHECK_EQ(enc(ch('a', Mod_CapsLock | Mod_Alt)), std::string("\x1b" "A"));
    CHECK_EQ(enc(ch('1', Mod_CapsLock)), std::string("1"));
    // Super / Hyper are not xterm modifiers for text.
    CHECK_EQ(enc(ch('a', Mod_Super)), std::string("a"));
    CHECK_EQ(enc(ch('a', Mod_Hyper)), std::string("a"));
    // Repeat is a press; release is nothing.
    CHECK_EQ(enc(ch('a', Mod_None, KeyAction::Repeat)), std::string("a"));
    CHECK_EQ(enc(ch('a', Mod_None, KeyAction::Release)), std::string());
    // Unicode, and the caller's text for layout / IME results.
    CHECK_EQ(enc(ch(0xe9)), std::string("\xc3\xa9"));
    CHECK_EQ(enc(ch(0xe9, Mod_Alt)), std::string("\x1b\xc3\xa9"));
    e = ch('e');
    e.text = "\xc3\xaa";
    CHECK_EQ(enc(e), std::string("\xc3\xaa"));
    e.text = "a\x03";
    CHECK_EQ(enc(e), std::string("a"));
    KeyEvent t;
    t.text = "h\xc3\xa9llo";
    CHECK_EQ(enc(t), std::string("h\xc3\xa9llo"));
    // Non-Latin layout: ctrl / alt fall back to the base-layout key.
    e = ch(0x441, Mod_Ctrl);
    e.base_layout = 'c';
    CHECK_EQ(enc(e), std::string("\x03"));
    e.mods = Mod_Alt;
    CHECK_EQ(enc(e), std::string("\x1b" "c"));
    e.mods = Mod_None;
    CHECK_EQ(enc(e), std::string("\xd1\x81"));
}

void probe_cases() {
    CHECK_EQ(enc(fk(Key::Up)), std::string("\x1b[A"));
    CHECK_EQ(enc(fk(Key::Up), ckm()), std::string("\x1bOA"));
    CHECK_EQ(enc(fk(Key::Up, Mod_Ctrl)), std::string("\x1b[1;5A"));
    CHECK_EQ(enc(fk(Key::Home), ckm()), std::string("\x1bOH"));
    CHECK_EQ(enc(fk(Key::F1, Mod_Shift)), std::string("\x1b[1;2P"));
    CHECK_EQ(enc(fk(Key::F5)), std::string("\x1b[15~"));
    CHECK_EQ(enc(fk(Key::F12, Mod_Ctrl)), std::string("\x1b[24;5~"));
    CHECK_EQ(enc(ch('a', Mod_Ctrl | Mod_Alt)), std::string("\x1b\x01"));
    CHECK_EQ(enc(ch('[', Mod_Ctrl)), std::string("\x1b"));
    CHECK_EQ(enc(ch('/', Mod_Ctrl)), std::string("\x1f"));
    CHECK_EQ(enc(ch('@', Mod_Ctrl)), std::string(1, '\0'));
    CHECK_EQ(enc(ch('\\', Mod_Ctrl)), std::string("\x1c"));
    CHECK_EQ(enc(ch(']', Mod_Ctrl)), std::string("\x1d"));
    CHECK_EQ(enc(ch('^', Mod_Ctrl)), std::string("\x1e"));
    CHECK_EQ(enc(ch('_', Mod_Ctrl)), std::string("\x1f"));
    CHECK_EQ(enc(fk(Key::Backspace, Mod_Ctrl)), std::string("\x08"));
    CHECK_EQ(enc(ch(0xE9)), std::string("\xc3\xa9"));
    CHECK_EQ(enc(fk(Key::Enter)), std::string("\r"));
    CHECK_EQ(enc(fk(Key::Tab)), std::string("\t"));
    CHECK_EQ(enc(fk(Key::Tab, Mod_Shift)), std::string("\x1b[Z"));
    CHECK_EQ(enc(fk(Key::Backspace)), std::string("\x7f"));
    CHECK_EQ(enc(fk(Key::Escape)), std::string("\x1b"));
}

void alt_and_meta() {
    KeyboardModes eight;
    eight.alt_sends_escape = false;
    eight.meta_sends_escape = false;
    // xterm UTF-8 mode: the 8th bit is set and the result sent as UTF-8.
    CHECK_EQ(enc(ch('a', Mod_Alt), eight), std::string("\xc3\xa1"));  // U+00E1
    CHECK_EQ(enc(ch('a', Mod_Alt | Mod_Shift), eight), std::string("\xc3\x81"));
    CHECK_EQ(enc(ch('a', Mod_Alt | Mod_Ctrl), eight), std::string("\xc2\x81"));
    CHECK_EQ(enc(fk(Key::Enter, Mod_Alt), eight), std::string("\xc2\x8d"));
    CHECK_EQ(enc(fk(Key::Backspace, Mod_Alt), eight), std::string("\xc3\xbf"));  // 0x7f | 0x80
    CHECK_EQ(enc(ch(0xe9, Mod_Alt), eight), std::string("\xc3\xa9"));  // not a single byte: unchanged
    CHECK_EQ(enc(fk(Key::Tab, Mod_Alt | Mod_Shift), eight), std::string("\x1b[Z"));
    // Either of ?1036 / ?1039 makes Alt an ESC prefix.
    KeyboardModes alt_only = eight;
    alt_only.alt_sends_escape = true;
    CHECK_EQ(enc(ch('a', Mod_Alt), alt_only), std::string("\x1b" "a"));
    KeyboardModes meta_only = eight;
    meta_only.meta_sends_escape = true;
    CHECK_EQ(enc(ch('a', Mod_Alt), meta_only), std::string("\x1b" "a"));
    CHECK_EQ(enc(ch('a', Mod_Meta), meta_only), std::string("\x1b" "a"));
    // Meta follows ?1036 only.
    CHECK_EQ(enc(ch('a', Mod_Meta), alt_only), std::string("\xc3\xa1"));
    CHECK_EQ(enc(ch('a', Mod_Meta)), std::string("\x1b" "a"));
}

void modify_other_keys_level1() {
    const KeyboardModes m = mok(1);
    // Well-known combinations keep their bytes.
    CHECK_EQ(enc(ch('a', Mod_Ctrl), m), std::string("\x01"));
    CHECK_EQ(enc(ch('a', Mod_Ctrl | Mod_Shift), m), std::string("\x01"));
    CHECK_EQ(enc(ch('a', Mod_Shift), m), std::string("A"));
    CHECK_EQ(enc(ch('a', Mod_Alt), m), std::string("\x1b" "a"));
    CHECK_EQ(enc(ch('a', Mod_Ctrl | Mod_Alt), m), std::string("\x1b\x01"));
    CHECK_EQ(enc(ch(' ', Mod_Ctrl), m), std::string(1, '\0'));
    CHECK_EQ(enc(ch('2', Mod_Ctrl), m), std::string(1, '\0'));
    CHECK_EQ(enc(ch('3', Mod_Ctrl), m), std::string("\x1b"));
    CHECK_EQ(enc(ch('/', Mod_Ctrl), m), std::string("\x1f"));
    CHECK_EQ(enc(ch('1', Mod_Shift), m), std::string("!"));
    CHECK_EQ(enc(ch('1', Mod_Alt), m), std::string("\x1b" "1"));
    CHECK_EQ(enc(fk(Key::Backspace, Mod_Ctrl), m), std::string("\x08"));
    CHECK_EQ(enc(fk(Key::Backspace, Mod_Alt), m), std::string("\x1b\x7f"));
    CHECK_EQ(enc(fk(Key::Backspace, Mod_Shift), m), std::string("\x7f"));
    CHECK_EQ(enc(fk(Key::Escape, Mod_Ctrl), m), std::string("\x1b"));
    CHECK_EQ(enc(fk(Key::Escape, Mod_Alt), m), std::string("\x1b\x1b"));
    CHECK_EQ(enc(fk(Key::Enter, Mod_Alt), m), std::string("\x1b\r"));
    CHECK_EQ(enc(fk(Key::Enter, Mod_Ctrl | Mod_Alt), m), std::string("\x1b\r"));
    CHECK_EQ(enc(fk(Key::Tab, Mod_Shift), m), std::string("\x1b[Z"));
    // The rest become CSI 27 ; m ; code ~ (the Emacs set from xterm's FAQ).
    CHECK_EQ(enc(fk(Key::Tab, Mod_Ctrl), m), std::string("\x1b[27;5;9~"));
    CHECK_EQ(enc(ch(',', Mod_Ctrl), m), std::string("\x1b[27;5;44~"));
    CHECK_EQ(enc(ch('.', Mod_Ctrl), m), std::string("\x1b[27;5;46~"));
    CHECK_EQ(enc(ch('1', Mod_Ctrl), m), std::string("\x1b[27;5;49~"));
    CHECK_EQ(enc(ch('1', Mod_Ctrl | Mod_Shift), m), std::string("\x1b[27;6;33~"));
    CHECK_EQ(enc(ch(';', Mod_Ctrl), m), std::string("\x1b[27;5;59~"));
    CHECK_EQ(enc(fk(Key::Enter, Mod_Ctrl), m), std::string("\x1b[27;5;13~"));
    CHECK_EQ(enc(fk(Key::Enter, Mod_Shift), m), std::string("\x1b[27;2;13~"));
    // Unmodified keys and functional keys are untouched.
    CHECK_EQ(enc(ch('a'), m), std::string("a"));
    CHECK_EQ(enc(fk(Key::Enter), m), std::string("\r"));
    CHECK_EQ(enc(fk(Key::Up, Mod_Ctrl), m), std::string("\x1b[1;5A"));
    CHECK_EQ(enc(fk(Key::F5, Mod_Ctrl), m), std::string("\x1b[15;5~"));
}

void modify_other_keys_level2() {
    const KeyboardModes m = mok(2);
    CHECK_EQ(enc(ch('a', Mod_Ctrl), m), std::string("\x1b[27;5;97~"));
    CHECK_EQ(enc(ch('a', Mod_Shift), m), std::string("\x1b[27;2;65~"));  // xterm FAQ: "Shift-Q"
    CHECK_EQ(enc(ch('a', Mod_Alt), m), std::string("\x1b[27;3;97~"));
    CHECK_EQ(enc(ch('a', Mod_Ctrl | Mod_Shift), m), std::string("\x1b[27;6;65~"));
    CHECK_EQ(enc(ch('a', Mod_Ctrl | Mod_Alt), m), std::string("\x1b[27;7;97~"));
    CHECK_EQ(enc(ch('1', Mod_Shift), m), std::string("!"));  // "1" and "!" encode natively
    CHECK_EQ(enc(ch('1', Mod_Ctrl), m), std::string("\x1b[27;5;49~"));
    CHECK_EQ(enc(ch('1', Mod_Alt), m), std::string("\x1b[27;3;49~"));
    CHECK_EQ(enc(ch(' ', Mod_Shift), m), std::string("\x1b[27;2;32~"));
    CHECK_EQ(enc(ch(' ', Mod_Ctrl), m), std::string("\x1b[27;5;32~"));
    CHECK_EQ(enc(ch('2', Mod_Ctrl), m), std::string("\x1b[27;5;50~"));
    CHECK_EQ(enc(ch('[', Mod_Ctrl), m), std::string("\x1b[27;5;91~"));
    CHECK_EQ(enc(fk(Key::Enter, Mod_Ctrl), m), std::string("\x1b[27;5;13~"));
    CHECK_EQ(enc(fk(Key::Enter, Mod_Shift), m), std::string("\x1b[27;2;13~"));
    CHECK_EQ(enc(fk(Key::Enter, Mod_Alt), m), std::string("\x1b[27;3;13~"));
    CHECK_EQ(enc(fk(Key::Tab, Mod_Ctrl), m), std::string("\x1b[27;5;9~"));
    CHECK_EQ(enc(fk(Key::Tab, Mod_Shift), m), std::string("\x1b[Z"));
    CHECK_EQ(enc(fk(Key::Tab, Mod_Ctrl | Mod_Shift), m), std::string("\x1b[27;6;9~"));
    CHECK_EQ(enc(fk(Key::Escape, Mod_Ctrl), m), std::string("\x1b[27;5;27~"));
    CHECK_EQ(enc(fk(Key::Escape, Mod_Shift), m), std::string("\x1b[27;2;27~"));
    // Backspace: the DEL-sending key is xterm's Delete keysym.
    CHECK_EQ(enc(fk(Key::Backspace), m), std::string("\x7f"));
    CHECK_EQ(enc(fk(Key::Backspace, Mod_Shift), m), std::string("\x1b[27;2;127~"));
    CHECK_EQ(enc(fk(Key::Backspace, Mod_Alt), m), std::string("\x1b[27;3;127~"));
    CHECK_EQ(enc(fk(Key::Backspace, Mod_Ctrl), m), std::string("\x08"));
    CHECK_EQ(enc(fk(Key::Backspace, Mod_Ctrl | Mod_Alt), m), std::string("\x1b[27;7;8~"));
    KeyboardModes mb = m;
    mb.backarrow_sends_bs = true;
    CHECK_EQ(enc(fk(Key::Backspace), mb), std::string("\x08"));
    CHECK_EQ(enc(fk(Key::Backspace, Mod_Ctrl), mb), std::string("\x7f"));
    CHECK_EQ(enc(fk(Key::Backspace, Mod_Shift), mb), std::string("\x1b[27;2;8~"));
    // Untouched: unmodified keys, functional keys, releases.
    CHECK_EQ(enc(ch('a'), m), std::string("a"));
    CHECK_EQ(enc(fk(Key::Enter), m), std::string("\r"));
    CHECK_EQ(enc(fk(Key::Up, Mod_Ctrl), m), std::string("\x1b[1;5A"));
    CHECK_EQ(enc(fk(Key::Kp5, Mod_Ctrl), m), std::string("5"));
    CHECK_EQ(enc(ch('a', Mod_Ctrl, KeyAction::Release), m), std::string());
    // Level 3 behaves as 2; kitty flags take precedence over modifyOtherKeys.
    CHECK_EQ(enc(ch('a', Mod_Ctrl), mok(3)), std::string("\x1b[27;5;97~"));
    KeyboardModes k = m;
    k.kitty_flags = 1;
    CHECK_EQ(enc(ch('a', Mod_Ctrl), k), std::string("\x1b[97;5u"));
}

} // namespace

int main() {
    init_test();
    cursor_and_function_keys();
    keys_without_legacy_form();
    keypad();
    c0_keys();
    text_keys();
    probe_cases();
    alt_and_meta();
    modify_other_keys_level1();
    modify_other_keys_level2();
    return check::finish("test_keys_legacy");
}
