#pragma once
// Colors as stored in cells (4 bytes, unresolved) and the palette that
// resolves them. A Color is either the terminal default, one of the 256
// palette slots, or a direct 24-bit RGB value; resolution happens at render
// time against the terminal's live Palette (OSC 4/10/11/12 can change it).

#include <array>
#include <cstdint>

namespace bropty {

struct Rgb {
    uint8_t r{0};
    uint8_t g{0};
    uint8_t b{0};

    constexpr bool operator==(const Rgb&) const noexcept = default;
    [[nodiscard]] constexpr uint32_t to_u32() const noexcept {
        return (uint32_t(r) << 16) | (uint32_t(g) << 8) | uint32_t(b);
    }
};

class Color {
public:
    enum class Kind : uint8_t { Default = 0, Indexed = 1, Rgb = 2 };

    constexpr Color() noexcept = default;

    static constexpr Color default_color() noexcept { return Color(); }
    static constexpr Color indexed(uint8_t idx) noexcept { return Color(Kind::Indexed, idx, 0, 0); }
    static constexpr Color rgb(uint8_t r, uint8_t g, uint8_t b) noexcept { return Color(Kind::Rgb, r, g, b); }

    [[nodiscard]] constexpr Kind kind() const noexcept { return kind_; }
    [[nodiscard]] constexpr bool is_default() const noexcept { return kind_ == Kind::Default; }
    [[nodiscard]] constexpr bool is_indexed() const noexcept { return kind_ == Kind::Indexed; }
    [[nodiscard]] constexpr bool is_rgb() const noexcept { return kind_ == Kind::Rgb; }
    [[nodiscard]] constexpr uint8_t index() const noexcept { return a_; }
    [[nodiscard]] constexpr Rgb rgb_value() const noexcept { return Rgb{a_, b_, c_}; }

    // Packed form, used for hashing and for the compact scrollback encoding.
    [[nodiscard]] constexpr uint32_t packed() const noexcept {
        return (uint32_t(kind_) << 24) | (uint32_t(a_) << 16) | (uint32_t(b_) << 8) | uint32_t(c_);
    }
    static constexpr Color from_packed(uint32_t v) noexcept {
        return Color(Kind((v >> 24) & 3), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v));
    }

    constexpr bool operator==(const Color&) const noexcept = default;

private:
    constexpr Color(Kind k, uint8_t a, uint8_t b, uint8_t c) noexcept : kind_(k), a_(a), b_(b), c_(c) {}
    Kind kind_{Kind::Default};
    uint8_t a_{0};
    uint8_t b_{0};
    uint8_t c_{0};
};
static_assert(sizeof(Color) == 4);

// The live color table: 256 indexed colors plus the special defaults.
struct Palette {
    std::array<Rgb, 256> colors{};
    Rgb foreground{204, 204, 204};
    Rgb background{12, 12, 12};
    Rgb cursor{204, 204, 204};

    // xterm's 256-color table with a neutral 16-color base.
    static Palette standard() noexcept;
    static Rgb standard_index(uint8_t index) noexcept;

    [[nodiscard]] Rgb resolve_fg(Color c) const noexcept {
        return c.is_rgb() ? c.rgb_value() : c.is_indexed() ? colors[c.index()] : foreground;
    }
    [[nodiscard]] Rgb resolve_bg(Color c) const noexcept {
        return c.is_rgb() ? c.rgb_value() : c.is_indexed() ? colors[c.index()] : background;
    }
};

} // namespace bropty
