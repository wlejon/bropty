#include "bropty/color.h"

#include <cstddef>

namespace bropty {

namespace {

constexpr Rgb kBase16[16] = {
    {0, 0, 0},       {205, 49, 49},   {13, 188, 121},  {229, 229, 16},
    {36, 114, 200},  {188, 63, 188},  {17, 168, 205},  {229, 229, 229},
    {102, 102, 102}, {241, 76, 76},   {35, 209, 139},  {245, 245, 67},
    {59, 142, 234},  {214, 112, 214}, {41, 184, 219},  {255, 255, 255},
};

constexpr Rgb compute_index(uint8_t i) {
    if (i < 16) return kBase16[i];
    if (i < 232) {
        constexpr uint8_t steps[6] = {0, 95, 135, 175, 215, 255};
        int v = i - 16;
        return Rgb{steps[v / 36], steps[(v / 6) % 6], steps[v % 6]};
    }
    uint8_t g = uint8_t(8 + (i - 232) * 10);
    return Rgb{g, g, g};
}

} // namespace

Rgb Palette::standard_index(uint8_t index) noexcept { return compute_index(index); }

Palette Palette::standard() noexcept {
    Palette p;
    for (int i = 0; i < 256; ++i) p.colors[std::size_t(i)] = compute_index(uint8_t(i));
    return p;
}

} // namespace bropty
