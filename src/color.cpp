#include "bropty/color.h"
#include <array>

namespace bropty {

namespace {

constexpr std::array<Rgb, 16> kStandardAnsiPalette = {{
    // 0-7: Standard
    {0, 0, 0},         // 0: Black
    {205, 49, 49},     // 1: Red
    {13, 188, 121},    // 2: Green
    {229, 229, 16},    // 3: Yellow
    {36, 114, 200},    // 4: Blue
    {188, 63, 188},    // 5: Magenta
    {17, 168, 205},    // 6: Cyan
    {229, 229, 229},   // 7: White
    // 8-15: Bright
    {102, 102, 102},   // 8: Bright Black (Gray)
    {241, 76, 76},     // 9: Bright Red
    {35, 209, 139},    // 10: Bright Green
    {245, 245, 67},    // 11: Bright Yellow
    {59, 142, 234},    // 12: Bright Blue
    {214, 112, 214},   // 13: Bright Magenta
    {41, 184, 219},    // 14: Bright Cyan
    {255, 255, 255}    // 15: Bright White
}};

// Precomputed 256 color table
constexpr auto make_256_palette() {
    std::array<Rgb, 256> table{};
    for (size_t i = 0; i < 16; ++i) {
        table[i] = kStandardAnsiPalette[i];
    }
    // 16..231: 6x6x6 color cube
    constexpr uint8_t steps[6] = {0, 95, 135, 175, 215, 255};
    size_t idx = 16;
    for (uint8_t r = 0; r < 6; ++r) {
        for (uint8_t g = 0; g < 6; ++g) {
            for (uint8_t b = 0; b < 6; ++b) {
                table[idx++] = Rgb{steps[r], steps[g], steps[b]};
            }
        }
    }
    // 232..255: 24 grayscale steps from 8 to 238
    for (uint8_t i = 0; i < 24; ++i) {
        uint8_t v = static_cast<uint8_t>(8 + i * 10);
        table[idx++] = Rgb{v, v, v};
    }
    return table;
}

constexpr auto kPalette256 = make_256_palette();

} // namespace

Rgb Color::default_fg_rgb() noexcept {
    return Rgb{204, 204, 204}; // #CCCCCC
}

Rgb Color::default_bg_rgb() noexcept {
    return Rgb{12, 12, 12}; // #0C0C0C
}

Rgb Color::get_indexed_rgb(uint8_t index) noexcept {
    return kPalette256[index];
}

Rgb Color::resolve(bool is_fg, const Rgb* custom_palette_256) const noexcept {
    switch (type_) {
        case ColorType::Default:
            return is_fg ? default_fg_rgb() : default_bg_rgb();
        case ColorType::Indexed:
            if (custom_palette_256) {
                return custom_palette_256[index_];
            }
            return kPalette256[index_];
        case ColorType::Rgb:
            return rgb_;
    }
    return is_fg ? default_fg_rgb() : default_bg_rgb();
}

} // namespace bropty
