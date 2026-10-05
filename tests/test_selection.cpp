// Selection: gestures (character, word, line, block, OSC 133 zone), plain
// and HTML extraction, and how a selection follows its text through output,
// scrolling, eviction, overwrites and reflow.
#include "bropty/view.h"
#include "term_helpers.h"

using namespace bropty;

namespace {

struct V {
    th::T t;
    TerminalView view;
    V(int cols, int rows, size_t sb = 1000) : t(cols, rows, sb), view(t.t) {}
    V& operator<<(std::string_view s) {
        t << s;
        view.sync();
        return *this;
    }
    Selection& sel() { return view.selection(); }
    // Absolute position of screen cell (y, x).
    RowPos at(int y, int x) const { return RowPos{t.t.screen_top_row() + y, x}; }
    std::string text() { return sel().text(); }
};

void character_and_wrap() {
    V v(10, 4);
    v << "hello world";  // wraps after "hello worl"
    v.sel().start(v.at(0, 0), SelectionMode::Character);
    v.sel().extend(v.at(0, 4), true);
    CHECK_EQ(v.text(), std::string("hello"));
    // Across the soft wrap: no newline.
    v.sel().extend(v.at(1, 0), true);
    CHECK_EQ(v.text(), std::string("hello world"));
    // Dragging backwards from the anchor.
    v.sel().start(v.at(1, 0), SelectionMode::Character, true);
    v.sel().extend(v.at(0, 6), false);
    CHECK_EQ(v.text(), std::string("world"));
    // A press and release in one cell half selects nothing.
    v.sel().start(v.at(0, 2), SelectionMode::Character);
    CHECK(v.sel().range().empty());
    CHECK_EQ(v.text(), std::string(""));
}

void hard_breaks_and_trailing_blanks() {
    V v(20, 5);
    v << "foo   \r\nbar\r\n\r\nbaz";
    v.sel().start(v.at(0, 0), SelectionMode::Character);
    v.sel().extend(v.at(3, 19), true);
    // Trailing blanks of each line dropped; the empty line stays one newline.
    CHECK_EQ(v.text(), std::string("foo\nbar\n\nbaz"));
    // Ending past the end of a line (xterm): the newline is included.
    v.sel().start(v.at(1, 0), SelectionMode::Character);
    v.sel().extend(v.at(1, 15), true);
    CHECK_EQ(v.text(), std::string("bar"));
    v.sel().extend(v.at(2, 0), false);
    CHECK_EQ(v.text(), std::string("bar\n"));
    TextOptions crlf;
    crlf.newline = "\r\n";
    v.sel().start(v.at(0, 0), SelectionMode::Character);
    v.sel().extend(v.at(1, 2), true);
    CHECK_EQ(v.sel().text(crlf), std::string("foo\r\nbar"));
    // Interior spaces are content.
    V w(20, 2);
    w << "a  b";
    w.sel().start(w.at(0, 0), SelectionMode::Character);
    w.sel().extend(w.at(0, 19), true);
    CHECK_EQ(w.text(), std::string("a  b"));
}

void wide_and_clusters() {
    V v(20, 3);
    v << "a\xe4\xb8\xad" "b";  // a 中 b: 中 is cols 1-2
    // Starting on the right half (spacer) of 中 still takes it whole.
    v.sel().start(v.at(0, 2), SelectionMode::Character);
    v.sel().extend(v.at(0, 3), true);
    CHECK_EQ(v.text(), std::string("\xe4\xb8\xad" "b"));
    // Ending between its halves takes it whole too.
    v.sel().start(v.at(0, 0), SelectionMode::Character);
    v.sel().extend(v.at(0, 1), true);
    CHECK_EQ(v.text(), std::string("a\xe4\xb8\xad"));
    // A decomposed cluster and a ZWJ emoji sequence come out whole.
    V c(20, 3);
    c << "e\xcc\x81x \xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x92\xbb!";
    c.sel().start(c.at(0, 0), SelectionMode::Character);
    c.sel().extend(c.at(0, 0), true);
    CHECK_EQ(c.text(), std::string("e\xcc\x81"));
    c.sel().start(c.at(0, 4), SelectionMode::Word);  // the emoji's right half
    CHECK_EQ(c.text(), std::string("\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x92\xbb"));
}

void words() {
    V v(40, 3);
    v << "foo bar.baz/qux, end  x";
    v.sel().start(v.at(0, 5), SelectionMode::Word);  // in "bar.baz/qux"
    CHECK_EQ(v.text(), std::string("bar.baz/qux"));
    v.sel().start(v.at(0, 15), SelectionMode::Word);  // the comma
    CHECK_EQ(v.text(), std::string(","));
    v.sel().start(v.at(0, 21), SelectionMode::Word);  // the run of two spaces
    CHECK_EQ(v.sel().range().end.col - v.sel().range().start.col, 2);
    // Extending by words.
    v.sel().start(v.at(0, 1), SelectionMode::Word);
    v.sel().extend(v.at(0, 18));
    CHECK_EQ(v.text(), std::string("foo bar.baz/qux, end"));
    // Configurable word characters.
    v.sel().set_word_chars(U"_");
    v.sel().start(v.at(0, 5), SelectionMode::Word);
    CHECK_EQ(v.text(), std::string("bar"));
    // Words continue across soft wraps but not hard breaks.
    V w(10, 4);
    w << "aaaaaaaaaabbbb cc\r\ndd";
    w.sel().start(w.at(1, 1), SelectionMode::Word);
    CHECK_EQ(w.text(), std::string("aaaaaaaaaabbbb"));
    w.sel().start(w.at(2, 0), SelectionMode::Word);
    CHECK_EQ(w.text(), std::string("dd"));
    // Non-ASCII letters are word characters; quotes are not.
    V u(30, 2);
    u << "\xe2\x80\x9c" "caf\xc3\xa9" "\xe2\x80\x9d x";
    u.sel().start(u.at(0, 2), SelectionMode::Word);
    CHECK_EQ(u.text(), std::string("caf\xc3\xa9"));
}

void lines_and_blocks() {
    V v(10, 6);
    v << "0123456789abc\r\nsecond\r\nthird";
    v.sel().start(v.at(1, 2), SelectionMode::Line);  // on the wrapped row of line 1
    CHECK_EQ(v.text(), std::string("0123456789abc"));
    v.sel().extend(v.at(2, 0));
    CHECK_EQ(v.text(), std::string("0123456789abc\nsecond"));

    V b(10, 4);
    b << "abcdef\r\nghijkl\r\nmn";
    b.sel().start(b.at(0, 1), SelectionMode::Block);
    b.sel().extend(b.at(2, 3));
    CHECK(b.sel().is_block());
    CHECK_EQ(b.text(), std::string("bcd\nhij\nn"));
    int c0 = 0, c1 = 0;
    CHECK(b.sel().row_span(b.at(1, 0).row, c0, c1));
    CHECK_EQ(c0, 1);
    CHECK_EQ(c1, 4);
    // Wide characters straddling a block edge are taken whole.
    V wb(10, 3);
    wb << "\xe4\xb8\xad\xe6\x96\x87x\r\nabcde";
    wb.sel().start(wb.at(0, 1), SelectionMode::Block);
    wb.sel().extend(wb.at(1, 2));
    CHECK_EQ(wb.text(), std::string("\xe4\xb8\xad\xe6\x96\x87\nbc"));

    V all(10, 3);
    all << "one\r\ntwo";
    all.sel().select_all();
    CHECK_EQ(all.text(), std::string("one\ntwo"));
}

const char* kPromptCycle =
    "\x1b]133;A\x07$ \x1b]133;B\x07ls -l\r\n\x1b]133;C\x07out1\r\n\r\nout2\r\n\x1b]133;D;0\x07"
    "\x1b]133;A\x07$ \x1b]133;B\x07" "echo hi\r\n\x1b]133;C\x07hi\r\n\x1b]133;D;0\x07"
    "\x1b]133;A\x07$ \x1b]133;B\x07";

void zones() {
    V v(30, 12);
    v << kPromptCycle;
    // Rows: 0 "$ ls -l", 1 "out1", 2 "", 3 "out2", 4 "$ echo hi", 5 "hi", 6 "$ "
    CHECK(v.t.style(1, 0).zone == Zone::Output);
    CHECK(v.t.style(0, 0).zone == Zone::Prompt);
    CHECK(v.t.style(0, 2).zone == Zone::Input);
    CHECK(v.sel().select_zone(v.at(1, 1)));
    CHECK_EQ(v.text(), std::string("out1\n\nout2"));
    CHECK(v.sel().select_zone(v.at(2, 0)));  // the blank line inside the output
    CHECK_EQ(v.text(), std::string("out1\n\nout2"));
    CHECK(v.sel().select_zone(v.at(0, 3)));
    CHECK_EQ(v.text(), std::string("ls -l"));
    CHECK(v.sel().select_zone(v.at(4, 0)));
    CHECK_EQ(v.text(), std::string("$ "));
    // A command's output from its prompt or input.
    CHECK(v.sel().select_output(v.at(0, 0)));
    CHECK_EQ(v.text(), std::string("out1\n\nout2"));
    CHECK(v.sel().select_output(v.at(4, 4)));
    CHECK_EQ(v.text(), std::string("hi"));
    CHECK(!v.sel().select_output(v.at(6, 0)));  // the current prompt has none yet
    CHECK(v.sel().select_last_output());
    CHECK_EQ(v.text(), std::string("hi"));
    CHECK(!v.sel().select_zone(v.at(8, 5)));  // past everything
    // The zones survive into history and through reflow.
    v << std::string(15, '\n');
    v.view.sync();
    CHECK(v.t.t.history_rows() > 0);
    CHECK(v.sel().select_output(RowPos{v.t.t.first_row(), 0}));
    CHECK_EQ(v.text(), std::string("out1\n\nout2"));
    v.t.t.resize(12, 12);
    v.view.sync();
    CHECK_EQ(v.text(), std::string("out1\n\nout2"));
    // Prompt navigation.
    v.view.scroll_to_row(v.t.t.first_row());
    CHECK(v.view.scroll_to_prompt(false));
    CHECK(v.t.t.row_at(v.view.top_row()).text().rfind("$ echo", 0) == 0);
    CHECK(v.view.scroll_to_prompt(true));
    CHECK(v.t.t.row_at(v.view.top_row()).text().rfind("$ ls", 0) == 0);
}

void follows_text() {
    V v(20, 4, 100);
    v << "keep me\r\nother";
    v.sel().start(v.at(0, 0), SelectionMode::Line);
    CHECK_EQ(v.text(), std::string("keep me"));
    // New output scrolls it into history: still selected, same text.
    v << "\r\n1\r\n2\r\n3\r\n4\r\n5";
    CHECK(v.sel().active());
    CHECK(v.sel().range().start.row < v.t.t.screen_top_row());
    CHECK_EQ(v.text(), std::string("keep me"));
    // Reflow (narrower and back) keeps it on the same characters.
    v.t.t.resize(4, 4);
    v.view.sync();
    CHECK_EQ(v.text(), std::string("keep me"));
    v.t.t.resize(30, 6);
    v.view.sync();
    CHECK_EQ(v.text(), std::string("keep me"));

    // Overwriting the selected text clears the selection...
    V o(20, 4);
    o << "abc\r\ndef";
    o.sel().start(o.at(0, 0), SelectionMode::Line);
    o << "\x1b[1;1Hxyz";
    CHECK(!o.sel().active());
    // ...rewriting identical text does not...
    o.sel().start(o.at(1, 0), SelectionMode::Line);
    o << "\x1b[2;1Hdef";
    CHECK(o.sel().active());
    // ...nor does output elsewhere on the screen.
    o << "\x1b[4;1Hzzz";
    CHECK(o.sel().active());
    // A scroll region at the top scrolls into history like the full screen:
    // the text keeps its row number, so the selection stays on it.
    o << "\x1b[1;3r\x1b[3;1H\n\x1b[r";
    CHECK(o.sel().active());
    CHECK_EQ(o.text(), std::string("def"));
    // Scrolling inside a region below the top moves the text out from under
    // it: cleared.
    o << "\x1b[2J\x1b[Habc\r\ndef\r\nghi";
    o.sel().start(o.at(1, 0), SelectionMode::Line);
    CHECK_EQ(o.text(), std::string("def"));
    o << "\x1b[2;4r\x1b[4;1H\n\x1b[r";
    CHECK(!o.sel().active());

    // Eviction: the selected start falls off the front of history.
    V e(10, 2, 3);
    e << "a1\r\na2\r\na3\r\na4";
    e.sel().start(RowPos{e.t.t.first_row(), 0}, SelectionMode::Character);
    e.sel().extend(e.at(1, 1), true);
    CHECK_EQ(e.text(), std::string("a1\na2\na3\na4"));
    e << "\r\na5\r\na6";
    CHECK(e.sel().active());
    CHECK_EQ(e.text(), std::string("a2\na3\na4"));
    e << "\r\na7\r\na8\r\na9\r\nb0";
    CHECK(!e.sel().active());

    // Switching to the alternate screen drops the selection.
    V a(10, 3);
    a << "x";
    a.sel().select_all();
    a << "\x1b[?1049h";
    CHECK(!a.sel().active());
}

void html() {
    V v(40, 3);
    v << "\x1b[31mred\x1b[0m <&> \x1b[1;4mbold\x1b[0m \x1b]8;;https://e.x/?a=1&b=2\x1b\\link\x1b]8;;\x1b\\";
    v.sel().select_all();
    const std::string h = v.sel().html(v.t.t.palette());
    CHECK(h.rfind("<pre style=\"color:#", 0) == 0);
    CHECK(h.find("<span style=\"color:#") != std::string::npos);
    CHECK(h.find(">red</span>") != std::string::npos);
    CHECK(h.find(" &lt;&amp;&gt; ") != std::string::npos);
    CHECK(h.find("font-weight:bold;text-decoration-line:underline;") != std::string::npos);
    CHECK(h.find("<a href=\"https://e.x/?a=1&amp;b=2\">link</a>") != std::string::npos);
    CHECK(h.find("</pre>") != std::string::npos);
    CHECK_EQ(v.text(), std::string("red <&> bold link"));
}

} // namespace

int main() {
    init_test();
    character_and_wrap();
    hard_breaks_and_trailing_blanks();
    wide_and_clusters();
    words();
    lines_and_blocks();
    zones();
    follows_text();
    html();
    return check::finish("test_selection");
}
