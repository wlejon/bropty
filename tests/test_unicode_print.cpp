// Printing Unicode through the terminal: combining marks, grapheme clusters
// (mode 2027 on and off), cluster widths and wide-cell placement.
#include "term_helpers.h"

using namespace bropty;
using th::T;
using th::utf8;

int main() {
    init_test();

    {
        T t(10, 3);
        t << "e\xcc\x81X";  // e + COMBINING ACUTE
        CHECK_EQ(t.ccol(), 2);
        CHECK_EQ(utf8(t.cluster(0, 0)), std::string("e\xcc\x81"));
        CHECK_EQ(t.row(0), std::string("e\xcc\x81X"));
    }
    {
        // Combining mark after a cursor move attaches to the cell left of the cursor.
        T t(10, 3);
        t << "ab\x1b[1;2H\xcc\x88";
        CHECK_EQ(utf8(t.cluster(0, 0)), std::string("a\xcc\x88"));
    }
    {
        // Combining mark at the pending-wrap position joins the last column, not the next row.
        T t(3, 3);
        t << "abc\xcc\x81";
        CHECK_EQ(utf8(t.cluster(0, 2)), std::string("c\xcc\x81"));
        CHECK_EQ(t.row(1), std::string(""));
    }
    {
        // Combining mark on a wide character.
        T t(10, 3);
        t << "\xe4\xb8\xad\xcc\x81";
        CHECK_EQ(utf8(t.cluster(0, 0)), std::string("\xe4\xb8\xad\xcc\x81"));
        CHECK_EQ(t.ccol(), 2);
    }
    {
        // Several marks, Hebrew points, Thai.
        T t(10, 3);
        t << "a\xcc\x81\xcc\x82\xcc\x83" "\xd7\xa9\xd6\xb0" "\xe0\xb8\x81\xe0\xb8\xb1";
        CHECK_EQ(t.ccol(), 3);
        CHECK_EQ(utf8(t.cluster(0, 0)), std::string("a\xcc\x81\xcc\x82\xcc\x83"));
        CHECK_EQ(utf8(t.cluster(0, 1)), std::string("\xd7\xa9\xd6\xb0"));
        CHECK_EQ(utf8(t.cluster(0, 2)), std::string("\xe0\xb8\x81\xe0\xb8\xb1"));
    }
    {
        // Emoji widths.
        T t(20, 3);
        t << "\xe2\x9c\x85|\xe2\x8c\x9a|\xf0\x9f\xa9\xb0|";  // U+2705 U+231A U+1FA70
        CHECK_EQ(t.row(0), std::string("\xe2\x9c\x85|\xe2\x8c\x9a|\xf0\x9f\xa9\xb0|"));
        CHECK_EQ(t.ccol(), 9);
        CHECK(t.cell(0, 0).wide() == Wide::Lead);
    }
    {
        // Mode 2027 (default on): ZWJ family, skin tone, flag, VS16, keycap are single clusters.
        T t(20, 3);
        const std::string family = "\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x91\xa7";
        t << family << "|";
        CHECK_EQ(t.ccol(), 3);
        CHECK_EQ(utf8(t.cluster(0, 0)), family);
        t << "\xf0\x9f\x91\x8d\xf0\x9f\x8f\xbd|";  // thumbs up + skin tone
        CHECK_EQ(t.ccol(), 6);
        t << "\xf0\x9f\x87\xba\xf0\x9f\x87\xb8|";  // US flag
        CHECK_EQ(t.ccol(), 9);
        t << "\xe2\x9d\xa4\xef\xb8\x8f|";  // heart + VS16 widens to 2
        CHECK_EQ(t.ccol(), 12);
        CHECK(t.cell(0, 9).wide() == Wide::Lead);
        t << "#\xef\xb8\x8f\xe2\x83\xa3|";  // keycap
        CHECK_EQ(t.ccol(), 15);
        t << "\xe1\x84\x80\xe1\x85\xa1\xe1\x86\xa8|";  // Hangul L V T jamo
        CHECK_EQ(t.ccol(), 18);
    }
    {
        // Mode 2027 off: per-code-point widths; zero-width joiners/selectors still combine.
        T t(20, 3, 100, false);
        t << "\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9|";
        CHECK_EQ(t.ccol(), 5);
        t << "\xe2\x9d\xa4\xef\xb8\x8f|";  // VS16 does not widen
        CHECK_EQ(t.ccol(), 7);
        t << "\xf0\x9f\x87\xba\xf0\x9f\x87\xb8|";
        CHECK_EQ(t.ccol(), 10);
        t << "\x1b[?2027h\x1b[2;1H\xe2\x9d\xa4\xef\xb8\x8f|";  // DECSET 2027 turns it on
        CHECK_EQ(t.ccol(), 3);
        t << "\x1b[?2027$p";
        CHECK_EQ(t.reply(), std::string("\x1b[?2027;1$y"));
    }
    {
        // VS16 widening at the last column moves the cluster to the next row.
        T t(5, 3);
        t << "abcd\xe2\x9d\xa4\xef\xb8\x8f";
        CHECK_EQ(t.row(0), std::string("abcd"));
        CHECK(t.cell(0, 4).wide() == Wide::SpacerHead);
        CHECK(t.t.row(0).wrapped());
        CHECK_EQ(utf8(t.cluster(1, 0)), std::string("\xe2\x9d\xa4\xef\xb8\x8f"));
        CHECK_EQ(t.crow(), 1);
        CHECK_EQ(t.ccol(), 2);
    }
    {
        // Cluster continuation breaks if the cursor moved in between.
        T t(10, 3);
        t << "\xf0\x9f\x87\xba\x1b[C\xf0\x9f\x87\xb8";
        CHECK(t.cell(0, 0).wide() == Wide::Narrow);
        CHECK_EQ(t.cell(0, 2).cp(), char32_t(0x1F1F8));
    }
    {
        // Lone zero-width characters at column 0 are dropped; C1 code points are not printed.
        T t(10, 3);
        t << "\xcc\x81\xc2\x85X";
        CHECK_EQ(t.row(0), std::string("X"));
        CHECK_EQ(t.ccol(), 1);
    }
    {
        // Replacement character for ill-formed input occupies a cell.
        T t(10, 3);
        t << "a\xff" "b";
        CHECK_EQ(t.row(0), std::string("a\xef\xbf\xbd" "b"));
    }
    {
        // Ambiguous width option.
        TerminalOptions o = th::opts(10, 2);
        o.ambiguous_wide = true;
        Terminal term(o);
        term.feed("\xc2\xb1x");  // U+00B1 is East Asian Ambiguous
        CHECK_EQ(term.cursor().col, 3);
    }
    return check::finish("test_unicode_print");
}
