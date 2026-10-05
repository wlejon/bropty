#pragma once
// Input encoding: keyboard, mouse, paste and focus events turned into the
// bytes an application reads from its terminal, according to the modes the
// application has set. Everything here is pure (no Terminal, no PTY): the
// caller passes the relevant mode state as a plain struct, so every encoding
// is testable in isolation. Session (session.h) builds that state from
// Terminal::modes() / kitty_keyboard_flags() and writes the result to the PTY.
//
// Keyboard
//   kitty_flags != 0  -> the kitty keyboard protocol
//                        (sw.kovidgoyal.net/kitty/keyboard-protocol), all five
//                        progressive-enhancement flags, following kitty's own
//                        encoder (kitty/key_encoding.c) where the prose is silent.
//   kitty_flags == 0  -> xterm: DECCKM / DECKPAM / DECBKM, CSI 1;m forms for
//                        modified functional keys, the C0 ctrl mapping, Alt as
//                        an ESC prefix or (with ?1036/?1039 off) as the 8th bit,
//                        and modifyOtherKeys levels 1 and 2 (CSI 27;m;code ~).
//
// Choices worth knowing (see the .cpp files for the rest):
//   * Text keys report their *unshifted* code point in `codepoint` ('a' for
//     shift+a, '1' for shift+1); `shifted` is the shifted one ('A', '!'). An
//     upper-case ASCII letter in `codepoint` is normalised to lower case. For
//     ASCII, a missing `shifted` is derived from the US layout.
//   * `text` is what the key types (after layout, shift, caps lock, dead keys).
//     It is only used when no ctrl/alt/super/hyper/meta modifier is held: if
//     the platform consumed a modifier to produce text (macOS Option+a -> å),
//     clear that modifier in `mods`. Control characters are never sent as text.
//   * Lock modifiers (Mod_CapsLock / Mod_NumLock) are never encoded in legacy
//     mode. In kitty mode they are reported as kitty does: never for text that
//     is sent as text, on functional keys and CSI u forms otherwise. NumLock
//     also selects numeric keypad output under DECKPAM (xterm's realNumLock).
//   * For a modifier key event (Key::LeftShift ... ), `mods` must already
//     include the effect of this event (press sets the bit, release of the
//     last held key clears it), as the kitty spec requires.
//   * Releases are never encoded in legacy mode, nor in kitty mode without the
//     report-event-types flag. A release under that flag is always a CSI form
//     (kitty itself re-sends a bare ESC on Escape release under flag 2 alone;
//     bropty reports CSI 27;1:3u instead).

#include "bropty/terminal.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace bropty {

// Modifier bits, in kitty protocol order (the kitty modifier parameter is
// 1 + these bits).
enum KeyMod : uint16_t {
    Mod_None     = 0,
    Mod_Shift    = 1 << 0,
    Mod_Alt      = 1 << 1,
    Mod_Ctrl     = 1 << 2,
    Mod_Super    = 1 << 3,
    Mod_Hyper    = 1 << 4,
    Mod_Meta     = 1 << 5,
    Mod_CapsLock = 1 << 6,
    Mod_NumLock  = 1 << 7,
};
using KeyMods = uint16_t;
inline constexpr KeyMods Mod_LockMask = Mod_CapsLock | Mod_NumLock;

// Functional (non-text) keys. The values are kitty's key numbers: the
// private-use code points from the spec's functional key table (CAPS_LOCK =
// 57358, F13 = 57376, KP_0 = 57399, ...), plus kitty's internal numbers for
// the keys the spec encodes in legacy form (ESCAPE ... END, F1 ... F12).
enum class Key : uint32_t {
    None = 0,
    Escape = 57344, Enter, Tab, Backspace, Insert, Delete,
    Left, Right, Up, Down, PageUp, PageDown, Home, End,
    CapsLock = 57358, ScrollLock, NumLock, PrintScreen, Pause, Menu,
    F1 = 57364, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    F13 = 57376, F14, F15, F16, F17, F18, F19, F20, F21, F22, F23, F24,
    F25, F26, F27, F28, F29, F30, F31, F32, F33, F34, F35,
    Kp0 = 57399, Kp1, Kp2, Kp3, Kp4, Kp5, Kp6, Kp7, Kp8, Kp9,
    KpDecimal = 57409, KpDivide, KpMultiply, KpSubtract, KpAdd, KpEnter, KpEqual,
    KpSeparator, KpLeft, KpRight, KpUp, KpDown, KpPageUp, KpPageDown, KpHome,
    KpEnd, KpInsert, KpDelete, KpBegin,
    MediaPlay = 57428, MediaPause, MediaPlayPause, MediaReverse, MediaStop,
    MediaFastForward, MediaRewind, MediaTrackNext, MediaTrackPrevious, MediaRecord,
    LowerVolume, RaiseVolume, MuteVolume,
    LeftShift = 57441, LeftControl, LeftAlt, LeftSuper, LeftHyper, LeftMeta,
    RightShift, RightControl, RightAlt, RightSuper, RightHyper, RightMeta,
    IsoLevel3Shift, IsoLevel5Shift,
};

enum class KeyAction : uint8_t { Press = 1, Repeat = 2, Release = 3 };

struct KeyEvent {
    // Exactly one of `key` (functional key) and `codepoint` (text key) is set;
    // both empty with non-empty `text` is a pure text event (IME commit).
    Key key{Key::None};
    char32_t codepoint{0};    // unshifted key, e.g. 'a', '1', ' ', 0x441 (Cyrillic es)
    char32_t shifted{0};      // shifted key in the active layout, 0 = unknown / derive
    char32_t base_layout{0};  // the key in the PC-101 US layout ('c' for Cyrillic es), 0 = none
    KeyMods mods{Mod_None};
    KeyAction action{KeyAction::Press};
    std::string text;         // UTF-8 text the key produces, may be empty

    static KeyEvent functional(Key k, KeyMods m = Mod_None, KeyAction a = KeyAction::Press) {
        KeyEvent e;
        e.key = k;
        e.mods = m;
        e.action = a;
        return e;
    }
    static KeyEvent character(char32_t cp, KeyMods m = Mod_None, KeyAction a = KeyAction::Press) {
        KeyEvent e;
        e.codepoint = cp;
        e.mods = m;
        e.action = a;
        return e;
    }
};

// The terminal state that shapes key encoding.
struct KeyboardModes {
    uint32_t kitty_flags{0};          // CSI = / > / < u (current screen's stack top)
    bool app_cursor_keys{false};      // DECCKM (?1)
    bool app_keypad{false};           // DECKPAM / DECNKM (?66)
    bool backarrow_sends_bs{false};   // DECBKM (?67)
    int modify_other_keys{0};         // XTMODKEYS 4 (CSI > 4 ; Pv m): 0, 1, 2
    // xterm's modifyCursorKeys / modifyFunctionKeys / modifyKeypadKeys (XTMODKEYS
    // 1, 2, 3) for a modified key: -1 drops the modifier; 0 puts it first
    // (SS3 5 A, SS3 5 P, CSI 5 ~ forms keep their type); 1 forces CSI (CSI 5 A);
    // 2 makes it the second parameter (CSI 1;5 A); 3 also marks the sequence
    // private (CSI > 1;5 A). Tilde keys already carry the modifier second, so
    // 0..2 all give CSI n;m ~ and 3 gives CSI > n;m ~.
    int modify_cursor_keys{2};        // arrows, Home, End, KP_Begin
    int modify_function_keys{2};      // F1-F12, editing keys, Menu
    int modify_keypad_keys{0};        // the DECKPAM application keypad
    int format_other_keys{0};         // XTFMTKEYS 4: 1 sends modifyOtherKeys as CSI code;m u
    bool alt_sends_escape{true};      // ?1039
    bool meta_sends_escape{true};     // ?1036

    static KeyboardModes from(const Terminal& t) noexcept;
};

// Bytes for one key event; empty when the event produces nothing.
[[nodiscard]] std::string encode_key(const KeyEvent& ev, const KeyboardModes& modes);

// ---- mouse -----------------------------------------------------------------

enum class MouseButton : uint8_t {
    None,  // motion with no button held
    Left, Middle, Right,
    WheelUp, WheelDown, WheelLeft, WheelRight,
    Button8, Button9, Button10, Button11,  // back, forward, ...
};

enum class MouseAction : uint8_t { Press, Release, Motion };

struct MouseEvent {
    MouseAction action{MouseAction::Press};
    MouseButton button{MouseButton::Left};  // for Motion: ignored by MouseReporter (it tracks held buttons)
    KeyMods mods{Mod_None};  // Shift / Alt (reported as meta) / Ctrl
    int col{0};  // 0-based cell
    int row{0};
    int x{0};    // 0-based pixel position, for SGR-pixels (?1016)
    int y{0};
};

struct MouseModes {
    MouseTracking tracking{MouseTracking::None};
    MouseEncoding encoding{MouseEncoding::Default};
    static MouseModes from(const Terminal& t) noexcept;
};

// Stateless encoding of one report. `held` is the button reported for a
// Motion event (MouseButton::None = no button). Returns empty when the mode
// does not report this event or the position is not representable (default
// encoding beyond column/row 223, UTF-8 beyond 2015): the event is dropped.
[[nodiscard]] std::string encode_mouse(const MouseEvent& ev, const MouseModes& modes,
                                       MouseButton held = MouseButton::None);

// Stateful reporter: tracks held buttons (the button a motion report carries,
// and whether button-event tracking reports motion at all) and suppresses
// motion that stays in the same cell (the same pixel under SGR-pixels).
class MouseReporter {
public:
    [[nodiscard]] std::string encode(const MouseEvent& ev, const MouseModes& modes);
    void reset() noexcept;
    [[nodiscard]] MouseButton held_button() const noexcept;

private:
    uint16_t held_{0};  // bit per MouseButton
    bool have_last_{false};
    int last_x_{0};
    int last_y_{0};
};

// ---- paste / focus -----------------------------------------------------------

// Paste. Line breaks (CRLF, lone LF) become CR in both modes, as xterm does.
// Without bracketed paste nothing else changes. With it, the text is wrapped in
// CSI 200~ ... CSI 201~ and cannot end the paste early: every C0 control except
// HT/LF/CR (so every ESC), DEL, every C1 control (U+0080..U+009F) and every
// byte that is not well-formed UTF-8 (replaced by U+FFFD, so no stray 0x9b) is
// removed, which makes an embedded ESC [ 201 ~ impossible however it is split.
[[nodiscard]] std::string encode_paste(std::string_view text, bool bracketed);

// Focus in / out report (?1004); empty when the mode is off.
[[nodiscard]] std::string encode_focus(bool focused, bool focus_events);

} // namespace bropty
