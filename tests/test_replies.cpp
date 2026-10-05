// Everything the terminal answers: device attributes, status reports, mode
// reports, kitty keyboard flags, DECRQSS, XTGETTCAP, color and clipboard queries.
#include "term_helpers.h"

using namespace bropty;
using th::T;

int main() {
    init_test();
    T t(40, 10);

    // 4: sixel graphics (on by default).
    t << "\x1b[c";
    CHECK_EQ(t.reply(), std::string("\x1b[?62;4;22;52c"));
    t << "\x1b[0c\x1b[1c";
    CHECK_EQ(t.reply(), std::string("\x1b[?62;4;22;52c"));
    {
        TerminalOptions o;
        o.graphics.sixel = false;
        T plain(o);
        plain << "\x1b[c";
        CHECK_EQ(plain.reply(), std::string("\x1b[?62;22;52c"));
    }
    t << "\x1b[>c";
    CHECK_EQ(t.reply(), std::string("\x1b[>1;10;0c"));
    t << "\x1b[=c";
    CHECK_EQ(t.reply(), std::string("\x1bP!|00000000\x1b\\"));
    t << "\x1b[>q";
    CHECK_EQ(t.reply().substr(0, 11), std::string("\x1bP>|bropty("));

    t << "\x1b[3;4H\x1b[6n";
    CHECK_EQ(t.reply(), std::string("\x1b[3;4R"));
    t << "\x1b[?6n";
    CHECK_EQ(t.reply(), std::string("\x1b[?3;4R"));
    t << "\x1b[5n";
    CHECK_EQ(t.reply(), std::string("\x1b[0n"));
    t << "\x1b[2;5r\x1b[?6h\x1b[2;2H\x1b[6n";  // origin-relative CPR
    CHECK_EQ(t.reply(), std::string("\x1b[2;2R"));
    t << "\x1b[?6l\x1b[r";

    // DECRQM
    t << "\x1b[?2004$p";
    CHECK_EQ(t.reply(), std::string("\x1b[?2004;2$y"));
    t << "\x1b[?2004h\x1b[?2004$p";
    CHECK_EQ(t.reply(), std::string("\x1b[?2004;1$y"));
    t << "\x1b[?7$p\x1b[?25$p\x1b[?9999$p\x1b[4$p\x1b[4h\x1b[4$p\x1b[4l";
    CHECK_EQ(t.reply(), std::string("\x1b[?7;1$y\x1b[?25;1$y\x1b[?9999;0$y\x1b[4;2$y\x1b[4;1$y"));
    t << "\x1b[?1049h\x1b[?1049$p\x1b[?1049l\x1b[?1049$p";
    CHECK_EQ(t.reply(), std::string("\x1b[?1049;1$y\x1b[?1049;2$y"));
    t << "\x1b[?2026h\x1b[?2026$p";
    CHECK_EQ(t.reply(), std::string("\x1b[?2026;1$y"));
    CHECK(t.t.modes().synchronized_output);
    t << "\x1b[?2026l";

    // Mouse modes.
    t << "\x1b[?1002h\x1b[?1006h";
    CHECK(t.t.modes().mouse_tracking == MouseTracking::Button);
    CHECK(t.t.modes().mouse_encoding == MouseEncoding::Sgr);
    t << "\x1b[?1000l";  // resetting a different tracking mode leaves 1002 alone
    CHECK(t.t.modes().mouse_tracking == MouseTracking::Button);
    t << "\x1b[?1002l";
    CHECK(t.t.modes().mouse_tracking == MouseTracking::None);

    // Kitty keyboard protocol flag stack (per screen).
    t << "\x1b[?u";
    CHECK_EQ(t.reply(), std::string("\x1b[?0u"));
    t << "\x1b[>1u\x1b[>5u\x1b[?u";
    CHECK_EQ(t.reply(), std::string("\x1b[?5u"));
    CHECK_EQ(t.t.kitty_keyboard_flags(), 5u);
    t << "\x1b[=2;2u\x1b[?u";
    CHECK_EQ(t.reply(), std::string("\x1b[?7u"));
    t << "\x1b[=1;3u\x1b[?u";
    CHECK_EQ(t.reply(), std::string("\x1b[?6u"));
    t << "\x1b[?1049h\x1b[?u";
    CHECK_EQ(t.reply(), std::string("\x1b[?0u"));
    t << "\x1b[?1049l\x1b[<u\x1b[?u";
    CHECK_EQ(t.reply(), std::string("\x1b[?1u"));
    t << "\x1b[<10u\x1b[?u";
    CHECK_EQ(t.reply(), std::string("\x1b[?0u"));

    // DECRQSS
    t << "\x1b[0;1;4:3;38;5;200;48;2;1;2;3m\x1bP$qm\x1b\\";
    CHECK_EQ(t.reply(), std::string("\x1bP1$r0;1;4:3;38;5;200;48;2;1;2;3m\x1b\\"));
    t << "\x1b[0;31;102m\x1bP$qm\x1b\\\x1b[0m";
    CHECK_EQ(t.reply(), std::string("\x1bP1$r0;31;102m\x1b\\"));
    t << "\x1b[2;9r\x1bP$qr\x1b\\\x1b[r";
    CHECK_EQ(t.reply(), std::string("\x1bP1$r2;9r\x1b\\"));
    t << "\x1b[6 q\x1bP$q q\x1b\\";
    CHECK_EQ(t.reply(), std::string("\x1bP1$r6 q\x1b\\"));
    CHECK(t.t.cursor().shape == CursorShape::Bar);
    CHECK(!t.t.cursor().blink);
    t << "\x1bP$qzz\x1b\\";
    CHECK_EQ(t.reply(), std::string("\x1bP0$r\x1b\\"));

    // XTGETTCAP: "TN" = 544E, "Co" = 436F, "xx" = 7878.
    t << "\x1bP+q544E;436F;7878\x1b\\";
    CHECK_EQ(t.reply(), std::string("\x1bP1+r544E=787465726D2D323536636F6C6F72\x1b\\"
                                    "\x1bP1+r436F=323536\x1b\\\x1bP0+r7878\x1b\\"));

    // OSC color queries mirror the request's terminator.
    t << "\x1b]4;1;?\x07";
    CHECK_EQ(t.reply(), std::string("\x1b]4;1;rgb:cdcd/3131/3131\x07"));
    t << "\x1b]4;1;#ff0000\x1b\\\x1b]4;1;?\x1b\\";
    CHECK_EQ(t.reply(), std::string("\x1b]4;1;rgb:ffff/0000/0000\x1b\\"));
    t << "\x1b]104;1\x07\x1b]4;1;?\x07";
    CHECK_EQ(t.reply(), std::string("\x1b]4;1;rgb:cdcd/3131/3131\x07"));
    t << "\x1b]10;?\x07\x1b]11;rgb:1/2/3\x07\x1b]11;?\x07";
    CHECK_EQ(t.reply(), std::string("\x1b]10;rgb:cccc/cccc/cccc\x07\x1b]11;rgb:1111/2222/3333\x07"));
    t << "\x1b]111\x07\x1b]11;?\x07";
    CHECK_EQ(t.reply(), std::string("\x1b]11;rgb:0c0c/0c0c/0c0c\x07"));
    CHECK(t.host.palette_changes >= 3);

    // OSC 52 query: refused unless the host answers.
    t << "\x1b]52;c;?\x07";
    CHECK_EQ(t.reply(), std::string(""));
    t.host.clipboard_content = std::string("hi!");
    t << "\x1b]52;c;?\x07";
    CHECK_EQ(t.reply(), std::string("\x1b]52;c;aGkh\x07"));

    // XTWINOPS size reports.
    t << "\x1b[18t";
    CHECK_EQ(t.reply(), std::string("\x1b[8;10;40t"));
    t.t.set_cell_pixel_size(8, 16);
    t << "\x1b[14t\x1b[16t";
    CHECK_EQ(t.reply(), std::string("\x1b[4;160;320t\x1b[6;16;8t"));

    // ENQ answerback.
    {
        TerminalOptions o = th::opts(10, 2);
        o.answerback = "bropty";
        Terminal term(o);
        th::Capture cap;
        term.set_host(&cap);
        term.feed("\x05");
        CHECK_EQ(cap.out, std::string("bropty"));
    }
    return check::finish("test_replies");
}
