#pragma once

#include "bropty/color.h"
#include <cstdint>

namespace bropty {

enum CellFlags : uint16_t {
    CellFlag_None             = 0,
    CellFlag_Bold             = 1 << 0,
    CellFlag_Dim              = 1 << 1,
    CellFlag_Italic           = 1 << 2,
    CellFlag_UnderlineSingle  = 1 << 3,
    CellFlag_UnderlineDouble  = 1 << 4,
    CellFlag_UnderlineCurly   = 1 << 5,
    CellFlag_UnderlineDotted  = 1 << 6,
    CellFlag_UnderlineDashed  = 1 << 7,
    CellFlag_Blink            = 1 << 8,
    CellFlag_Inverse          = 1 << 9,
    CellFlag_Hidden           = 1 << 10,
    CellFlag_Strikethrough    = 1 << 11,
    CellFlag_WideLead         = 1 << 12, // First column of a 2-column wide character
    CellFlag_WideTrail        = 1 << 13  // Second column spacer of a 2-column character
};

constexpr uint16_t CellFlag_UnderlineMask =
    CellFlag_UnderlineSingle |
    CellFlag_UnderlineDouble |
    CellFlag_UnderlineCurly  |
    CellFlag_UnderlineDotted |
    CellFlag_UnderlineDashed;

struct Cell {
    uint32_t codepoint{' '};
    Color fg{Color::default_color()};
    Color bg{Color::default_color()};
    Color underline_color{Color::default_color()};
    uint16_t flags{CellFlag_None};
    uint16_t hyperlink_id{0}; // 0 = none; non-zero references terminal's hyperlink table
    uint8_t width{1};         // 1, 2, or 0 (spacer/combining)

    constexpr Cell() noexcept = default;

    constexpr explicit Cell(uint32_t cp, Color f = Color::default_color(), Color b = Color::default_color(), uint16_t fl = CellFlag_None) noexcept
        : codepoint(cp), fg(f), bg(b), flags(fl) {}

    void reset() noexcept {
        codepoint = ' ';
        fg = Color::default_color();
        bg = Color::default_color();
        underline_color = Color::default_color();
        flags = CellFlag_None;
        hyperlink_id = 0;
        width = 1;
    }

    [[nodiscard]] constexpr bool has_flag(CellFlags flag) const noexcept {
        return (flags & static_cast<uint16_t>(flag)) != 0;
    }

    void set_flag(CellFlags flag, bool enable = true) noexcept {
        if (enable) {
            flags |= static_cast<uint16_t>(flag);
        } else {
            flags &= ~static_cast<uint16_t>(flag);
        }
    }

    [[nodiscard]] constexpr bool is_underlined() const noexcept {
        return (flags & CellFlag_UnderlineMask) != 0;
    }

    [[nodiscard]] constexpr bool is_empty() const noexcept {
        return (codepoint == ' ' || codepoint == 0) &&
               fg.is_default() &&
               bg.is_default() &&
               flags == CellFlag_None &&
               hyperlink_id == 0;
    }

    constexpr bool operator==(const Cell& other) const noexcept {
        return codepoint == other.codepoint &&
               fg == other.fg &&
               bg == other.bg &&
               underline_color == other.underline_color &&
               flags == other.flags &&
               hyperlink_id == other.hyperlink_id &&
               width == other.width;
    }

    constexpr bool operator!=(const Cell& other) const noexcept {
        return !(*this == other);
    }
};

} // namespace bropty
