#include "bropty/unicode.h"

namespace bropty::unicode {

namespace {

#include "unicode_tables.inc"

// Packed property layout; see tools/gen_unicode.cpp.
constexpr uint16_t kWidthMask = 0x3;
constexpr unsigned kGcbShift = 2;
constexpr uint16_t kGcbMask = 0xF;
constexpr uint16_t kExtPictBit = 1u << 6;
constexpr uint16_t kEmojiBit = 1u << 7;
constexpr unsigned kIncbShift = 8;
constexpr uint16_t kIncbMask = 0x3;
constexpr uint16_t kControlBit = 1u << 10;

constexpr uint16_t kWidthZero = 0;
constexpr uint16_t kWidthWide = 2;
constexpr uint16_t kWidthAmbiguous = 3;
// width_of() returns the field itself for every value but ambiguous.
static_assert(kWidthZero == 0 && kWidthWide == 2, "width field encodes the cell width directly");

inline uint16_t props(char32_t cp) noexcept {
    if (cp > 0x10FFFF) cp = 0x10FFFF;  // a noncharacter: width 1, Other, like unassigned
    return kProps[kStage2[(static_cast<unsigned>(kStage1[cp >> kShift]) << kShift) | (cp & kMask)]];
}

inline int width_of(uint16_t p, bool ambiguous_wide) noexcept {
    unsigned w = p & kWidthMask;
    if (w == kWidthAmbiguous) return ambiguous_wide ? 2 : 1;
    return static_cast<int>(w);
}

inline GraphemeBreak gcb_of(uint16_t p) noexcept {
    return static_cast<GraphemeBreak>((p >> kGcbShift) & kGcbMask);
}

inline IndicConjunctBreak incb_of(uint16_t p) noexcept {
    return static_cast<IndicConjunctBreak>((p >> kIncbShift) & kIncbMask);
}

} // namespace

int width(char32_t cp, bool ambiguous_wide) noexcept {
    // ASCII fast path: printable ASCII is always 1; the parser handles the rest itself.
    if (cp >= 0x20 && cp < 0x7F) return 1;
    return width_of(props(cp), ambiguous_wide);
}

bool is_zero_width(char32_t cp) noexcept {
    uint16_t p = props(cp);
    return (p & kWidthMask) == kWidthZero && !(p & kControlBit);
}

bool is_ambiguous(char32_t cp) noexcept { return (props(cp) & kWidthMask) == kWidthAmbiguous; }

bool is_extended_pictographic(char32_t cp) noexcept { return (props(cp) & kExtPictBit) != 0; }

bool is_emoji(char32_t cp) noexcept { return (props(cp) & kEmojiBit) != 0; }

GraphemeBreak grapheme_break(char32_t cp) noexcept { return gcb_of(props(cp)); }

IndicConjunctBreak indic_conjunct_break(char32_t cp) noexcept { return incb_of(props(cp)); }

bool GraphemeSegmenter::next(char32_t cp) noexcept {
    using G = GraphemeBreak;
    using I = IndicConjunctBreak;
    const uint16_t p = props(cp);
    const G cur = gcb_of(p);
    const I incb = incb_of(p);
    const bool ext_pict = (p & kExtPictBit) != 0;

    bool brk;
    if (prev_ == 0xFF) {
        brk = true;  // GB1
    } else {
        const G prev = static_cast<G>(prev_);
        if (prev == G::CR && cur == G::LF) {
            brk = false;  // GB3
        } else if (prev == G::CR || prev == G::LF || prev == G::Control) {
            brk = true;  // GB4
        } else if (cur == G::CR || cur == G::LF || cur == G::Control) {
            brk = true;  // GB5
        } else if (prev == G::L && (cur == G::L || cur == G::V || cur == G::LV || cur == G::LVT)) {
            brk = false;  // GB6
        } else if ((prev == G::LV || prev == G::V) && (cur == G::V || cur == G::T)) {
            brk = false;  // GB7
        } else if ((prev == G::LVT || prev == G::T) && cur == G::T) {
            brk = false;  // GB8
        } else if (cur == G::Extend || cur == G::ZWJ || cur == G::SpacingMark) {
            brk = false;  // GB9, GB9a
        } else if (prev == G::Prepend) {
            brk = false;  // GB9b
        } else if (incb == I::Consonant && incb_ == 2) {
            brk = false;  // GB9c
        } else if (ext_pict && emoji_ == 2) {
            brk = false;  // GB11
        } else if (prev == G::RegionalIndicator && cur == G::RegionalIndicator && ri_odd_) {
            brk = false;  // GB12, GB13
        } else {
            brk = true;  // GB999
        }
    }

    // GB9c state: Consonant [Extend Linker]* Linker [Extend Linker]*
    if (incb == I::Consonant)
        incb_ = 1;
    else if (incb_ != 0 && incb == I::Linker)
        incb_ = 2;
    else if (incb_ != 0 && incb == I::Extend)
        ;  // keep
    else
        incb_ = 0;

    // GB11 state: ExtPict Extend* ZWJ
    if (ext_pict)
        emoji_ = 1;
    else if (emoji_ == 1 && cur == G::Extend)
        ;  // keep
    else if (emoji_ == 1 && cur == G::ZWJ)
        emoji_ = 2;
    else
        emoji_ = 0;

    // GB12/13 parity: count of consecutive regional indicators ending here.
    if (cur == G::RegionalIndicator)
        ri_odd_ = static_cast<uint8_t>(prev_ == static_cast<uint8_t>(G::RegionalIndicator) ? !ri_odd_ : 1);
    else
        ri_odd_ = 0;

    prev_ = static_cast<uint8_t>(cur);
    return brk;
}

int cluster_width(std::u32string_view cluster, bool ambiguous_wide) noexcept {
    if (cluster.empty()) return 0;
    const char32_t first = cluster[0];
    const uint16_t p = props(first);
    if (p & kControlBit) return 0;

    if (cluster.size() >= 2) {
        const char32_t second = cluster[1];
        if (is_regional_indicator(first)) return is_regional_indicator(second) ? 2 : 1;
        if (p & kEmojiBit) {
            if (second == 0xFE0F) return 2;
            if (second == 0xFE0E) return 1;
            if (is_emoji_modifier(second)) return 2;
        }
        if (p & kExtPictBit) {
            // An emoji ZWJ sequence: ZWJ followed by another pictograph (GB11 joined it).
            for (size_t i = 1; i + 1 < cluster.size(); ++i) {
                if (cluster[i] == 0x200D && is_extended_pictographic(cluster[i + 1])) return 2;
            }
        }
    }

    const int w = width_of(p, ambiguous_wide);
    if (w != 0) return w;
    // A zero-width first code point: an invisible format character standing alone stays 0;
    // a combining sequence without a base is drawn as a standalone glyph in one cell.
    return gcb_of(p) == GraphemeBreak::Control ? 0 : 1;
}

} // namespace bropty::unicode
