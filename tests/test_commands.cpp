// OSC 133 command records: the mark sequence, exit codes, command lines, and
// positions that stay on their text through scrolling, history eviction, ED 3
// and reflowing resizes (also while the alternate screen shows).
#include "term_helpers.h"

using namespace bropty;
using th::T;

namespace {

const std::string kA = "\x1b]133;A\x07";
const std::string kB = "\x1b]133;B\x07";
const std::string kC = "\x1b]133;C\x07";

std::string D(std::string_view params = {}) {
    return "\x1b]133;D" + (params.empty() ? std::string() : ";" + std::string(params)) + "\x07";
}

// A shell running "echo <k>": prompt, input, `lines` output lines, D;<code>.
std::string command(int k, int lines, int code = 0) {
    std::string s = kA + "$ " + kB + "echo " + std::to_string(k) + "\r\n" + kC;
    for (int i = 0; i < lines; ++i)
        s += "out-" + std::to_string(k) + "-" + std::to_string(i) + "-" + std::string(size_t(k % 4) * 9, 'x') + "\r\n";
    return s + D(std::to_string(code));
}

// Up to n characters of text from a position on, following soft wraps
// (ASCII only; empty cells read as spaces; trailing spaces trimmed).
std::string text_from(const Terminal& t, RowPos p, size_t n = 40) {
    std::string s;
    int64_t row = p.row;
    int col = p.col;
    while (s.size() < n && row < t.end_row()) {
        const RowView v = t.row_at(row);
        if (!v.cells) break;
        for (int x = col; x < v.cols && s.size() < n; ++x) {
            const Cell& c = v.cells[x];
            if (c.wide() == Wide::SpacerTail || c.wide() == Wide::SpacerHead) continue;
            s.push_back(c.cp() ? char(c.cp()) : ' ');
        }
        if (!v.wrapped()) break;
        ++row;
        col = 0;
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

// The text at every position of every record.
std::vector<std::string> snapshot(const Terminal& t) {
    std::vector<std::string> out;
    for (const CommandRecord& r : t.commands()) {
        out.push_back(text_from(t, r.prompt));
        for (const std::optional<RowPos>* o : {&r.input, &r.output, &r.end})
            out.push_back(*o ? text_from(t, **o) : std::string("-"));
    }
    return out;
}

void check_in_range(const Terminal& t) {
    for (const CommandRecord& r : t.commands()) {
        CHECK(r.prompt.row >= t.history_first_row() && r.prompt.row < t.end_row());
        for (const std::optional<RowPos>* o : {&r.input, &r.output, &r.end}) {
            if (!*o) continue;
            CHECK((*o)->row >= t.history_first_row() && (*o)->row < t.end_row());
            CHECK((*o)->col >= 0 && (*o)->col < t.cols());
            CHECK(!((**o) < r.prompt));
        }
    }
}

void sequence() {
    T t(40, 10);
    CHECK(t.t.commands().empty());
    const uint64_t v0 = t.t.commands_version();
    t << kA << "$ " << kB << "true\r\n" << kC << D("0");
    t << kA << "$ " << kB << "false\r\n" << kC << "no\r\n" << D("1");
    CHECK(t.t.commands_version() != v0);
    const auto& c = t.t.commands();
    CHECK_EQ(c.size(), size_t(2));
    if (c.size() != 2) return;
    CHECK(c[0].finished && c[1].finished);
    CHECK(c[0].exit_code == std::optional<int>(0));
    CHECK(c[1].exit_code == std::optional<int>(1));
    CHECK(c[0].prompt == (RowPos{0, 0}));
    CHECK(c[0].input == (RowPos{0, 2}));
    CHECK(c[0].output == (RowPos{1, 0}));
    CHECK(c[0].end == (RowPos{1, 0}));
    CHECK(c[1].prompt == (RowPos{1, 0}));
    CHECK(c[1].output == (RowPos{2, 0}));
    CHECK(c[1].end == (RowPos{3, 0}));
    CHECK(c[0].command_line.empty());
    CHECK(!c[0].trimmed);

    // D without a code; a D with no command open is ignored; so is a
    // non-numeric first parameter.
    t << kA << "$ " << kB << "x\r\n" << kC << D();
    CHECK_EQ(c.size(), size_t(3));
    CHECK(c.back().finished);
    CHECK(!c.back().exit_code);
    const uint64_t v1 = t.t.commands_version();
    t << D("7") << kB << kC;
    CHECK_EQ(t.t.commands_version(), v1);
    CHECK(!c.back().exit_code);
    t << kA << "$ " << kB << "y\r\n" << kC << D("aid=12") << kA << kB << kC << D("-2;aid=3");
    CHECK(!c[3].exit_code);
    CHECK(c[4].exit_code == std::optional<int>(-2));

    // A prompt that never ran anything moves to the next prompt; a command
    // that ran but sent no D is finished without a code by the next A.
    T u(40, 10);
    u << kA << "$ " << kB << "\r\n" << kA << "$ " << kB << "^C\r\n" << kA << "$ ";
    CHECK_EQ(u.t.commands().size(), size_t(1));
    CHECK(u.t.commands()[0].prompt == (RowPos{2, 0}));
    CHECK(!u.t.commands()[0].input);
    CHECK(!u.t.commands()[0].finished);
    u << kB << "sleep\r\n" << kC << "\x1b]133;A;k=s\x07" << "> ";  // a secondary prompt starts nothing
    CHECK_EQ(u.t.commands().size(), size_t(1));
    u << "\r\n" << kA << "$ ";
    CHECK_EQ(u.t.commands().size(), size_t(2));
    CHECK(u.t.commands()[0].finished);
    CHECK(!u.t.commands()[0].exit_code);
    CHECK(!u.t.commands()[0].end);
    CHECK(!u.t.commands()[1].finished);

    // Command lines: kitty's cmdline / cmdline_url, VS Code's 633;E.
    T w(40, 10);
    w << kA << "$ " << kB << "ls -l\r\n" << "\x1b]133;C;cmdline_url=ls%20-l%3B%20x\x07" << D("0");
    w << kA << "$ " << kB << "pwd\r\n" << "\x1b]133;C;cmdline=pwd\x07" << D("0");
    w << kA << "$ " << kB << "a;b\r\n" << "\x1b]633;E;a\\x3bb \\\\ c;nonce\x07" << kC << D("0");
    CHECK_EQ(w.t.commands().size(), size_t(3));
    if (w.t.commands().size() == 3) {
        CHECK_EQ(w.t.commands()[0].command_line, std::string("ls -l; x"));
        CHECK_EQ(w.t.commands()[1].command_line, std::string("pwd"));
        CHECK_EQ(w.t.commands()[2].command_line, std::string("a;b \\ c"));
    }

    // Marks on the alternate screen are ignored.
    w << "\x1b[?1049h" << kA << "$ " << kB << "z\r\n" << kC << D("0") << "\x1b[?1049l";
    CHECK_EQ(w.t.commands().size(), size_t(3));

    // ED 2 drops the finished commands drawn on the screen (not the open one).
    w << kA << "$ " << kB << "clear\r\n" << kC << "\x1b[H\x1b[2J";
    CHECK_EQ(w.t.commands().size(), size_t(1));
    CHECK(!w.t.commands()[0].finished);
    w << D("0");
    // RIS drops everything.
    w << "\x1b" "c";
    CHECK(w.t.commands().empty());

    // The list is bounded.
    T many(20, 4, 10);
    for (size_t i = 0; i < Terminal::kMaxCommands + 5; ++i) many << kA << kB << kC << D("0") << "\x1b[H";
    CHECK_EQ(many.t.commands().size(), Terminal::kMaxCommands);
}

void scrolling() {
    T t(30, 6, 1000);
    for (int k = 0; k < 4; ++k) t << command(k, 2, k);
    const std::vector<std::string> before = snapshot(t.t);
    std::vector<int64_t> rows;
    for (const CommandRecord& r : t.t.commands()) rows.push_back(r.prompt.row);
    // Push everything into history.
    for (int i = 0; i < 20; ++i) t << "filler " + std::to_string(i) + "\r\n";
    CHECK(t.t.history_rows() > 0);
    CHECK(t.t.commands()[0].prompt.row < t.t.screen_top_row());
    const std::vector<std::string> after = snapshot(t.t);
    CHECK_EQ(after.size(), before.size());
    // (The last command's end is the empty row the fillers were then written on.)
    for (size_t i = 0; i + 1 < after.size() && i + 1 < before.size(); ++i) CHECK_EQ(after[i], before[i]);
    CHECK_EQ(after.back(), std::string("filler 0"));
    for (size_t i = 0; i < rows.size(); ++i) CHECK_EQ(t.t.commands()[i].prompt.row, rows[i]);
    CHECK_EQ(text_from(t.t, t.t.commands()[2].prompt), std::string("$ echo 2"));
    CHECK_EQ(text_from(t.t, *t.t.commands()[2].output), std::string("out-2-0-" + std::string(18, 'x')));
    check_in_range(t.t);
}

void eviction() {
    T t(30, 6, 12);  // 12 rows of history
    const int total = 12;
    for (int k = 0; k < total; ++k) t << command(k, 2, 0);
    t << kA << "$ ";
    check_in_range(t.t);
    const auto& c = t.t.commands();
    CHECK(c.size() < size_t(total) + 1);
    CHECK(c.size() >= 3);
    // The survivors are the newest, in order, each still on its own text.
    int expect = total + 1 - int(c.size());
    for (const CommandRecord& r : c) {
        if (&r == &c.back()) {
            CHECK_EQ(text_from(t.t, r.prompt), std::string("$"));
            break;
        }
        if (!r.trimmed) {
            CHECK_EQ(text_from(t.t, r.prompt), "$ echo " + std::to_string(expect));
            CHECK_EQ(text_from(t.t, *r.input), "echo " + std::to_string(expect));
        } else {
            CHECK(r.prompt == (RowPos{t.t.history_first_row(), 0}));
        }
        ++expect;
    }
    // ED 3 clears history: the commands that lived only there go, and what
    // reached into it is trimmed.
    const uint64_t v = t.t.commands_version();
    t << "\x1b[3J";
    CHECK(t.t.commands_version() != v);
    check_in_range(t.t);
    for (const CommandRecord& r : t.t.commands())
        CHECK(r.prompt.row >= t.t.screen_top_row() || r.trimmed);
    CHECK(!t.t.commands().empty());
}

void resizes() {
    T t(40, 8, 1000);
    for (int k = 0; k < 9; ++k) t << command(k, 1 + k % 3, k);
    t << kA << "$ " << kB << "echo pending";
    const size_t n = t.t.commands().size();
    CHECK_EQ(n, size_t(10));
    const std::vector<std::string> before = snapshot(t.t);
    CHECK(t.t.history_rows() > 0);
    const int sizes[][2] = {{13, 8}, {57, 5}, {21, 12}, {9, 3}, {40, 8}};
    for (const auto& s : sizes) {
        t.t.resize(s[0], s[1]);
        check_in_range(t.t);
        CHECK_EQ(t.t.commands().size(), n);
        const std::vector<std::string> now = snapshot(t.t);
        CHECK(now == before);
        for (size_t i = 0; i < now.size() && i < before.size(); ++i)
            if (now[i] != before[i]) CHECK_EQ(now[i], before[i]);
    }

    // Resized while a full-screen program shows the alternate screen: the
    // primary reflows all the same, and the records go with it.
    t << "\x1b[?1049h" << "vim\r\n";
    t.t.resize(17, 9);
    t.t.resize(33, 6);
    t << "\x1b[?1049l";
    check_in_range(t.t);
    CHECK(snapshot(t.t) == before);

    // Text on the screen while the history fills: eviction during a reflow.
    T s(40, 6, 8);
    for (int k = 0; k < 4; ++k) s << command(k, 1, 0);
    s.t.resize(10, 6);  // narrower: more rows, the oldest leave history
    check_in_range(s.t);
    for (const CommandRecord& r : s.t.commands()) {
        if (r.trimmed) continue;
        const std::string p = text_from(s.t, r.prompt);
        CHECK(p.rfind("$ echo ", 0) == 0);
    }
}

} // namespace

int main() {
    init_test();
    sequence();
    scrolling();
    eviction();
    resizes();
    return check::finish("test_commands");
}
