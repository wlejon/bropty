#pragma once
// Cell styling. Every cell carries a 32-bit style id that indexes the
// terminal's StyleTable; identical styles share one id, so a screen of
// default text costs nothing beyond the id. Hyperlinks are part of the style
// (a link is just another attribute of a run of cells).

#include "bropty/color.h"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace bropty {

enum StyleAttr : uint16_t {
    Attr_Bold = 1 << 0,
    Attr_Dim = 1 << 1,
    Attr_Italic = 1 << 2,
    Attr_Blink = 1 << 3,
    Attr_RapidBlink = 1 << 4,
    Attr_Inverse = 1 << 5,
    Attr_Invisible = 1 << 6,
    Attr_Strike = 1 << 7,
    Attr_Overline = 1 << 8,
};

enum class Underline : uint8_t { None = 0, Single, Double, Curly, Dotted, Dashed };

struct Style {
    Color fg;
    Color bg;
    Color underline_color;
    uint16_t attrs{0};
    Underline underline{Underline::None};
    uint8_t reserved{0};
    uint32_t link{0};  // hyperlink id, 0 = none (Terminal::hyperlink(id))

    [[nodiscard]] bool has(StyleAttr a) const noexcept { return (attrs & a) != 0; }
    [[nodiscard]] bool is_default() const noexcept { return *this == Style{}; }
    bool operator==(const Style&) const noexcept = default;
};
static_assert(sizeof(Style) == 20);

struct StyleHash {
    size_t operator()(const Style& s) const noexcept {
        uint64_t h = s.fg.packed();
        h = h * 0x9E3779B97F4A7C15ull ^ s.bg.packed();
        h = h * 0x9E3779B97F4A7C15ull ^ s.underline_color.packed();
        h = h * 0x9E3779B97F4A7C15ull ^ (uint64_t(s.attrs) | (uint64_t(s.underline) << 16));
        h = h * 0x9E3779B97F4A7C15ull ^ s.link;
        h ^= h >> 29;
        h *= 0xBF58476D1CE4E5B9ull;  // finalise: the table indexes by the low bits
        return size_t(h ^ (h >> 32));
    }
};

// Interning table. Id 0 is always the default style. Ids are stable until a
// sweep; sweeps run only at safe points (end of Terminal::feed / resize), so
// ids read from cells stay valid for the duration of a frame.
//
// The index is an open-addressed (linear probing) table of {hash, id} slots
// kept at most half full; a lookup compares 32 hash bits before touching the
// Style. Sweeps rebuild it, so it never carries tombstones.
class StyleTable {
public:
    StyleTable();

    uint32_t intern(const Style& s);
    [[nodiscard]] const Style& get(uint32_t id) const noexcept { return styles_[id]; }
    [[nodiscard]] const Style* data() const noexcept { return styles_.data(); }
    [[nodiscard]] size_t live() const noexcept { return styles_.size() - free_.size(); }

    // True when enough garbage may have accumulated to make a sweep worthwhile.
    [[nodiscard]] bool wants_sweep() const noexcept { return live() > sweep_threshold_; }
    // Mark-and-sweep: `marked[id]` true keeps the style. Unmarked ids are freed.
    void sweep(const std::vector<uint8_t>& marked);
    [[nodiscard]] size_t capacity_ids() const noexcept { return styles_.size(); }
    void clear();

private:
    void index_insert(uint32_t hash, uint32_t id);
    void rebuild_index(size_t slots);

    std::vector<Style> styles_;
    std::vector<uint32_t> free_;
    std::vector<uint8_t> in_use_;
    std::vector<uint64_t> index_;  // (hash << 32) | id, 0 = empty (id 0 is never indexed)
    size_t indexed_{0};
    size_t sweep_threshold_{4096};
};

} // namespace bropty
