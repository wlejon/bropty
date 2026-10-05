// Screen model against xterm behaviour: wrapping, cursor motion, scrolling
// regions (vertical and horizontal), editing, erasing, SGR, charsets, alternate
// screen, history capacity, hyperlinks.
#include "term_helpers.h"

using namespace bropty;
using th::T;

static void wrapping() {
    {
        T t(10, 4);
        t << "0123456789";
        CHECK_EQ(t.crow(), 0);
        CHECK_EQ(t.ccol(), 9);
        CHECK(t.t.cursor().pending_wrap);
        t << "\r\n";
        CHECK_EQ(t.crow(), 1);  // CRLF after a full line moves one row, not two
    }
    {
        T t(10, 4);
        t << "0123456789\rX";
        CHECK_EQ(t.row(0), std::string("X123456789"));
    }
    {
        T t(10, 4);
        t << "0123456789AB";
        CHECK_EQ(t.row(0), std::string("0123456789"));
        CHECK_EQ(t.row(1), std::string("AB"));
        CHECK(t.t.row(0).wrapped());
        CHECK(!t.t.row(1).wrapped());
    }
    {
        T t(10, 4);
        t << "0123456789" << "\x1b[?7l" << "ABC";
        CHECK_EQ(t.row(0), std::string("012345678C"));
        CHECK_EQ(t.row(1), std::string(""));
        CHECK(!t.t.row(0).wrapped());
    }
    {
        // Backspace from the pending-wrap state moves left (xterm).
        T t(10, 4);
        t << "0123456789\bX";
        CHECK_EQ(t.row(0), std::string("01234567X9"));
        CHECK_EQ(t.row(1), std::string(""));
    }
    {
        // Cursor motion clears pending wrap.
        T t(10, 4);
        t << "0123456789\x1b[DX";
        CHECK_EQ(t.row(0), std::string("01234567X9"));
    }
}

static void index_and_regions() {
    {
        T t(10, 4);
        t << "L1\r\nL2\x1b[H\x1bM";
        CHECK_EQ(t.row(0), std::string(""));
        CHECK_EQ(t.row(1), std::string("L1"));
        CHECK_EQ(t.row(2), std::string("L2"));
    }
    {
        T t(10, 4);
        t << "ab\x1b" "Ecd";
        CHECK_EQ(t.row(1), std::string("cd"));
        T u(10, 4);
        u << "ab\x1b" "Dcd";
        CHECK_EQ(u.row(1), std::string("  cd"));
    }
    {
        T t(10, 5);
        t << "A\r\nB\r\nC\r\nD\r\nE" << "\x1b[2;4r" << "\x1b[4;1H\n";
        CHECK_EQ(t.row(0), std::string("A"));
        CHECK_EQ(t.row(1), std::string("C"));
        CHECK_EQ(t.row(2), std::string("D"));
        CHECK_EQ(t.row(3), std::string(""));
        CHECK_EQ(t.row(4), std::string("E"));
        CHECK_EQ(t.t.history_rows(), size_t(0));
        CHECK_EQ(t.crow(), 3);
    }
    {
        T t(10, 5);
        t << "\x1b[3;3H\x1b[2;4r";
        CHECK_EQ(t.crow(), 0);
        CHECK_EQ(t.ccol(), 0);
        t << "\x1b[?6h";  // origin mode homes to the region
        CHECK_EQ(t.crow(), 1);
        t << "\x1b[10;1H";  // clamped inside the region
        CHECK_EQ(t.crow(), 3);
        t << "\x1b[?6l\x1b[r";
        CHECK_EQ(t.crow(), 0);
    }
    {
        // Lines scrolled off a region whose top is the screen top go to history.
        T t(10, 4);
        t << "A\r\nB\r\nC\r\nD" << "\x1b[1;3r\x1b[3;1H\n";
        CHECK_EQ(t.t.history_rows(), size_t(1));
        CHECK_EQ(t.t.history_text(0), std::string("A"));
        CHECK_EQ(t.row(3), std::string("D"));
    }
    {
        // SU / SD.
        T t(10, 4);
        t << "A\r\nB\r\nC\r\nD\x1b[2S";
        CHECK_EQ(t.row(0), std::string("C"));
        CHECK_EQ(t.row(1), std::string("D"));
        t << "\x1b[1T";
        CHECK_EQ(t.row(0), std::string(""));
        CHECK_EQ(t.row(1), std::string("C"));
    }
    {
        // IL / DL inside the region; outside it they do nothing.
        T t(10, 5);
        t << "A\r\nB\r\nC\r\nD\r\nE\x1b[2;4r\x1b[3;3H\x1b[L";
        CHECK_EQ(t.row(1), std::string("B"));
        CHECK_EQ(t.row(2), std::string(""));
        CHECK_EQ(t.row(3), std::string("C"));
        CHECK_EQ(t.row(4), std::string("E"));
        CHECK_EQ(t.ccol(), 0);
        t << "\x1b[2;1H\x1b[2M";
        CHECK_EQ(t.row(1), std::string("C"));
        CHECK_EQ(t.row(2), std::string(""));
        CHECK_EQ(t.row(3), std::string(""));
        t << "\x1b[5;1H\x1b[L";
        CHECK_EQ(t.row(4), std::string("E"));
    }
}

static void horizontal_margins() {
    T t(10, 4);
    t << "0123456789\r\nabcdefghij\r\nABCDEFGHIJ";
    t << "\x1b[?69h\x1b[3;7s";  // DECLRMM, left/right margins columns 3..7
    CHECK_EQ(t.crow(), 0);
    CHECK_EQ(t.ccol(), 0);
    t << "\x1b[1;3H\x1b[2@";  // ICH inside margins shifts only to the right margin
    CHECK_EQ(t.row(0), std::string("01  234789"));
    t << "\x1b[1;3H\x1b[P";
    CHECK_EQ(t.row(0), std::string("01 234 789"));
    t << "\x1b[2;7HXYZ";  // printing wraps at the right margin, back to the left one
    CHECK_EQ(t.row(1), std::string("abcdefXhij"));
    CHECK_EQ(t.row(2), std::string("ABYZEFGHIJ"));
    CHECK(!t.t.row(1).wrapped());  // not a soft wrap of the whole line
    t << "\x1b[1;1r\x1b[1;4r\x1b[2;3H\x1b[S";  // SU scrolls only the margin box
    CHECK_EQ(t.row(0), std::string("01cdefX789"));
    CHECK_EQ(t.row(1), std::string("abYZEFGhij"));
    CHECK_EQ(t.row(2), std::string("AB     HIJ"));
    CHECK_EQ(t.t.history_rows(), size_t(0));
    t << "\x1b[?69l";
    t << "\x1b[1;1H";
    CHECK_EQ(t.ccol(), 0);
}

static void tabs() {
    T t(40, 3);
    t << "\tX";
    CHECK_EQ(t.cell(0, 8).cp(), char32_t('X'));
    t << "\r\x1b[3g\x1b[1;5H\x1bH\r\tY";
    CHECK_EQ(t.cell(0, 4).cp(), char32_t('Y'));
    t << "\r\t\tZ";  // no stop after 4: tab goes to the last column
    CHECK_EQ(t.cell(0, 39).cp(), char32_t('Z'));
    t << "\x1b[2;1H\x1b[?5W\x1b[2IA";
    CHECK_EQ(t.cell(1, 16).cp(), char32_t('A'));
    t << "\x1b[Z" "B";
    CHECK_EQ(t.cell(1, 16).cp(), char32_t('B'));
    t << "\x1b[3;20H\x1b[0g\x1b[3;1H\t\tC";
    CHECK_EQ(t.cell(2, 16).cp(), char32_t('C'));
}

static void charsets() {
    T t(20, 2);
    t << "\x1b(0lqk\x1b(Bq";
    CHECK_EQ(t.cell(0, 0).cp(), char32_t(0x250C));
    CHECK_EQ(t.cell(0, 1).cp(), char32_t(0x2500));
    CHECK_EQ(t.cell(0, 2).cp(), char32_t(0x2510));
    CHECK_EQ(t.cell(0, 3).cp(), char32_t('q'));
    t << "\x1b)0\x0eq\x0fq";  // G1 = graphics, SO / SI
    CHECK_EQ(t.cell(0, 4).cp(), char32_t(0x2500));
    CHECK_EQ(t.cell(0, 5).cp(), char32_t('q'));
    t << "\x1b*0\x1bNqq";  // SS2: one character from G2
    CHECK_EQ(t.cell(0, 6).cp(), char32_t(0x2500));
    CHECK_EQ(t.cell(0, 7).cp(), char32_t('q'));
    t << "\x1b(A#\x1b(B#";
    CHECK_EQ(t.cell(0, 8).cp(), char32_t(0xA3));
    CHECK_EQ(t.cell(0, 9).cp(), char32_t('#'));
    t << "\x1b(0\x1b" "7\x1b(B\x1b" "8x";  // DECSC/DECRC save the charsets
    CHECK_EQ(t.cell(0, 10).cp(), char32_t(0x2502));
}

static void sgr() {
    {
        T t(40, 2);
        t << "\x1b[4:3mA\x1b[4:0mB\x1b[21mC\x1b[0m\x1b[4:5mD\x1b[4mE\x1b[24mF";
        CHECK(t.style(0, 0).underline == Underline::Curly);
        CHECK(t.style(0, 1).underline == Underline::None);
        CHECK(t.style(0, 2).underline == Underline::Double);
        CHECK(t.style(0, 3).underline == Underline::Dashed);
        CHECK(t.style(0, 4).underline == Underline::Single);
        CHECK(t.style(0, 5).underline == Underline::None);
    }
    {
        T t(40, 2);
        t << "\x1b[38;2;10;20;30mA\x1b[38:2:10:20:30mB\x1b[38:2::10:20:30mC\x1b[38;5;196mD\x1b[38:5:17mE";
        for (int x = 0; x < 3; ++x) CHECK(t.style(0, x).fg == Color::rgb(10, 20, 30));
        CHECK(t.style(0, 3).fg == Color::indexed(196));
        CHECK(t.style(0, 4).fg == Color::indexed(17));
    }
    {
        T t(40, 2);
        t << "\x1b[1;38;2;1;2;3;4mA\x1b[0;48;5;9;3mB\x1b[31;42;95;105mC\x1b[39;49mD";
        CHECK(t.style(0, 0).has(Attr_Bold));
        CHECK(t.style(0, 0).underline == Underline::Single);
        CHECK(t.style(0, 0).fg == Color::rgb(1, 2, 3));
        CHECK(!t.style(0, 1).has(Attr_Bold));
        CHECK(t.style(0, 1).bg == Color::indexed(9));
        CHECK(t.style(0, 1).has(Attr_Italic));
        CHECK(t.style(0, 2).fg == Color::indexed(13));
        CHECK(t.style(0, 2).bg == Color::indexed(13));
        CHECK(t.style(0, 3).fg.is_default() && t.style(0, 3).bg.is_default());
    }
    {
        T t(40, 2);
        t << "\x1b[58:2::1:2:3m\x1b[4mA\x1b[59mB\x1b[58;5;4mC";
        CHECK(t.style(0, 0).underline_color == Color::rgb(1, 2, 3));
        CHECK(t.style(0, 1).underline_color.is_default());
        CHECK(t.style(0, 2).underline_color == Color::indexed(4));
    }
    {
        T t(40, 2);
        t << "\x1b[1;2;3;5;7;8;9;53mA\x1b[22;23;25;27;28;29;55mB";
        const Style& a = t.style(0, 0);
        CHECK(a.has(Attr_Bold) && a.has(Attr_Dim) && a.has(Attr_Italic) && a.has(Attr_Blink) &&
              a.has(Attr_Inverse) && a.has(Attr_Invisible) && a.has(Attr_Strike) && a.has(Attr_Overline));
        CHECK(t.style(0, 1).attrs == 0);
    }
    {
        // Malformed extended color stops SGR processing (xterm).
        T t(40, 2);
        t << "\x1b[38;5m\x1b[1;38;2;1mA";
        CHECK(t.style(0, 0).has(Attr_Bold));
        CHECK(t.style(0, 0).fg.is_default());
    }
    {
        // Prefixed / intermediate CSI finals never alias their unprefixed forms.
        T t(40, 5);
        t << "\x1b[>4;1mX";
        CHECK(t.style(0, 0).attrs == 0 && t.style(0, 0).underline == Underline::None);
        t << "\x1b[s\x1b[4;6H\x1b[>1u";
        CHECK_EQ(t.crow(), 3);
        CHECK_EQ(t.ccol(), 5);
        t << "\x1b[?5h";
        CHECK(t.t.modes().reverse_video);
        CHECK(!t.t.modes().insert);
        t << "\x1b[?4h";  // DECSCLM (smooth scroll), not IRM
        CHECK(!t.t.modes().insert);
        t << "\x1b[1 J";  // unknown intermediate: no ED
        t << "\x1b[1;1HZ";
        CHECK_EQ(t.row(0), std::string("Z"));
    }
}

static void editing() {
    {
        T t(10, 2);
        t << "abcdef\x1b[1;2H\x1b[4hXY\x1b[4l";
        CHECK_EQ(t.row(0), std::string("aXYbcdef"));
    }
    {
        T t(10, 2);
        t << "abcdefghij\x1b[1;2H\x1b[4hX";
        CHECK_EQ(t.row(0), std::string("aXbcdefghi"));  // IRM pushes the last char off
    }
    {
        T t(10, 2);
        t << "abcdef\x1b[1;2H\x1b[2P";
        CHECK_EQ(t.row(0), std::string("adef"));
        t << "\x1b[1;2H\x1b[2@";
        CHECK_EQ(t.row(0), std::string("a  def"));
        t << "\x1b[1;1H\x1b[3X";
        CHECK_EQ(t.row(0), std::string("   def"));
        t << "\x1b[1;5H\x1b[99X";
        CHECK_EQ(t.row(0), std::string("   d"));
    }
    {
        // Erase uses the current background (BCE) and drops other attributes.
        T t(10, 3);
        t << "abc\x1b[1;44m\x1b[K";
        CHECK(t.style(0, 5).bg == Color::indexed(4));
        CHECK(!t.style(0, 5).has(Attr_Bold));
        CHECK(t.cell(0, 5).is_empty());
        t << "\x1b[0m\x1b[2;1Hxyz\x1b[2;2H\x1b[1K";
        CHECK_EQ(t.row(1), std::string("  z"));
        t << "\x1b[2;2H\x1b[2K";
        CHECK_EQ(t.row(1), std::string(""));
    }
    {
        T t(5, 3);
        t << "aaaaa\r\nbbbbb\r\nccc\x1b[2;3H\x1b[J";
        CHECK_EQ(t.row(0), std::string("aaaaa"));
        CHECK_EQ(t.row(1), std::string("bb"));
        CHECK_EQ(t.row(2), std::string(""));
        t << "\x1b[1;3H\x1b[1J";
        CHECK_EQ(t.row(0), std::string("   aa"));
        t << "\x1b[2J";
        CHECK_EQ(t.row(0), std::string(""));
        CHECK_EQ(t.crow(), 0);
    }
    {
        // Selective erase keeps DECSCA-protected cells; plain erase does not.
        T t(10, 2);
        t << "ab\x1b[1\"qCD\x1b[0\"qef\x1b[1;1H\x1b[?K";
        CHECK_EQ(t.row(0), std::string("  CD"));
        t << "\x1b[K";
        CHECK_EQ(t.row(0), std::string(""));
    }
    {
        // REP repeats the last graphic character.
        T t(10, 2);
        t << "x\x1b[4b";
        CHECK_EQ(t.row(0), std::string("xxxxx"));
        t << "\x1b(0q\x1b(B\x1b[2b";
        CHECK_EQ(t.row(0), std::string("xxxxx\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"));
        // Like printing: wraps at the margin (libvterm clamps instead).
        t << "\x1b[1;9Hy\x1b[3b";
        CHECK_EQ(t.all(), std::string("xxxxx\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80yy~yy"));
    }
    {
        // REP of a wide character repeats it at full width.
        T t(10, 2);
        t << "\xe4\xb8\xad\x1b[2b";
        CHECK_EQ(t.row(0), std::string("\xe4\xb8\xad\xe4\xb8\xad\xe4\xb8\xad"));
        CHECK_EQ(t.ccol(), 6);
    }
    {
        // Autowrap off: a wide character that does not fit backs up to the
        // last two columns (kitty / WezTerm), never leaving half a cell.
        T t(6, 2);
        t << "\x1b[?7l\x1b[1;5H\xe4\xb8\xad\xe4\xb8\x80";
        CHECK_EQ(t.row(0), std::string("    \xe4\xb8\x80"));
        CHECK(t.cell(0, 4).wide() == Wide::Lead);
        CHECK(t.cell(0, 5).wide() == Wide::SpacerTail);
        CHECK_EQ(t.ccol(), 5);
        // Pending wrap is dropped when autowrap is turned off: the next
        // character overwrites the last column (xterm).
        T u(5, 2);
        u << "abcde\x1b[?7lX";
        CHECK_EQ(u.row(0), std::string("abcdX"));
        CHECK_EQ(u.row(1), std::string(""));
    }
    {
        T t(5, 3);
        t << "ab\x1b[1;31m\x1b#8";
        CHECK_EQ(t.row(0), std::string("EEEEE"));
        CHECK_EQ(t.row(2), std::string("EEEEE"));
        // xterm fills with default attributes and homes the cursor.
        CHECK(t.style(0, 0) == Style{});
        CHECK_EQ(t.crow(), 0);
        CHECK_EQ(t.ccol(), 0);
    }
    {
        // DCH never leaves a SpacerHead away from the last column.
        T t(5, 2);
        t << "w'\xf0\x9f\x98\x99\xf0\x9f\x98\xab\x1b[1;3H\x1b[P";
        for (int x = 0; x < 4; ++x) CHECK(t.cell(0, x).wide() != Wide::SpacerHead);
        CHECK_EQ(t.row(0), std::string("w'"));
    }
    {
        // 1049h while already on the alternate screen does nothing (xterm).
        T t(10, 2);
        t << "\x1b[?1049hab\x1b[?1049h";
        CHECK_EQ(t.row(0), std::string("ab"));
        // DECSTBM with top >= bottom is ignored entirely, cursor included.
        t << "\x1b[1;1r";
        CHECK_EQ(t.ccol(), 2);
    }
    {
        // Rectangular area operations.
        T t(6, 4);
        t << "\x1b[42;2;2;3;4$x";  // DECFRA '*'
        CHECK_EQ(t.row(1), std::string(" ***"));
        CHECK_EQ(t.row(2), std::string(" ***"));
        t << "\x1b[2;3;2;3$z";  // DECERA one cell
        CHECK_EQ(t.row(1), std::string(" * *"));
        t << "\x1b[2;2;3;4;1;4;3;1$v";  // DECCRA copy to row 4 col 3
        CHECK_EQ(t.row(3), std::string("  * *"));
    }
    {
        // Column insert/delete (DECIC / DECDC) and SL / SR.
        T t(6, 2);
        t << "abcdef\r\nghijkl\x1b[1;3H\x1b['}";
        CHECK_EQ(t.row(0), std::string("ab cde"));
        CHECK_EQ(t.row(1), std::string("gh ijk"));
        t << "\x1b['~";
        CHECK_EQ(t.row(0), std::string("abcde"));
        t << "\x1b[2 @";
        CHECK_EQ(t.row(0), std::string("cde"));
        t << "\x1b[1 A";
        CHECK_EQ(t.row(1), std::string(" ijk"));
    }
}

static void cursor_save_and_alt_screen() {
    {
        T t(20, 4);
        t << "\x1b[1;31m\x1b" "7\x1b[0m\x1b[3;3H\x1b" "8Z";
        CHECK(t.style(0, 0).has(Attr_Bold));
        CHECK(t.style(0, 0).fg == Color::indexed(1));
    }
    {
        T t(20, 4);
        t << "\x1b" "8";  // restore without save: home, default pen
        CHECK_EQ(t.crow(), 0);
        t << "\x1b[3;4H\x1b[s\x1b[H\x1b[uX";  // SCOSC / SCORC
        CHECK_EQ(t.cell(2, 3).cp(), char32_t('X'));
    }
    {
        T t(20, 4);
        t << "primary\x1b[2;5H" << "\x1b[?1049h";
        CHECK(t.t.alt_screen_active());
        CHECK_EQ(t.crow(), 1);
        CHECK_EQ(t.ccol(), 4);
        CHECK_EQ(t.row(0), std::string(""));
        t << "ALT\x1b[4;1H" << "\x1b[?1049l";
        CHECK(!t.t.alt_screen_active());
        CHECK_EQ(t.row(0), std::string("primary"));
        CHECK_EQ(t.crow(), 1);
        CHECK_EQ(t.ccol(), 4);
        t << "\x1b[?1049h";
        CHECK_EQ(t.row(0), std::string(""));
        t << "x\x1b[?47l";
        CHECK_EQ(t.row(0), std::string("primary"));
        t << "\x1b[?47h";
        CHECK_EQ(t.row(1), std::string("    x"));  // 47 does not clear
        t << "\x1b[?1047l\x1b[?47h";
        CHECK_EQ(t.row(1), std::string(""));       // 1047 cleared on exit
    }
    {
        // The alternate screen never feeds history.
        T t(10, 2);
        t << "\x1b[?1049h";
        for (int i = 0; i < 10; ++i) t << "line\r\n";
        CHECK_EQ(t.t.history_rows(), size_t(0));
    }
}

static void history() {
    {
        T t(10, 2);
        t << "a\r\nb\r\nc\r\nd";
        CHECK_EQ(t.t.history_rows(), size_t(2));
        t << "\x1b[3J";
        CHECK_EQ(t.t.history_rows(), size_t(0));
        CHECK_EQ(t.row(1), std::string("d"));
    }
    {
        T t(10, 2, 50);
        for (int i = 0; i < 200; ++i) t << ("L" + std::to_string(i) + "\r\n");
        CHECK_EQ(t.t.history_rows(), size_t(50));
        CHECK_EQ(t.t.history_text(49), std::string("L198"));
        CHECK_EQ(t.t.history_text(0), std::string("L149"));
    }
    {
        // Styles and wrap flags survive the trip into history.
        T t(5, 1);
        t << "\x1b[1;31mabcdefg\x1b[0m\r\n";
        CHECK_EQ(t.t.history_rows(), size_t(2));
        RowView r0 = t.t.history_row(0);
        CHECK(r0.wrapped());
        CHECK(r0.style(0).has(Attr_Bold));
        CHECK(r0.style(4).fg == Color::indexed(1));
        CHECK_EQ(t.t.history_text(1), std::string("fg"));
    }
}

static void hyperlinks_and_osc() {
    {
        T t(10, 2);
        for (int i = 0; i < 70000; ++i) t << ("\x1b]8;;https://x/" + std::to_string(i) + "\x07\x1b]8;;\x07");
        t << "\x1b]8;;https://last\x07L\x1b]8;;\x07M";
        const Hyperlink* h = t.t.hyperlink(t.style(0, 0).link);
        CHECK(h != nullptr);
        if (h) CHECK_EQ(h->uri, std::string("https://last"));
        CHECK_EQ(t.style(0, 1).link, 0u);
        t << "\x1b]8;id=a;https://same\x07X\x1b]8;;\x07 \x1b]8;id=a;https://same\x07Y";
        CHECK_EQ(t.style(0, 2).link, t.style(0, 4).link);
        t << "\x1b[0mZ";  // SGR 0 does not end a hyperlink
        CHECK(t.style(0, 5).link != 0);
    }
    {
        T t(10, 2);
        t << "\x1b]0;both\x07";
        CHECK_EQ(t.host.title, std::string("both"));
        CHECK_EQ(t.host.icon, std::string("both"));
        t << "\x1b]2;title\x1b\\\x1b]1;icon\x07";
        CHECK_EQ(t.t.title(), std::string("title"));
        CHECK_EQ(t.t.icon_name(), std::string("icon"));
        t << "\x1b[22t\x1b]2;tmp\x07\x1b[23t";
        CHECK_EQ(t.t.title(), std::string("title"));
        t << "\x1b]7;file://host/tmp/x\x1b\\";
        CHECK_EQ(t.host.cwd, std::string("file://host/tmp/x"));
        t << "\x1b]133;A\x07$ \x1b]133;B\x07ls\r\n\x1b]133;C\x07out\r\n\x1b]133;D;1\x07";
        CHECK_EQ(t.host.marks.size(), size_t(4));
        CHECK(t.t.history_rows() > 0 || (t.t.row(0).flags & Row_Prompt));
        t << "\x1b]52;c;aGVsbG8=\x07";
        CHECK_EQ(t.host.clipboard_writes.size(), size_t(1));
        if (!t.host.clipboard_writes.empty()) {
            CHECK_EQ(t.host.clipboard_writes[0].first, std::string("c"));
            CHECK_EQ(t.host.clipboard_writes[0].second, std::string("hello"));
        }
        t << "\x1b]9;hi there\x07\x1b]777;notify;T;B\x07";
        CHECK_EQ(t.host.notes.size(), size_t(2));
        t << "\x07";
        CHECK_EQ(t.host.bells, 1);
        // Kitty graphics commands (APC G) are the terminal's; other APCs go to the host.
        t << "\x1b_Gi=1;AAAA\x1b\\";
        CHECK_EQ(t.host.apcs.size(), size_t(0));
        t << "\x1b_Xsomething\x1b\\";
        CHECK_EQ(t.host.apcs.size(), size_t(1));
    }
}

static void wide_integrity() {
    {
        T t(10, 3);
        t << "aaaaaaaaa" << "\xe4\xb8\xad";  // wide char at the last column wraps
        CHECK_EQ(t.row(0), std::string("aaaaaaaaa"));
        CHECK(t.cell(0, 9).wide() == Wide::SpacerHead);
        CHECK_EQ(t.cell(1, 0).cp(), char32_t(0x4E2D));
        CHECK(t.t.row(0).wrapped());
    }
    {
        T t(10, 3);
        t << "\xe4\xb8\xad" << "\x1b[1;2Hx";  // overwrite the trailing half
        CHECK(t.cell(0, 0).wide() == Wide::Narrow && t.cell(0, 0).is_empty());
        CHECK_EQ(t.cell(0, 1).cp(), char32_t('x'));
    }
    {
        T t(10, 3);
        t << "\xe4\xb8\xad" << "\x1b[1;1Hx";  // overwrite the leading half
        CHECK(t.cell(0, 1).wide() == Wide::Narrow && t.cell(0, 1).is_empty());
    }
    {
        T t(10, 3);
        t << "ab\xe4\xb8\xad" "cd\x1b[1;4H\x1b[K";  // erase from the tail erases the lead
        CHECK_EQ(t.row(0), std::string("ab"));
        t << "\x1b[2;1Hab\xe4\xb8\xad" "cd\x1b[2;4H\x1b[P";  // DCH at the tail
        CHECK_EQ(t.row(1), std::string("ab cd"));
        t << "\x1b[3;1Hab\xe4\xb8\xad" "cd\x1b[3;4H\x1b[@";  // ICH splits the pair
        CHECK(t.cell(2, 2).wide() != Wide::Lead || t.cell(2, 3).wide() == Wide::SpacerTail);
        for (int y = 0; y < 3; ++y)
            for (int x = 0; x < 10; ++x) {
                const Cell& c = t.cell(y, x);
                if (c.wide() == Wide::Lead) CHECK(x + 1 < 10 && t.cell(y, x + 1).wide() == Wide::SpacerTail);
                if (c.wide() == Wide::SpacerTail) CHECK(x > 0 && t.cell(y, x - 1).wide() == Wide::Lead);
            }
    }
    {
        T t(10, 3);
        t << "\x1b[?7l" "aaaaaaaaa\xe4\xb8\xad";  // no autowrap: wide char shifts left to fit
        CHECK_EQ(t.cell(0, 8).cp(), char32_t(0x4E2D));
        CHECK(t.cell(0, 9).wide() == Wide::SpacerTail);
    }
}

int main() {
    init_test();
    wrapping();
    index_and_regions();
    horizontal_margins();
    tabs();
    charsets();
    sgr();
    editing();
    cursor_save_and_alt_screen();
    history();
    hyperlinks_and_osc();
    wide_integrity();
    return check::finish("test_screen");
}
