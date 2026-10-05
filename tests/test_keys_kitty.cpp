// Kitty keyboard protocol encoding, against the spec
// (sw.kovidgoyal.net/kitty/keyboard-protocol: the functional key table, the
// progressive-enhancement sections, the legacy C0 / text key tables) and the
// cases in kitty's own kitty_tests/keys.py.
#include "bropty/input.h"
#include "check.h"

#include <string>

using namespace bropty;

namespace {

constexpr uint32_t Disambiguate = 1, Events = 2, Alternates = 4, AllKeys = 8, Text = 16;

std::string enc(uint32_t flags, const KeyEvent& ev, bool app_cursor = false, bool bkm = false) {
    KeyboardModes m;
    m.kitty_flags = flags;
    m.app_cursor_keys = app_cursor;
    m.backarrow_sends_bs = bkm;
    return encode_key(ev, m);
}
KeyEvent fk(Key k, KeyMods m = Mod_None, KeyAction a = KeyAction::Press) { return KeyEvent::functional(k, m, a); }
KeyEvent ch(char32_t c, KeyMods m = Mod_None, KeyAction a = KeyAction::Press) { return KeyEvent::character(c, m, a); }
Key operator+(Key k, int n) { return Key(uint32_t(k) + uint32_t(n)); }
std::string csi(const std::string& body) { return "\x1b[" + body; }

void functional_table() {
    // Spec "Functional key definitions", flag 1: unmodified and with ctrl.
    struct Row {
        Key key;
        const char* plain;
        const char* ctrl;
    };
    const Row rows[] = {
        {Key::Escape, "\x1b[27u", "\x1b[27;5u"},
        {Key::Enter, "\r", "\x1b[13;5u"},
        {Key::Tab, "\t", "\x1b[9;5u"},
        {Key::Backspace, "\x7f", "\x1b[127;5u"},
        {Key::Insert, "\x1b[2~", "\x1b[2;5~"},
        {Key::Delete, "\x1b[3~", "\x1b[3;5~"},
        {Key::Left, "\x1b[D", "\x1b[1;5D"},
        {Key::Right, "\x1b[C", "\x1b[1;5C"},
        {Key::Up, "\x1b[A", "\x1b[1;5A"},
        {Key::Down, "\x1b[B", "\x1b[1;5B"},
        {Key::PageUp, "\x1b[5~", "\x1b[5;5~"},
        {Key::PageDown, "\x1b[6~", "\x1b[6;5~"},
        {Key::Home, "\x1b[H", "\x1b[1;5H"},
        {Key::End, "\x1b[F", "\x1b[1;5F"},
        {Key::PrintScreen, "\x1b[57361u", "\x1b[57361;5u"},
        {Key::Pause, "\x1b[57362u", "\x1b[57362;5u"},
        {Key::Menu, "\x1b[57363u", "\x1b[57363;5u"},
        {Key::F1, "\x1b[P", "\x1b[1;5P"},
        {Key::F2, "\x1b[Q", "\x1b[1;5Q"},
        {Key::F3, "\x1b[13~", "\x1b[13;5~"},
        {Key::F4, "\x1b[S", "\x1b[1;5S"},
        {Key::F5, "\x1b[15~", "\x1b[15;5~"},
        {Key::F6, "\x1b[17~", "\x1b[17;5~"},
        {Key::F7, "\x1b[18~", "\x1b[18;5~"},
        {Key::F8, "\x1b[19~", "\x1b[19;5~"},
        {Key::F9, "\x1b[20~", "\x1b[20;5~"},
        {Key::F10, "\x1b[21~", "\x1b[21;5~"},
        {Key::F11, "\x1b[23~", "\x1b[23;5~"},
        {Key::F12, "\x1b[24~", "\x1b[24;5~"},
        {Key::KpBegin, "\x1b[E", "\x1b[1;5E"},
    };
    for (const Row& r : rows) {
        CHECK_EQ(enc(Disambiguate, fk(r.key)), std::string(r.plain));
        CHECK_EQ(enc(Disambiguate, fk(r.key, Mod_Ctrl)), std::string(r.ctrl));
    }
    // F13..F35 = 57376..57398
    for (int i = 0; i <= 22; ++i) {
        CHECK_EQ(enc(Disambiguate, fk(Key::F13 + i)), csi(std::to_string(57376 + i) + "u"));
        CHECK_EQ(enc(Disambiguate, fk(Key::F13 + i, Mod_Shift)), csi(std::to_string(57376 + i) + ";2u"));
    }
    CHECK_EQ(uint32_t(Key::F35), 57398u);
    // Keypad keys that type nothing: KP_LEFT 57417 .. KP_DELETE 57426, KP_ENTER 57414.
    for (int i = 0; i <= 9; ++i) {
        CHECK_EQ(enc(Disambiguate, fk(Key::KpLeft + i)), csi(std::to_string(57417 + i) + "u"));
    }
    CHECK_EQ(enc(Disambiguate, fk(Key::KpEnter)), std::string("\x1b[57414u"));
    CHECK_EQ(enc(Disambiguate, fk(Key::KpPageUp)), std::string("\x1b[57421u"));
    CHECK_EQ(enc(Disambiguate, fk(Key::KpPageUp, Mod_Ctrl)), std::string("\x1b[57421;5u"));
    // Keypad keys that type text stay text unless modified (or flag 8).
    CHECK_EQ(enc(Disambiguate, fk(Key::Kp0)), std::string("0"));
    CHECK_EQ(enc(Disambiguate, fk(Key::Kp0, Mod_Ctrl)), std::string("\x1b[57399;5u"));
    CHECK_EQ(enc(Disambiguate, fk(Key::Kp9, Mod_Alt)), std::string("\x1b[57408;3u"));
    CHECK_EQ(enc(Disambiguate, fk(Key::KpDecimal, Mod_Ctrl)), std::string("\x1b[57409;5u"));
    CHECK_EQ(enc(Disambiguate, fk(Key::KpSeparator, Mod_Ctrl)), std::string("\x1b[57416;5u"));
    CHECK_EQ(enc(Disambiguate, fk(Key::KpAdd)), std::string("+"));
    // Media keys 57428..57440.
    for (int i = 0; i <= 12; ++i) {
        CHECK_EQ(enc(Disambiguate, fk(Key::MediaPlay + i)), csi(std::to_string(57428 + i) + "u"));
    }
    CHECK_EQ(uint32_t(Key::MuteVolume), 57440u);
    // Modifier and lock keys only report under flag 8.
    for (int i = 0; i <= 13; ++i) {
        CHECK_EQ(enc(Disambiguate | Events, fk(Key::LeftShift + i)), std::string());
        CHECK_EQ(enc(AllKeys, fk(Key::LeftShift + i)), csi(std::to_string(57441 + i) + "u"));
    }
    CHECK_EQ(uint32_t(Key::IsoLevel5Shift), 57454u);
    for (Key k : {Key::CapsLock, Key::ScrollLock, Key::NumLock}) {
        CHECK_EQ(enc(Disambiguate, fk(k)), std::string());
    }
    CHECK_EQ(enc(AllKeys, fk(Key::CapsLock)), std::string("\x1b[57358u"));
    CHECK_EQ(enc(AllKeys, fk(Key::ScrollLock)), std::string("\x1b[57359u"));
    CHECK_EQ(enc(AllKeys, fk(Key::NumLock)), std::string("\x1b[57360u"));
    // Modifier key events carry the modifier state including the event itself.
    CHECK_EQ(enc(AllKeys | Events, fk(Key::LeftControl, Mod_Ctrl)), std::string("\x1b[57442;5u"));
    CHECK_EQ(enc(AllKeys | Events, fk(Key::LeftControl, Mod_None, KeyAction::Release)),
             std::string("\x1b[57442;1:3u"));
    CHECK_EQ(enc(AllKeys | Events, fk(Key::RightAlt, Mod_Alt | Mod_Shift, KeyAction::Repeat)),
             std::string("\x1b[57449;4:2u"));
}

void modifiers() {
    // 1 + shift|alt<<1|ctrl<<2|super<<3|hyper<<4|meta<<5|caps<<6|num<<7
    CHECK_EQ(enc(Disambiguate, fk(Key::Up, Mod_Shift)), std::string("\x1b[1;2A"));
    CHECK_EQ(enc(Disambiguate, fk(Key::Up, Mod_Alt)), std::string("\x1b[1;3A"));
    CHECK_EQ(enc(Disambiguate, fk(Key::Up, Mod_Super)), std::string("\x1b[1;9A"));
    CHECK_EQ(enc(Disambiguate, fk(Key::Up, Mod_Hyper)), std::string("\x1b[1;17A"));
    CHECK_EQ(enc(Disambiguate, fk(Key::Up, Mod_Meta)), std::string("\x1b[1;33A"));
    CHECK_EQ(enc(Disambiguate, fk(Key::Up, Mod_Shift | Mod_Alt | Mod_Ctrl | Mod_Super | Mod_Hyper | Mod_Meta)),
             std::string("\x1b[1;64A"));
    // Lock modifiers: reported on keys that are escape codes anyway (kitty's encoder) ...
    CHECK_EQ(enc(Disambiguate, fk(Key::Up, Mod_NumLock)), std::string("\x1b[1;129A"));
    CHECK_EQ(enc(Disambiguate, fk(Key::Escape, Mod_CapsLock)), std::string("\x1b[27;65u"));
    CHECK_EQ(enc(Disambiguate, ch('a', Mod_Ctrl | Mod_CapsLock)), std::string("\x1b[97;69u"));
    CHECK_EQ(enc(Disambiguate, ch('a', Mod_Ctrl | Mod_NumLock)), std::string("\x1b[97;133u"));
    // ... but never turn text, or Enter / Tab / Backspace, into escape codes.
    CHECK_EQ(enc(Disambiguate, ch('a', Mod_CapsLock)), std::string("A"));
    CHECK_EQ(enc(Disambiguate, ch('a', Mod_NumLock)), std::string("a"));
    CHECK_EQ(enc(Disambiguate, fk(Key::Enter, Mod_NumLock | Mod_CapsLock)), std::string("\r"));
    CHECK_EQ(enc(Disambiguate, fk(Key::Tab, Mod_CapsLock)), std::string("\t"));
    CHECK_EQ(enc(Disambiguate, fk(Key::Backspace, Mod_NumLock)), std::string("\x7f"));
    // Under flag 8 they are reported everywhere.
    CHECK_EQ(enc(AllKeys, ch('a', Mod_CapsLock)), std::string("\x1b[97;65u"));
    CHECK_EQ(enc(AllKeys, fk(Key::Enter, Mod_NumLock)), std::string("\x1b[13;129u"));
}

void disambiguate() {
    // Legacy text keys stay text when unmodified or shift-only.
    CHECK_EQ(enc(Disambiguate, ch('a')), std::string("a"));
    CHECK_EQ(enc(Disambiguate, ch('a', Mod_Shift)), std::string("A"));
    CHECK_EQ(enc(Disambiguate, ch('3', Mod_Shift)), std::string("#"));
    CHECK_EQ(enc(Disambiguate, ch(' ')), std::string(" "));
    CHECK_EQ(enc(Disambiguate, ch(' ', Mod_Shift)), std::string(" "));
    CHECK_EQ(enc(Disambiguate, ch(0xe9)), std::string("\xc3\xa9"));
    // ctrl / alt / ctrl+alt / shift+alt / ctrl+shift: CSI u with the unshifted key.
    CHECK_EQ(enc(Disambiguate, ch('a', Mod_Ctrl)), std::string("\x1b[97;5u"));
    CHECK_EQ(enc(Disambiguate, ch('a', Mod_Alt)), std::string("\x1b[97;3u"));
    CHECK_EQ(enc(Disambiguate, ch('a', Mod_Ctrl | Mod_Alt)), std::string("\x1b[97;7u"));
    CHECK_EQ(enc(Disambiguate, ch('a', Mod_Shift | Mod_Alt)), std::string("\x1b[97;4u"));
    CHECK_EQ(enc(Disambiguate, ch('a', Mod_Ctrl | Mod_Shift)), std::string("\x1b[97;6u"));
    CHECK_EQ(enc(Disambiguate, ch('A', Mod_Ctrl | Mod_Shift)), std::string("\x1b[97;6u"));  // never 65
    CHECK_EQ(enc(Disambiguate, ch('3', Mod_Shift | Mod_Alt)), std::string("\x1b[51;4u"));
    CHECK_EQ(enc(Disambiguate, ch(' ', Mod_Ctrl)), std::string("\x1b[32;5u"));
    CHECK_EQ(enc(Disambiguate, ch('[', Mod_Alt)), std::string("\x1b[91;3u"));
    CHECK_EQ(enc(Disambiguate, ch('i', Mod_Ctrl)), std::string("\x1b[105;5u"));
    CHECK_EQ(enc(Disambiguate, ch('a', Mod_Super)), std::string("\x1b[97;9u"));
    // Esc, and the C0 keys with modifiers.
    CHECK_EQ(enc(Disambiguate, fk(Key::Escape, Mod_Alt)), std::string("\x1b[27;3u"));
    CHECK_EQ(enc(Disambiguate, fk(Key::Enter, Mod_Shift)), std::string("\x1b[13;2u"));
    CHECK_EQ(enc(Disambiguate, fk(Key::Enter, Mod_Alt)), std::string("\x1b[13;3u"));
    CHECK_EQ(enc(Disambiguate, fk(Key::Tab, Mod_Shift)), std::string("\x1b[9;2u"));
    CHECK_EQ(enc(Disambiguate, fk(Key::Tab, Mod_Ctrl | Mod_Shift)), std::string("\x1b[9;6u"));
    CHECK_EQ(enc(Disambiguate, fk(Key::Backspace, Mod_Alt)), std::string("\x1b[127;3u"));
    CHECK_EQ(enc(Disambiguate, fk(Key::Backspace, Mod_None), false, true), std::string("\x08"));  // DECBKM
    // DECCKM has no effect once the protocol is on.
    CHECK_EQ(enc(Disambiguate, fk(Key::Up), true), std::string("\x1b[A"));
    CHECK_EQ(enc(Disambiguate, fk(Key::Home), true), std::string("\x1b[H"));
    // No release / repeat reporting without flag 2: repeat is a press, release is nothing.
    CHECK_EQ(enc(Disambiguate, ch('a', Mod_Ctrl, KeyAction::Repeat)), std::string("\x1b[97;5u"));
    CHECK_EQ(enc(Disambiguate, ch('a', Mod_Ctrl, KeyAction::Release)), std::string());
    CHECK_EQ(enc(Disambiguate, fk(Key::Up, Mod_None, KeyAction::Release)), std::string());
}

void event_types() {
    const uint32_t f = Disambiguate | Events;
    CHECK_EQ(enc(f, ch('a', Mod_Ctrl, KeyAction::Repeat)), std::string("\x1b[97;5:2u"));
    CHECK_EQ(enc(f, ch('a', Mod_Ctrl, KeyAction::Release)), std::string("\x1b[97;5:3u"));
    CHECK_EQ(enc(f, ch('a', Mod_None, KeyAction::Repeat)), std::string("a"));  // text repeats as text
    CHECK_EQ(enc(f, ch('a', Mod_None, KeyAction::Release)), std::string("\x1b[97;1:3u"));
    CHECK_EQ(enc(f, ch('a', Mod_Shift, KeyAction::Release)), std::string("\x1b[97;2:3u"));
    CHECK_EQ(enc(f, fk(Key::Up, Mod_None, KeyAction::Repeat)), std::string("\x1b[1;1:2A"));
    CHECK_EQ(enc(f, fk(Key::Up, Mod_None, KeyAction::Release)), std::string("\x1b[1;1:3A"));
    CHECK_EQ(enc(f, fk(Key::F1, Mod_None, KeyAction::Release)), std::string("\x1b[1;1:3P"));
    CHECK_EQ(enc(f, fk(Key::F3, Mod_Shift, KeyAction::Release)), std::string("\x1b[13;2:3~"));
    CHECK_EQ(enc(f, fk(Key::Escape, Mod_None, KeyAction::Release)), std::string("\x1b[27;1:3u"));
    CHECK_EQ(enc(f, fk(Key::Escape, Mod_None, KeyAction::Press)), std::string("\x1b[27u"));
    // Unmodified Enter / Tab / Backspace: no release without flag 8.
    CHECK_EQ(enc(f, fk(Key::Enter, Mod_None, KeyAction::Release)), std::string());
    CHECK_EQ(enc(f, fk(Key::Tab, Mod_None, KeyAction::Release)), std::string());
    CHECK_EQ(enc(f, fk(Key::Backspace, Mod_None, KeyAction::Release)), std::string());
    CHECK_EQ(enc(f, fk(Key::Enter, Mod_NumLock | Mod_CapsLock, KeyAction::Release)), std::string());
    CHECK_EQ(enc(f, fk(Key::Backspace)), std::string("\x7f"));
    CHECK_EQ(enc(f, fk(Key::Enter, Mod_None, KeyAction::Repeat)), std::string("\r"));
    CHECK_EQ(enc(f, fk(Key::Enter, Mod_Ctrl, KeyAction::Release)), std::string("\x1b[13;5:3u"));
    CHECK_EQ(enc(f | AllKeys, fk(Key::Enter, Mod_None, KeyAction::Release)), std::string("\x1b[13;1:3u"));
    CHECK_EQ(enc(f | AllKeys, fk(Key::Tab, Mod_None, KeyAction::Repeat)), std::string("\x1b[9;1:2u"));
    // Flag 2 alone keeps legacy press encodings but reports releases as CSI.
    CHECK_EQ(enc(Events, ch('a', Mod_Ctrl)), std::string("\x01"));
    CHECK_EQ(enc(Events, ch('a')), std::string("a"));
    CHECK_EQ(enc(Events, ch('a', Mod_None, KeyAction::Release)), std::string("\x1b[97;1:3u"));
    CHECK_EQ(enc(Events, fk(Key::Escape)), std::string("\x1b"));
    // Deviation from kitty, which re-sends a bare ESC here:
    CHECK_EQ(enc(Events, fk(Key::Escape, Mod_None, KeyAction::Release)), std::string("\x1b[27;1:3u"));
    CHECK_EQ(enc(Events, fk(Key::Up), true), std::string("\x1b[A"));  // not legacy mode: no SS3
}

void alternate_keys() {
    const uint32_t f = Disambiguate | Alternates;
    KeyEvent e = ch('a', Mod_Ctrl | Mod_Shift);
    e.shifted = 'A';
    CHECK_EQ(enc(f, e), std::string("\x1b[97:65;6u"));
    e = ch('a', Mod_Ctrl);
    e.shifted = 'A';  // shifted key only while shift is held
    CHECK_EQ(enc(f, e), std::string("\x1b[97;5u"));
    // Cyrillic es: base layout key 'c'; "::base" when no shifted key applies.
    e = ch(0x441, Mod_Ctrl);
    e.shifted = 0x421;
    e.base_layout = 'c';
    CHECK_EQ(enc(f, e), std::string("\x1b[1089::99;5u"));
    e.mods = Mod_Ctrl | Mod_Shift;
    CHECK_EQ(enc(f, e), std::string("\x1b[1089:1057:99;6u"));
    // A base layout key equal to the key is not repeated.
    e = ch('c', Mod_Ctrl);
    e.base_layout = 'c';
    CHECK_EQ(enc(f, e), std::string("\x1b[99;5u"));
    // Only affects keys that are escape codes anyway.
    CHECK_EQ(enc(f, ch('a', Mod_Shift)), std::string("A"));
    CHECK_EQ(enc(f, fk(Key::Up, Mod_Ctrl | Mod_Shift)), std::string("\x1b[1;6A"));
    // With flag 8 (kitty_tests: shift+a -> CSI 97:65;2u, shift+a/base b -> CSI 97:65:98;2u).
    CHECK_EQ(enc(AllKeys | Alternates, ch('a', Mod_Shift)), std::string("\x1b[97:65;2u"));
    e = ch('a', Mod_Shift);
    e.base_layout = 'b';
    CHECK_EQ(enc(AllKeys | Alternates, e), std::string("\x1b[97:65:98;2u"));
    e = ch('a');
    e.base_layout = 'b';
    CHECK_EQ(enc(AllKeys | Alternates, e), std::string("\x1b[97::98u"));
    // Flag 4 alone leaves legacy encodings alone.
    CHECK_EQ(enc(Alternates, ch('a', Mod_Ctrl)), std::string("\x01"));
    CHECK_EQ(enc(Alternates, fk(Key::Up), true), std::string("\x1bOA"));
    CHECK_EQ(enc(Alternates, fk(Key::F1)), std::string("\x1bOP"));
    CHECK_EQ(enc(Alternates, fk(Key::Enter, Mod_Ctrl)), std::string("\r"));
    CHECK_EQ(enc(Alternates, fk(Key::Enter, Mod_Alt)), std::string("\x1b\r"));
    CHECK_EQ(enc(Alternates, fk(Key::Tab, Mod_Shift)), std::string("\x1b[Z"));
    CHECK_EQ(enc(Alternates, fk(Key::Tab, Mod_Shift | Mod_Alt)), std::string("\x1b\x1b[Z"));
    CHECK_EQ(enc(Alternates, fk(Key::Backspace, Mod_Ctrl)), std::string("\x08"));
    CHECK_EQ(enc(Alternates, fk(Key::Menu)), std::string("\x1b[29~"));
    CHECK_EQ(enc(Alternates, fk(Key::Escape)), std::string("\x1b"));
}

void report_all() {
    CHECK_EQ(enc(AllKeys, ch('a')), std::string("\x1b[97u"));
    CHECK_EQ(enc(AllKeys, ch('a', Mod_None, KeyAction::Repeat)), std::string("\x1b[97u"));
    CHECK_EQ(enc(AllKeys, ch('a', Mod_None, KeyAction::Release)), std::string());  // no flag 2
    CHECK_EQ(enc(AllKeys, ch('a', Mod_Shift)), std::string("\x1b[97;2u"));
    CHECK_EQ(enc(AllKeys, ch('a', Mod_Ctrl)), std::string("\x1b[97;5u"));
    CHECK_EQ(enc(AllKeys, ch(' ')), std::string("\x1b[32u"));
    CHECK_EQ(enc(AllKeys, ch(0xe9)), std::string("\x1b[233u"));
    CHECK_EQ(enc(AllKeys, fk(Key::Up)), std::string("\x1b[A"));
    CHECK_EQ(enc(AllKeys, fk(Key::F1)), std::string("\x1b[P"));
    CHECK_EQ(enc(AllKeys, fk(Key::Escape)), std::string("\x1b[27u"));
    CHECK_EQ(enc(AllKeys, fk(Key::Enter)), std::string("\x1b[13u"));
    CHECK_EQ(enc(AllKeys, fk(Key::Enter, Mod_Ctrl)), std::string("\x1b[13;5u"));
    CHECK_EQ(enc(AllKeys, fk(Key::Tab)), std::string("\x1b[9u"));
    CHECK_EQ(enc(AllKeys, fk(Key::Backspace)), std::string("\x1b[127u"));
    CHECK_EQ(enc(AllKeys, fk(Key::Kp1)), std::string("\x1b[57400u"));
    CHECK_EQ(enc(AllKeys, fk(Key::KpEnter)), std::string("\x1b[57414u"));
    CHECK_EQ(enc(AllKeys | Events, ch('w', Mod_None, KeyAction::Release)), std::string("\x1b[119;1:3u"));
    CHECK_EQ(enc(AllKeys | Events, ch('w', Mod_None, KeyAction::Repeat)), std::string("\x1b[119;1:2u"));
}

void associated_text() {
    const uint32_t f = AllKeys | Text;
    CHECK_EQ(enc(f, ch('a')), std::string("\x1b[97;;97u"));
    CHECK_EQ(enc(f, ch('a', Mod_Shift)), std::string("\x1b[97;2;65u"));
    KeyEvent e = ch('a', Mod_Shift);
    e.text = "AB";
    CHECK_EQ(enc(f, e), std::string("\x1b[97;2;65:66u"));
    e = ch('a');
    e.text = "\xc3\xa5";  // a dead-key / compose result
    CHECK_EQ(enc(f, e), std::string("\x1b[97;;229u"));
    e.text = "\x01";  // control characters are never associated text
    CHECK_EQ(enc(f, e), std::string("\x1b[97u"));
    CHECK_EQ(enc(f, ch('a', Mod_Ctrl)), std::string("\x1b[97;5u"));
    CHECK_EQ(enc(f, ch('a', Mod_Alt)), std::string("\x1b[97;3u"));
    CHECK_EQ(enc(f, ch('a', Mod_CapsLock)), std::string("\x1b[97;65;65u"));
    CHECK_EQ(enc(f | Events, ch('a', Mod_None, KeyAction::Repeat)), std::string("\x1b[97;1:2;97u"));
    CHECK_EQ(enc(f | Events, ch('a', Mod_None, KeyAction::Release)), std::string("\x1b[97;1:3u"));
    CHECK_EQ(enc(f, fk(Key::Kp1)), std::string("\x1b[57400;;49u"));
    CHECK_EQ(enc(f, fk(Key::Enter)), std::string("\x1b[13u"));
    CHECK_EQ(enc(f | Alternates, ch('a', Mod_Shift)), std::string("\x1b[97:65;2;65u"));
    // Text with no key (spec: key number 0).
    KeyEvent t;
    t.text = "\xc3\xa5";
    CHECK_EQ(enc(f, t), std::string("\x1b[0;;229u"));
    t.text = "hi";
    CHECK_EQ(enc(f, t), std::string("\x1b[0;;104:105u"));
    CHECK_EQ(enc(AllKeys, t), std::string("hi"));  // documented deviation: no information-free CSI 0 u
    CHECK_EQ(enc(Disambiguate, t), std::string("hi"));
    // Flag 16 is only defined together with 8.
    CHECK_EQ(enc(Disambiguate | Text, ch('a')), std::string("a"));
}

void keypad_without_disambiguate() {
    // Without flags 1 / 8 the keypad reports as the main-block keys (kitty).
    CHECK_EQ(enc(Events, fk(Key::KpEnter)), std::string("\r"));
    CHECK_EQ(enc(Events, fk(Key::KpUp)), std::string("\x1b[A"));
    CHECK_EQ(enc(Events, fk(Key::Kp1)), std::string("1"));
    CHECK_EQ(enc(Events, fk(Key::Kp1, Mod_Ctrl)), std::string("1"));  // kitty's ctrl table leaves digits alone
    CHECK_EQ(enc(Alternates, fk(Key::KpHome), true), std::string("\x1bOH"));
}

void probe_cases() {
    // Every kitty case from the former probe_keys.cpp (flag 1, release with flag 2).
    CHECK_EQ(enc(Disambiguate, fk(Key::Up)), std::string("\x1b[A"));
    CHECK_EQ(enc(Disambiguate, fk(Key::Up, Mod_Shift)), std::string("\x1b[1;2A"));
    CHECK_EQ(enc(Disambiguate, fk(Key::Left, Mod_Ctrl)), std::string("\x1b[1;5D"));
    CHECK_EQ(enc(Disambiguate, fk(Key::Insert)), std::string("\x1b[2~"));
    CHECK_EQ(enc(Disambiguate, fk(Key::Delete)), std::string("\x1b[3~"));
    CHECK_EQ(enc(Disambiguate, fk(Key::PageUp)), std::string("\x1b[5~"));
    CHECK_EQ(enc(Disambiguate, fk(Key::PageDown)), std::string("\x1b[6~"));
    CHECK_EQ(enc(Disambiguate, fk(Key::Home)), std::string("\x1b[H"));
    CHECK_EQ(enc(Disambiguate, fk(Key::End)), std::string("\x1b[F"));
    CHECK_EQ(enc(Disambiguate, fk(Key::F1)), std::string("\x1b[P"));
    CHECK_EQ(enc(Disambiguate, fk(Key::F3)), std::string("\x1b[13~"));
    CHECK_EQ(enc(Disambiguate, fk(Key::F5)), std::string("\x1b[15~"));
    CHECK_EQ(enc(Disambiguate, fk(Key::F12)), std::string("\x1b[24~"));
    CHECK_EQ(enc(Disambiguate, fk(Key::Escape)), std::string("\x1b[27u"));
    CHECK_EQ(enc(Disambiguate, ch('a')), std::string("a"));
    CHECK_EQ(enc(Disambiguate, fk(Key::Enter)), std::string("\r"));
    CHECK_EQ(enc(Disambiguate, fk(Key::Tab)), std::string("\t"));
    CHECK_EQ(enc(Disambiguate, fk(Key::Backspace)), std::string("\x7f"));
    CHECK_EQ(enc(Disambiguate, ch('a', Mod_Ctrl)), std::string("\x1b[97;5u"));
    CHECK_EQ(enc(Disambiguate, ch('a', Mod_Alt)), std::string("\x1b[97;3u"));
    CHECK_EQ(enc(Disambiguate, ch('A', Mod_Ctrl | Mod_Shift)), std::string("\x1b[97;6u"));
    CHECK_EQ(enc(Disambiguate, fk(Key::Enter, Mod_Ctrl)), std::string("\x1b[13;5u"));
    CHECK_EQ(enc(Disambiguate, fk(Key::Tab, Mod_Shift)), std::string("\x1b[9;2u"));
    CHECK_EQ(enc(Disambiguate | Events, ch('a', Mod_Ctrl, KeyAction::Release)), std::string("\x1b[97;5:3u"));
    CHECK_EQ(enc(Disambiguate, ch(' ', Mod_Ctrl)), std::string("\x1b[32;5u"));
    // The private-use numbers belong to the spec's keys, not to Insert / F1.
    CHECK_EQ(uint32_t(Key::CapsLock), 57358u);
    CHECK_EQ(uint32_t(Key::F13), 57376u);
    CHECK_EQ(uint32_t(Key::Kp0), 57399u);
}

} // namespace

int main() {
    init_test();
    functional_table();
    modifiers();
    disambiguate();
    event_types();
    alternate_keys();
    report_all();
    associated_text();
    keypad_without_disambiguate();
    probe_cases();
    return check::finish("test_keys_kitty");
}
