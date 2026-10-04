// Unicode width / grapheme segmentation tests.
// Usage: test_unicode <path to tests/data/GraphemeBreakTest.txt>
// Runs the full UAX #29 conformance suite through GraphemeSegmenter, the width spot
// checks, cluster_width cases and range self-consistency checks, and prints lookup speed.

#include "bropty/unicode.h"
#include "test_common.h"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace bropty::unicode;

namespace {

int failures = 0;

void fail(const char* fmt, unsigned a, long long b, long long c) {
    std::printf("FAIL: ");
    std::printf(fmt, a, b, c);
    std::printf("\n");
    ++failures;
}

void check_width(char32_t cp, int expect, bool amb = false) {
    int got = width(cp, amb);
    if (got != expect) {
        std::printf("FAIL: width(U+%04X, ambiguous_wide=%d) = %d, expected %d\n", static_cast<unsigned>(cp),
                    amb ? 1 : 0, got, expect);
        ++failures;
    }
}

void check_cluster(const std::u32string& s, int expect, const char* name) {
    int got = cluster_width(s);
    if (got != expect) {
        std::printf("FAIL: cluster_width(%s) = %d, expected %d\n", name, got, expect);
        ++failures;
    }
    // The cluster must also be one extended grapheme cluster.
    GraphemeSegmenter seg;
    for (size_t i = 0; i < s.size(); ++i) {
        bool brk = seg.next(s[i]);
        if (brk != (i == 0)) {
            std::printf("FAIL: %s is not one grapheme cluster (boundary before index %zu)\n", name, i);
            ++failures;
            break;
        }
    }
}

// Every code point in [lo, hi] must have width `expect`.
void check_range(char32_t lo, char32_t hi, int expect, bool amb = false) {
    for (char32_t c = lo; c <= hi; ++c) {
        if (width(c, amb) != expect) {
            fail("range width(U+%04X) = %lld, expected %lld", static_cast<unsigned>(c), width(c, amb), expect);
            return;
        }
    }
}

int run_conformance(const char* path) {
    std::ifstream in(path);
    if (!in) {
        std::printf("FAIL: cannot open %s\n", path);
        return -1;
    }
    std::string line;
    int lineno = 0, cases = 0;
    while (std::getline(in, line)) {
        ++lineno;
        size_t hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        // Tokens: "÷" (UTF-8 C3 B7), "×" (UTF-8 C3 97) and hex code points.
        std::vector<char32_t> cps;
        std::vector<bool> breaks;  // breaks[i] = boundary before cps[i]; breaks[n] = at end
        size_t i = 0;
        bool any = false;
        while (i < line.size()) {
            unsigned char ch = static_cast<unsigned char>(line[i]);
            if (ch == ' ' || ch == '\t' || ch == '\r') {
                ++i;
            } else if (ch == 0xC3 && i + 1 < line.size()) {
                unsigned char ch2 = static_cast<unsigned char>(line[i + 1]);
                breaks.push_back(ch2 == 0xB7);
                any = true;
                i += 2;
            } else {
                size_t j = i;
                while (j < line.size() && std::isxdigit(static_cast<unsigned char>(line[j]))) ++j;
                if (j == i) {
                    std::printf("FAIL: line %d: unparsable\n", lineno);
                    ++failures;
                    break;
                }
                cps.push_back(static_cast<char32_t>(std::stoul(line.substr(i, j - i), nullptr, 16)));
                i = j;
            }
        }
        if (!any) continue;
        if (breaks.size() != cps.size() + 1) {
            std::printf("FAIL: line %d: malformed\n", lineno);
            ++failures;
            continue;
        }
        ++cases;
        GraphemeSegmenter seg;
        std::string got = "", want = "";
        bool ok = true;
        for (size_t k = 0; k < cps.size(); ++k) {
            bool b = seg.next(cps[k]);
            if (b != breaks[k]) ok = false;
            char buf[16];
            std::snprintf(buf, sizeof buf, "%s%04X ", b ? "| " : "x ", static_cast<unsigned>(cps[k]));
            got += buf;
            std::snprintf(buf, sizeof buf, "%s%04X ", breaks[k] ? "| " : "x ", static_cast<unsigned>(cps[k]));
            want += buf;
        }
        if (!ok) {
            std::printf("FAIL: GraphemeBreakTest line %d\n  got:  %s\n  want: %s\n", lineno, got.c_str(), want.c_str());
            ++failures;
        }
        // reset() must give the same result as a fresh segmenter.
        seg.reset();
        if (!cps.empty() && !seg.next(cps[0])) {
            std::printf("FAIL: line %d: no boundary after reset()\n", lineno);
            ++failures;
        }
    }
    return cases;
}

void benchmark() {
    // Mixed text: ASCII, Latin-1, CJK, emoji, combining marks.
    std::vector<char32_t> text;
    for (char32_t c = 0x20; c < 0x7F; ++c) text.push_back(c);
    for (char32_t c = 0xA0; c < 0x250; ++c) text.push_back(c);
    for (char32_t c = 0x4E00; c < 0x4F00; ++c) text.push_back(c);
    for (char32_t c = 0x1F600; c < 0x1F650; ++c) text.push_back(c);
    for (char32_t c = 0x300; c < 0x370; ++c) text.push_back(c);
    for (char32_t c = 0x10000; c < 0x10FFFF; c += 4099) text.push_back(c);
    const int reps = 2000;
    unsigned sink = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (int r = 0; r < reps; ++r)
        for (char32_t c : text) sink += static_cast<unsigned>(width(c + static_cast<char32_t>(r & 1)));
    auto t1 = std::chrono::steady_clock::now();
    GraphemeSegmenter seg;
    for (int r = 0; r < reps; ++r)
        for (char32_t c : text) sink += seg.next(c) ? 1u : 0u;
    auto t2 = std::chrono::steady_clock::now();
    double n = static_cast<double>(reps) * static_cast<double>(text.size());
    std::printf("bench: width %.2f ns/cp, segmenter %.2f ns/cp (sink %u)\n",
                std::chrono::duration<double, std::nano>(t1 - t0).count() / n,
                std::chrono::duration<double, std::nano>(t2 - t1).count() / n, sink);
}

} // namespace

int main(int argc, char** argv) {
    init_test();
    if (argc < 2) {
        std::printf("usage: test_unicode <GraphemeBreakTest.txt>\n");
        return 2;
    }

    int cases = run_conformance(argv[1]);
    if (cases <= 0) {
        std::printf("FAIL: no conformance cases read\n");
        ++failures;
    } else {
        std::printf("GraphemeBreakTest: %d cases\n", cases);
    }

    // Width spot checks.
    struct W { char32_t cp; int w; };
    const W spot[] = {
        {0x2705, 2},  {0x231A, 2},  {0x1FA70, 2}, {0x30000, 2}, {0x4E2D, 2},  {0xFF21, 2}, {0x200B, 0},
        {0x0E31, 0},  {0x05B0, 0},  {0xFE0F, 0},  {0x200D, 0},  {0x0301, 0},  {0x00E9, 1}, {0x2764, 1},
        {0x1F600, 2}, {0x1F1E6, 1}, {0xAC00, 2},  {0x1160, 0},  {0x00AD, 1},  {'A', 1},    {0x00, 0},
        {0x1B, 0},    {0x7F, 0},    {0x85, 0},    {0x9F, 0},    {0xA0, 1},    {0x0600, 1}, {0x06DD, 1},
        {0x070F, 1},  {0x0890, 1},  {0x08E2, 1},  {0x110BD, 1}, {0x110CD, 1}, {0x200C, 0}, {0xFEFF, 0},
        {0x2060, 0},  {0xE0001, 0}, {0xE0100, 0}, {0x115F, 2},  {0x1100, 2},  {0x11A8, 0}, {0xD7B0, 0},
        {0xD7FF, 0},  {0x3164, 0},  {0x3099, 0},  {0xE000, 1},  {0x10FFFD, 1}, {0x0378, 1}, {0x3400, 2},
        {0x9FFF, 2},  {0xFAFF, 2},  {0x2FFFD, 2}, {0x3FFFD, 2}, {0x1F3FB, 2}, {0x1F1FF, 1}, {0x0903, 1},
        {0x20E3, 0},  {0x1F44D, 2}, {0x3000, 2},  {0xFF61, 1},  {0x0110000, 1}, {0xFFFFFFFF, 1},
    };
    for (const W& w : spot) check_width(w.cp, w.w);

    // Ambiguous.
    check_width(0x00A1, 1);
    check_width(0x00A1, 2, true);
    check_width(0x2460, 1);
    check_width(0x2460, 2, true);
    check_width(0x0300, 0, true);  // combining marks are A but zero-width wins
    check_width(0x4E2D, 2, true);
    check_width('a', 1, true);
    if (!is_ambiguous(0x00A1) || is_ambiguous('a') || is_ambiguous(0x0300)) {
        std::printf("FAIL: is_ambiguous\n");
        ++failures;
    }

    // Predicates.
    struct P { char32_t cp; bool zw, ep, em; };
    const P preds[] = {
        {0x0301, true, false, false}, {0x200D, true, false, false}, {0xFE0F, true, false, false},
        {0x200B, true, false, false}, {0x00, false, false, false},  {0x7F, false, false, false},
        {'a', false, false, false},   {'#', false, false, true},    {0x2764, false, true, true},
        {0x1F600, false, true, true}, {0x1F1E6, false, false, true}, {0x00A9, false, true, true},
        {0x1FFFD, false, true, false}, {0x4E2D, false, false, false},
    };
    for (const P& p : preds) {
        if (is_zero_width(p.cp) != p.zw || is_extended_pictographic(p.cp) != p.ep || is_emoji(p.cp) != p.em)
            fail("predicates U+%04X: zw/ep/em = %lld (want %lld)", static_cast<unsigned>(p.cp),
                 is_zero_width(p.cp) * 100 + is_extended_pictographic(p.cp) * 10 + is_emoji(p.cp),
                 p.zw * 100 + p.ep * 10 + p.em);
    }

    // Grapheme_Cluster_Break / InCB spot checks.
    if (grapheme_break(0x0D) != GraphemeBreak::CR || grapheme_break(0x0A) != GraphemeBreak::LF ||
        grapheme_break(0x200D) != GraphemeBreak::ZWJ || grapheme_break(0x1F1E6) != GraphemeBreak::RegionalIndicator ||
        grapheme_break(0x0600) != GraphemeBreak::Prepend || grapheme_break(0x0903) != GraphemeBreak::SpacingMark ||
        grapheme_break(0x1100) != GraphemeBreak::L || grapheme_break(0x1161) != GraphemeBreak::V ||
        grapheme_break(0x11A8) != GraphemeBreak::T || grapheme_break(0xAC00) != GraphemeBreak::LV ||
        grapheme_break(0xAC01) != GraphemeBreak::LVT || grapheme_break(0x0301) != GraphemeBreak::Extend ||
        grapheme_break(0x200B) != GraphemeBreak::Control || grapheme_break('a') != GraphemeBreak::Other ||
        indic_conjunct_break(0x094D) != IndicConjunctBreak::Linker ||
        indic_conjunct_break(0x0915) != IndicConjunctBreak::Consonant ||
        indic_conjunct_break(0x200D) != IndicConjunctBreak::Extend ||
        indic_conjunct_break('a') != IndicConjunctBreak::None) {
        std::printf("FAIL: grapheme_break / indic_conjunct_break spot checks\n");
        ++failures;
    }

    // Range self-consistency (rules in unicode.h applied to whole blocks).
    check_range(0x0300, 0x036F, 0);    // combining diacriticals
    check_range(0x1160, 0x11FF, 0);    // Hangul medial vowels / finals
    check_range(0xD7B0, 0xD7FF, 0);
    check_range(0xFE00, 0xFE0F, 0);    // variation selectors
    check_range(0xE0100, 0xE01EF, 0);  // variation selectors supplement
    check_range(0xE0000, 0xE0FFF, 0);  // default ignorable (tags etc.)
    check_range(0x200B, 0x200F, 0);
    check_range(0x2060, 0x206F, 0);
    check_range(0x1100, 0x115F, 2);    // Hangul leading jamo
    check_range(0xAC00, 0xD7A3, 2);    // Hangul syllables
    check_range(0x3400, 0x4DBF, 2);    // CJK ext A (incl. unassigned)
    check_range(0x4E00, 0x9FFF, 2);
    check_range(0xF900, 0xFAFF, 2);
    check_range(0x20000, 0x2FFFD, 2);  // planes 2 and 3
    check_range(0x30000, 0x3FFFD, 2);
    check_range(0xFF01, 0xFF60, 2);    // fullwidth forms
    check_range(0xFFE0, 0xFFE6, 2);
    check_range(0x1F1E6, 0x1F1FF, 1);  // regional indicators
    check_range(0x1F600, 0x1F64F, 2);  // emoticons
    check_range(0x1F947, 0x1F9FF, 2);  // supplemental symbols and pictographs (emoji part)
    check_range(0x1F900, 0x1F90B, 1);  // ... the non-emoji circled crosses at its start
    check_range(0x0041, 0x007E, 1);
    check_range(0x00A0, 0x00AC, 1);    // Latin-1 up to SHY (no ambiguous in the narrow sense here)
    check_range(0x0400, 0x0482, 1);    // Cyrillic letters
    check_range(0xE000, 0xF8FF, 1);    // private use (BMP)
    check_range(0xF0000, 0xFFFFD, 1);  // plane 15 private use
    check_range(0x50000, 0xDFFFF, 1);  // unassigned planes
    check_range(0x0000, 0x001F, 0);
    check_range(0x007F, 0x009F, 0);
    for (char32_t c = 0x80; c <= 0x10FFFF; ++c) {
        int w = width(c), wa = width(c, true);
        if (w < 0 || w > 2 || wa < w || (wa != w && !is_ambiguous(c)) || (is_zero_width(c) && w != 0)) {
            fail("consistency U+%04X: width %lld / ambiguous-wide %lld", static_cast<unsigned>(c), w, wa);
            break;
        }
    }

    // Cluster widths.
    check_cluster(U"\U0001F44D\U0001F3FD", 2, "thumbs up + skin tone");
    check_cluster(U"\U0001F468‍\U0001F469‍\U0001F467", 2, "family ZWJ");
    check_cluster(U"\U0001F1FA\U0001F1F8", 2, "US flag");
    check_cluster(U"❤️", 2, "heart + VS16");
    check_cluster(U"❤", 1, "heart (text)");
    check_cluster(U"⌚︎", 1, "watch + VS15");
    check_cluster(U"é", 1, "e + acute");
    check_cluster(U"각", 2, "L+V+T jamo");
    check_cluster(U"ᅟᅡ", 2, "choseong filler + V");
    check_cluster(U"각", 2, "LV + T");
    check_cluster(U"#️⃣", 2, "keycap #");
    check_cluster(U"☝\U0001F3FB", 2, "index pointing up + skin tone");
    check_cluster(U"❤️‍\U0001F525", 2, "heart on fire");
    check_cluster(U"\U0001F441‍\U0001F5E8", 2, "eye in speech bubble (unqualified)");
    check_cluster(U"\U0001F3F4\U000E0067\U000E0062\U000E0065\U000E006E\U000E0067\U000E007F", 2, "England flag");
    check_cluster(U"क्ष", 1, "Devanagari conjunct");
    check_cluster(U"́", 1, "lone combining mark");
    check_cluster(U"‍", 1, "lone ZWJ");
    check_cluster(U"​", 0, "ZWSP");
    check_cluster(U"中́", 2, "CJK + combining");
    check_cluster(U"¡", 1, "ambiguous");
    check_cluster(U"\U0001F1E6", 1, "lone regional indicator");
    check_cluster(U"؀١", 1, "prepend + digit");
    if (cluster_width(U"") != 0 || cluster_width(U"¡", true) != 2 || cluster_width(U"\x1B") != 0) {
        std::printf("FAIL: cluster_width edge cases\n");
        ++failures;
    }
    // Two flags are two clusters.
    {
        GraphemeSegmenter seg;
        const char32_t flags[] = {0x1F1FA, 0x1F1F8, 0x1F1EC, 0x1F1E7, 0x1F1E6};
        const bool want[] = {true, false, true, false, true};
        for (int i = 0; i < 5; ++i) {
            if (seg.next(flags[i]) != want[i]) {
                std::printf("FAIL: regional indicator pairing at %d\n", i);
                ++failures;
            }
        }
    }

    benchmark();

    if (failures == 0)
        std::printf("test_unicode: all passed\n");
    else
        std::printf("test_unicode: %d failure(s)\n", failures);
    return failures != 0;
}
