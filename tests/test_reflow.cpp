// Reflow of the whole primary buffer (history + screen) on resize: targeted
// cases plus a randomized property test whose oracle is invariance of the
// logical text and of the character under the cursor.
#include "term_helpers.h"

#include <cstdint>

using namespace bropty;
using th::T;

namespace {

// Logical lines of history + screen: soft-wrapped rows joined, each line's
// trailing blanks trimmed, trailing empty lines dropped.
std::vector<std::string> logical(const Terminal& t) {
    std::vector<std::string> lines;
    std::string cur;
    auto add = [&](const RowView& v) {
        cur += v.text(false);
        if (!v.wrapped()) {
            while (!cur.empty() && cur.back() == ' ') cur.pop_back();
            lines.push_back(cur);
            cur.clear();
        }
    };
    for (size_t i = 0; i < t.history_rows(); ++i) add(t.history_row(i));
    for (int y = 0; y < t.rows(); ++y) add(t.row(y));
    if (!cur.empty()) lines.push_back(cur);
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    return lines;
}

std::string join(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& l : v) s += l + "\n";
    return s;
}

// The cluster the cursor sits after (the last printed one).
std::string before_cursor(const Terminal& t) {
    CursorState c = t.cursor();
    RowView v = t.row(c.row);
    int x = c.pending_wrap ? c.col : c.col - 1;
    if (x < 0) return "";
    if (v[x].wide() == Wide::SpacerTail && x > 0) --x;
    return th::utf8(v.cluster(x));
}

struct Rng {
    uint64_t s;
    uint32_t next() {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return uint32_t(s >> 33);
    }
    int below(int n) { return int(next() % uint32_t(n)); }
};

} // namespace

static void targeted() {
    {
        T t(10, 3);
        t << "0123456789ABCDEFGHIJ\r\nxyz";
        CHECK_EQ(t.all(), std::string("0123456789~ABCDEFGHIJ|xyz"));
        t.t.resize(20, 3);
        CHECK_EQ(t.all(), std::string("0123456789ABCDEFGHIJ|xyz"));
        CHECK_EQ(t.crow(), 1);
        CHECK_EQ(t.ccol(), 3);
        t.t.resize(5, 3);
        CHECK_EQ(t.all(), std::string("01234~56789~ABCDE~FGHIJ|xyz"));
        CHECK_EQ(t.row(2), std::string("xyz"));
        CHECK_EQ(t.crow(), 2);
        t.t.resize(10, 3);
        CHECK_EQ(t.all(), std::string("0123456789~ABCDEFGHIJ|xyz"));
    }
    {
        // A logical line straddling history and screen rejoins.
        T t(5, 2);
        t << "abcdefghijkl";
        CHECK_EQ(t.t.history_rows(), size_t(1));
        t.t.resize(12, 2);
        CHECK_EQ(t.all(), std::string("abcdefghijkl"));
        CHECK_EQ(t.t.history_rows(), size_t(0));
        CHECK_EQ(t.crow(), 0);
        CHECK_EQ(t.ccol(), 11);
        CHECK(t.t.cursor().pending_wrap);
        t << "m";
        CHECK_EQ(t.all(), std::string("abcdefghijkl~m"));
    }
    {
        // Shrinking rows pushes the top into history; growing pulls it back.
        T t(10, 5);
        t << "r0\r\nr1\r\nr2\r\nr3\r\nr4";
        t.t.resize(10, 2);
        CHECK_EQ(t.row(0), std::string("r3"));
        CHECK_EQ(t.row(1), std::string("r4"));
        CHECK_EQ(t.t.history_rows(), size_t(3));
        t.t.resize(10, 5);
        CHECK_EQ(t.row(0), std::string("r0"));
        CHECK_EQ(t.row(4), std::string("r4"));
        CHECK_EQ(t.crow(), 4);
        CHECK_EQ(t.t.history_rows(), size_t(0));
    }
    {
        // Blank rows below the cursor are dropped first when shrinking.
        T t(10, 5);
        t << "\x1b[2J\x1b[Hprompt$ ";
        t.t.resize(10, 2);
        CHECK_EQ(t.row(0), std::string("prompt$"));
        CHECK_EQ(t.crow(), 0);
        CHECK_EQ(t.ccol(), 8);
        CHECK_EQ(t.t.history_rows(), size_t(0));
    }
    {
        // Wide characters rejoin and never split.
        T t(4, 3);
        t << "a\xe4\xb8\xad\xe4\xb8\xad";
        CHECK_EQ(t.row(0), std::string("a\xe4\xb8\xad"));
        CHECK(t.cell(0, 3).wide() == Wide::SpacerHead);
        t.t.resize(6, 3);
        CHECK_EQ(t.row(0), std::string("a\xe4\xb8\xad\xe4\xb8\xad"));
        CHECK(!t.t.row(0).wrapped());
        t.t.resize(3, 3);
        CHECK_EQ(t.all(), std::string("a\xe4\xb8\xad~\xe4\xb8\xad"));  // 1 + 2 columns fill a 3-wide row
        t.t.resize(2, 3);
        CHECK_EQ(t.all(), std::string("a~\xe4\xb8\xad~\xe4\xb8\xad"));
        CHECK(t.cell(0, 1).wide() == Wide::SpacerHead);
    }
    {
        // Cursor in the middle of a wrapped line stays on its character.
        T t(10, 4);
        t << "abcdefghijklmnopqrst\x1b[2;3H";  // on 'm'
        CHECK_EQ(t.cell(t.crow(), t.ccol()).cp(), char32_t('m'));
        t.t.resize(7, 4);
        CHECK_EQ(t.cell(t.crow(), t.ccol()).cp(), char32_t('m'));
        t.t.resize(25, 4);
        CHECK_EQ(t.cell(t.crow(), t.ccol()).cp(), char32_t('m'));
        CHECK_EQ(t.ccol(), 12);
    }
    {
        // Styles, clusters and hyperlinks survive reflow through history.
        T t(6, 2);
        t << "\x1b[1;32mbold\x1b[0m e\xcc\x81 \x1b]8;;http://x\x07link\x1b]8;;\x07 end\r\n\r\n\r\n";
        t.t.resize(3, 2);
        t.t.resize(30, 2);
        std::vector<std::string> l = logical(t.t);
        CHECK(!l.empty());
        if (!l.empty()) CHECK_EQ(l[0], std::string("bold e\xcc\x81 link end"));
        // Find it in history or on screen and check its attributes.
        RowView v = t.t.history_rows() ? t.t.history_row(0) : t.t.row(0);
        CHECK(v.style(0).has(Attr_Bold));
        CHECK(v.style(0).fg == Color::indexed(2));
        CHECK(!v.style(4).has(Attr_Bold));
        CHECK_EQ(th::utf8(v.cluster(5)), std::string("e\xcc\x81"));
        CHECK(v.style(7).link != 0);
    }
    {
        // Resizing while the alternate screen is active reflows the primary,
        // and leaving restores the cursor to its character.
        T t(10, 4);
        t << "abcdefghijklmno\x1b[?1049h\x1b[2J\x1b[Halt";
        t.t.resize(5, 4);
        CHECK_EQ(t.row(0), std::string("alt"));
        t << "\x1b[?1049l";
        CHECK_EQ(t.row(0), std::string("abcde"));
        CHECK_EQ(t.row(2), std::string("klmno"));
        CHECK_EQ(t.crow(), 2);
        CHECK_EQ(t.ccol(), 4);
        CHECK(t.t.cursor().pending_wrap);
        t << "p";
        CHECK_EQ(t.row(3), std::string("p"));
    }
    {
        // History lines rewrap to the new width.
        T t(10, 2, 100);
        for (int i = 0; i < 6; ++i) t << "0123456789abcdefghij\r\n";
        CHECK_EQ(t.t.history_rows(), size_t(11));
        t.t.resize(20, 2);
        CHECK_EQ(t.t.history_rows(), size_t(5));
        CHECK_EQ(t.t.history_text(0), std::string("0123456789abcdefghij"));
        t.t.resize(5, 2);
        CHECK_EQ(t.t.history_rows(), size_t(23));
        CHECK_EQ(t.t.history_text(1), std::string("56789"));
    }
}

static void randomized() {
    const char* pieces[] = {"a", "word ", "\xe4\xb8\xad", "\xf0\x9f\x98\x80", "e\xcc\x81", "\x1b[1mB\x1b[0m",
                            "\x1b[31mR\x1b[0m", "xy", "\xf0\x9f\x87\xba\xf0\x9f\x87\xb8", "-", "0123456789"};
    int failures_before = check::g_failures;
    for (int seed = 1; seed <= 300 && check::g_failures - failures_before < 5; ++seed) {
        Rng r{uint64_t(seed) * 7919};
        int cols = 3 + r.below(20);
        int rows = 1 + r.below(8);
        T t(cols, rows, 100000);
        std::string last;
        int nlines = 1 + r.below(12);
        for (int i = 0; i < nlines; ++i) {
            int n = r.below(14);
            for (int k = 0; k < n; ++k) t << pieces[r.below(int(sizeof pieces / sizeof *pieces))];
            if (i + 1 < nlines) t << "\r\n";
        }
        t << "#";  // the cursor sits right after this marker
        std::vector<std::string> want = logical(t.t);
        for (int step = 0; step < 6; ++step) {
            int nc = 3 + r.below(24);
            int nr = 1 + r.below(10);
            std::string before = std::to_string(t.t.cols()) + "x" + std::to_string(t.t.rows()) + " " + t.all();
            t.t.resize(nc, nr);
            std::vector<std::string> got = logical(t.t);
            ++check::g_checks;
            if (got != want) {
                check::fail(__FILE__, __LINE__,
                            "seed " + std::to_string(seed) + " step " + std::to_string(step) + " -> " +
                                std::to_string(nc) + "x" + std::to_string(nr) + "\n     got  " +
                                check::show(join(got)) + "\n     want " + check::show(join(want)) +
                                "\n     before " + check::show(before) + "\n     after  " + check::show(t.all()));
                break;
            }
            ++check::g_checks;
            if (before_cursor(t.t) != "#") {
                check::fail(__FILE__, __LINE__, "seed " + std::to_string(seed) + " step " + std::to_string(step) +
                                                    ": cursor not after marker, before=" + check::show(before_cursor(t.t)));
                break;
            }
            // Structural invariants on every visible row.
            for (int y = 0; y < t.t.rows(); ++y) {
                RowView v = t.t.row(y);
                for (int x = 0; x < v.cols; ++x) {
                    if (v[x].wide() == Wide::Lead && !(x + 1 < v.cols && v[x + 1].wide() == Wide::SpacerTail)) {
                        check::fail(__FILE__, __LINE__, "orphan wide lead, seed " + std::to_string(seed));
                    }
                }
            }
        }
        // Continuing to print after the resizes behaves like a fresh line end.
        t << "$";
        ++check::g_checks;
        if (before_cursor(t.t) != "$") check::fail(__FILE__, __LINE__, "print after reflow, seed " + std::to_string(seed));
        (void)last;
    }
}

int main() {
    init_test();
    targeted();
    randomized();
    return check::finish("test_reflow");
}
