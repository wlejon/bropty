#pragma once
// Internal: the kitty graphics protocol's Unicode placeholders. A cell holding
// U+10EEEE followed by up to three combining marks from kitty's
// rowcolumn-diacritics table (row, column, the image id's high byte) shows
// part of a virtual placement; its foreground color carries the image id's
// low 24 bits and its underline color the placement id.

#include <cstdint>

namespace bropty::detail {

constexpr int kKittyDiacritics = 297;

// 1-based number of a row/column diacritic, 0 when `cp` is not one.
[[nodiscard]] uint32_t kitty_diacritic_number(char32_t cp) noexcept;
// The diacritic for number n (1-based), 0 when out of range.
[[nodiscard]] char32_t kitty_diacritic(uint32_t n) noexcept;

} // namespace bropty::detail
