#pragma once
// Helpers shared by the key encoders (key_encoder.cpp, key_encoder_kitty.cpp,
// key_encoder_legacy.cpp). Not installed.

#include "bropty/input.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace bropty::input_detail {

void append_utf8(std::string& out, char32_t cp);
std::string utf8(char32_t cp);
// Decodes well-formed UTF-8; ill-formed bytes become U+FFFD (one per maximal subpart).
template <class F>
void for_each_code_point(std::string_view s, F&& f);

constexpr bool is_control(char32_t c) noexcept { return c < 0x20 || (c >= 0x7f && c < 0xa0); }

// US-layout shifted counterpart of an ASCII key, or 0.
char32_t us_shifted(char32_t cp) noexcept;
// The C0 byte ctrl produces for an ASCII key (the kitty spec's "legacy ctrl
// mapping", applied to upper-case letters too, as xterm does), or the key
// itself when ctrl does not map it.
char32_t ctrl_map(char32_t cp) noexcept;
// a-z 0-9 and the US punctuation keys (the spec's "legacy text keys"), plus
// their shifted forms and space (kitty's is_legacy_ascii_key).
bool is_legacy_ascii_key(char32_t cp) noexcept;

inline bool is_keypad(Key k) noexcept {
    return uint32_t(k) >= uint32_t(Key::Kp0) && uint32_t(k) <= uint32_t(Key::KpBegin);
}
inline bool is_modifier_key(Key k) noexcept {
    uint32_t v = uint32_t(k);
    return (v >= uint32_t(Key::LeftShift) && v <= uint32_t(Key::IsoLevel5Shift)) || k == Key::CapsLock ||
           k == Key::ScrollLock || k == Key::NumLock;
}
// The character a keypad key types with NumLock on (0 for navigation keys / Enter).
char32_t keypad_char(Key k) noexcept;
// Keypad navigation / Enter key -> the main-block key (KpUp -> Up), else Key::None.
Key keypad_to_normal(Key k) noexcept;

// A key event with the derived fields filled in.
struct NormalizedKey {
    Key key{Key::None};
    char32_t code{0};      // text key code point (lower case), 0 for functional keys
    char32_t shifted{0};
    char32_t base{0};      // base-layout key, 0 when absent or equal to `code`
    KeyMods mods{0};
    KeyAction action{KeyAction::Press};
    std::string text;      // text to type: control-free, empty for release or when a
                           // non-shift, non-lock modifier is held
};
NormalizedKey normalize(const KeyEvent& ev);

std::string encode_kitty(const NormalizedKey& k, const KeyboardModes& m);
std::string encode_legacy(const NormalizedKey& k, const KeyboardModes& m);

// ---- implementation of the template ----------------------------------------
template <class F>
void for_each_code_point(std::string_view s, F&& f) {
    size_t i = 0;
    const size_t n = s.size();
    while (i < n) {
        auto b0 = static_cast<unsigned char>(s[i]);
        if (b0 < 0x80) {
            f(char32_t(b0));
            ++i;
            continue;
        }
        int len = 0;
        char32_t cp = 0;
        unsigned char lo = 0x80, hi = 0xbf;
        if (b0 >= 0xc2 && b0 <= 0xdf) { len = 2; cp = b0 & 0x1f; }
        else if (b0 >= 0xe0 && b0 <= 0xef) {
            len = 3; cp = b0 & 0x0f;
            if (b0 == 0xe0) lo = 0xa0;
            if (b0 == 0xed) hi = 0x9f;
        } else if (b0 >= 0xf0 && b0 <= 0xf4) {
            len = 4; cp = b0 & 0x07;
            if (b0 == 0xf0) lo = 0x90;
            if (b0 == 0xf4) hi = 0x8f;
        } else {
            f(char32_t(0xfffd));
            ++i;
            continue;
        }
        size_t j = 1;
        for (; j < size_t(len); ++j) {
            if (i + j >= n) break;
            auto b = static_cast<unsigned char>(s[i + j]);
            if (b < lo || b > hi) break;
            cp = (cp << 6) | (b & 0x3f);
            lo = 0x80;
            hi = 0xbf;
        }
        if (j == size_t(len)) {
            f(cp);
            i += j;
        } else {
            f(char32_t(0xfffd));
            i += j;  // maximal subpart
        }
    }
}

} // namespace bropty::input_detail
