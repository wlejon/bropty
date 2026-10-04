#pragma once
// The cell and row model a renderer reads every frame.
//
// A Cell is 8 bytes: the first code point of its grapheme cluster (21 bits),
// its width role, a "has more code points" bit and the DECSCA protection bit,
// plus a 32-bit style id. Clusters longer than one code point keep their tail
// in the row's ClusterMap, which exists only for rows that need it.
//
// Wide (2-column) clusters occupy a Wide lead cell followed by a SpacerTail.
// A SpacerHead marks the last column of a soft-wrapped row that was left empty
// because the next wide cluster did not fit; it is not content (reflow drops it).

#include "bropty/style.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bropty {

enum class Wide : uint8_t { Narrow = 0, Lead = 1, SpacerTail = 2, SpacerHead = 3 };

struct Cell {
    uint32_t bits{0};   // [0,21) code point, [21,23) Wide, 23 cluster, 24 protected
    uint32_t style{0};  // StyleTable id (or a row-local palette index for history rows)

    static constexpr uint32_t kCpMask = 0x1FFFFF;
    static constexpr uint32_t kClusterBit = 1u << 23;
    static constexpr uint32_t kProtectedBit = 1u << 24;

    static constexpr Cell make(char32_t cp, uint32_t style, Wide w = Wide::Narrow) noexcept {
        return Cell{(uint32_t(cp) & kCpMask) | (uint32_t(w) << 21), style};
    }
    static constexpr Cell blank(uint32_t style = 0) noexcept { return Cell{0, style}; }

    // 0 for an empty (never written / erased) cell and for spacers.
    [[nodiscard]] constexpr char32_t cp() const noexcept { return char32_t(bits & kCpMask); }
    [[nodiscard]] constexpr Wide wide() const noexcept { return Wide((bits >> 21) & 3); }
    [[nodiscard]] constexpr bool has_cluster() const noexcept { return (bits & kClusterBit) != 0; }
    [[nodiscard]] constexpr bool is_protected() const noexcept { return (bits & kProtectedBit) != 0; }
    [[nodiscard]] constexpr bool is_empty() const noexcept { return cp() == 0; }
    [[nodiscard]] constexpr bool is_spacer() const noexcept {
        return wide() == Wide::SpacerTail || wide() == Wide::SpacerHead;
    }
    // Columns this cell's content spans (2 for a wide lead, 0 for spacers).
    [[nodiscard]] constexpr int columns() const noexcept {
        Wide w = wide();
        return w == Wide::Lead ? 2 : (w == Wide::Narrow ? 1 : 0);
    }

    constexpr void set_cp(char32_t cp) noexcept { bits = (bits & ~kCpMask) | (uint32_t(cp) & kCpMask); }
    constexpr void set_wide(Wide w) noexcept { bits = (bits & ~(3u << 21)) | (uint32_t(w) << 21); }
    constexpr void set_cluster(bool on) noexcept { bits = on ? (bits | kClusterBit) : (bits & ~kClusterBit); }
    constexpr void set_protected(bool on) noexcept { bits = on ? (bits | kProtectedBit) : (bits & ~kProtectedBit); }

    constexpr bool operator==(const Cell&) const noexcept = default;
};
static_assert(sizeof(Cell) == 8);

// Extra code points of multi-code-point clusters in one row, keyed by column.
// Sparse: almost every row has none and never allocates one.
class ClusterMap {
public:
    [[nodiscard]] std::u32string_view find(int col) const noexcept;
    void set(int col, std::u32string_view tail);
    void append(int col, char32_t cp);
    void erase(int col);
    void erase_range(int first, int last);  // [first, last)
    // Re-key entries in [first, last) by +delta (entries moved out of range are the caller's concern).
    void shift(int first, int last, int delta);
    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
    void clear() noexcept { entries_.clear(); }
    [[nodiscard]] const std::vector<std::pair<uint16_t, std::u32string>>& entries() const noexcept { return entries_; }

private:
    std::vector<std::pair<uint16_t, std::u32string>> entries_;  // sorted by column
};

enum RowFlag : uint32_t {
    Row_Wrapped = 1u << 0,        // this row soft-wraps into the next one
    Row_Prompt = 1u << 1,         // OSC 133;A seen on this row (shell prompt start)
    Row_Input = 1u << 2,          // OSC 133;B (command input start)
    Row_Output = 1u << 3,         // OSC 133;C (command output start)
    Row_DoubleWidth = 1u << 4,    // DECDWL
    Row_DoubleTop = 1u << 5,      // DECDHL top half
    Row_DoubleBottom = 1u << 6,   // DECDHL bottom half
};
constexpr uint32_t Row_SemanticMask = Row_Prompt | Row_Input | Row_Output;
constexpr uint32_t Row_LineAttrMask = Row_DoubleWidth | Row_DoubleTop | Row_DoubleBottom;

// A read-only view of one row, valid until the terminal is next mutated.
// `styles[cell.style]` is the cell's style; for screen rows this is the
// terminal's StyleTable, for history rows a palette local to the decoded line.
struct RowView {
    const Cell* cells{nullptr};
    int cols{0};
    uint32_t flags{0};
    const Style* styles{nullptr};
    const ClusterMap* clusters{nullptr};

    [[nodiscard]] const Cell& operator[](int col) const noexcept { return cells[col]; }
    [[nodiscard]] const Style& style(int col) const noexcept { return styles[cells[col].style]; }
    [[nodiscard]] bool wrapped() const noexcept { return (flags & Row_Wrapped) != 0; }
    // The full cluster at `col` (empty for empty cells and spacers).
    [[nodiscard]] std::u32string cluster(int col) const;
    // UTF-8 text of the row; empty cells become spaces, spacers are skipped,
    // trailing spaces are trimmed when `trim` is set.
    [[nodiscard]] std::string text(bool trim = true) const;
};

void append_utf8(std::string& out, char32_t cp);

} // namespace bropty
