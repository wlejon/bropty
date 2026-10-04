#pragma once

// Unicode 17.0.0 terminal cell widths and UAX #29 extended grapheme clusters.
//
// Every per-code-point query is a multi-stage table lookup generated from the UCD by
// tools/gen_unicode.cpp into src/unicode_tables.inc (three dependent loads, no branches on
// the data). This sits on the hot print path of the terminal.
//
// Width rules (width()):
//   * C0/C1 controls and DEL                                                       -> 0
//     (the parser never prints them; is_zero_width() is false for them)
//   * General_Category Mn, Me and Cf                                               -> 0
//       except U+00AD SOFT HYPHEN                                                  -> 1
//       and the prepended concatenation marks U+0600..0605, U+06DD, U+070F,
//       U+0890..0891, U+08E2, U+110BD, U+110CD (visible, they span what follows)    -> 1
//     (this covers ZWJ, ZWNJ, U+200B, the bidi controls, variation selectors, tags)
//   * Default_Ignorable_Code_Point                                                 -> 0
//       except U+00AD (above), and U+115F HANGUL CHOSEONG FILLER, which stays 2: it is Grapheme_Cluster_Break=L
//       and stands in for the leading consonant of an L+V(+T) syllable, so the syllable
//       keeps its double width.
//   * Hangul medial vowels and final consonants U+1160..11FF, U+D7B0..D7FF         -> 0
//     (they compose onto the leading jamo's cell)
//   * Regional indicators U+1F1E6..1F1FF alone                                     -> 1
//   * East_Asian_Width W or F (including the UCD's default-W unassigned CJK ranges and
//     all of planes 2 and 3), or Emoji_Presentation=Yes                            -> 2
//   * East_Asian_Width A                                       -> 2 if ambiguous_wide, else 1
//   * everything else, including unassigned code points, private use, spacing marks
//     (Mc), Zl/Zp and surrogates                                                   -> 1
// Code points above U+10FFFF are treated like unassigned ones (width 1, Other).
//
// Cluster width rules (cluster_width()), the semantics of grapheme-aware terminals
// (Ghostty, kitty, WezTerm, mode 2027):
//   * the width of the first code point, with these overrides:
//   * a regional-indicator pair (a flag)                                           -> 2
//   * an Emoji=Yes base followed directly by VS16 U+FE0F                           -> 2
//   * an Emoji=Yes base followed directly by VS15 U+FE0E                           -> 1
//   * an Emoji=Yes base followed directly by an emoji modifier U+1F3FB..1F3FF      -> 2
//   * an Extended_Pictographic base that is part of an emoji ZWJ sequence (GB11)   -> 2
//     (ZWJ sequences render in emoji presentation even when their first pictograph
//     defaults to text presentation)
//   * Hangul L+V(+T) jamo sequences take the width of the leading jamo             -> 2
//   * a cluster whose first code point is zero-width:
//       - if it is Grapheme_Cluster_Break=Control (an invisible format character such as
//         U+200B, U+2060, U+FEFF or a bidi mark; these are always clusters of their own) -> 0
//       - otherwise (a combining sequence with no base: a lone combining mark, a stray
//         ZWJ or variation selector, a lone medial jamo) the renderer draws it on a dotted
//         circle / as a standalone glyph                                           -> 1
//   * an empty cluster -> 0; a control (C0/C1/DEL) -> 0.

#include <cstdint>
#include <string_view>

namespace bropty::unicode {

// Version of the UCD the tables were generated from.
inline constexpr int kUnicodeMajor = 17;
inline constexpr int kUnicodeMinor = 0;
inline constexpr int kUnicodeUpdate = 0;

// Terminal column width of one code point: 0, 1 or 2 (see the rules above).
[[nodiscard]] int width(char32_t cp, bool ambiguous_wide = false) noexcept;

// width(cp) == 0 and printable (combining marks, ZWJ, VS, Cf, ...). False for controls.
[[nodiscard]] bool is_zero_width(char32_t cp) noexcept;

// East_Asian_Width=A (and not zero-width under the rules above).
[[nodiscard]] bool is_ambiguous(char32_t cp) noexcept;

[[nodiscard]] bool is_extended_pictographic(char32_t cp) noexcept;

// Emoji=Yes: a base that VS16 widens to emoji presentation.
[[nodiscard]] bool is_emoji(char32_t cp) noexcept;

// Emoji_Modifier (the Fitzpatrick skin tones U+1F3FB..1F3FF).
[[nodiscard]] constexpr bool is_emoji_modifier(char32_t cp) noexcept {
    return cp >= 0x1F3FB && cp <= 0x1F3FF;
}

[[nodiscard]] constexpr bool is_regional_indicator(char32_t cp) noexcept {
    return cp >= 0x1F1E6 && cp <= 0x1F1FF;
}

enum class GraphemeBreak : uint8_t {
    Other, CR, LF, Control, Extend, ZWJ, RegionalIndicator, Prepend, SpacingMark,
    L, V, T, LV, LVT
};

[[nodiscard]] GraphemeBreak grapheme_break(char32_t cp) noexcept;

enum class IndicConjunctBreak : uint8_t { None, Linker, Consonant, Extend };

[[nodiscard]] IndicConjunctBreak indic_conjunct_break(char32_t cp) noexcept;

// Incremental UAX #29 extended-grapheme-cluster segmenter implementing every rule
// (GB3..GB13, GB999), including GB9c Indic conjuncts, GB11 emoji ZWJ sequences and
// GB12/GB13 regional-indicator pairing. A small trivially-copyable value type: the
// terminal keeps one per "last printed cell" and resumes it when the next code point
// arrives.
class GraphemeSegmenter {
public:
    // Returns true if there is a cluster boundary BEFORE cp (always true for the first
    // code point after construction or reset()).
    bool next(char32_t cp) noexcept;
    void reset() noexcept { *this = GraphemeSegmenter{}; }

private:
    uint8_t prev_ = 0xFF;   // GraphemeBreak of the previous code point; 0xFF = start
    uint8_t emoji_ = 0;     // GB11: 0 none, 1 = ExtPict Extend*, 2 = ExtPict Extend* ZWJ
    uint8_t incb_ = 0;      // GB9c: 0 none, 1 = Consonant [Extend|Linker]*, 2 = ... Linker seen
    uint8_t ri_odd_ = 0;    // GB12/13: odd number of regional indicators precede
};

// Width of a whole extended grapheme cluster (see the cluster rules above).
[[nodiscard]] int cluster_width(std::u32string_view cluster, bool ambiguous_wide = false) noexcept;

} // namespace bropty::unicode
