// Absolute row numbers (history_first_row, row_at, row_numbering) as rows
// are evicted, cleared and reflowed; and row stamps as content serials:
// equal stamps mean equal content across scrolling, both screens and
// resizes, a scrolled row keeps its stamp, and two readers can share one
// terminal.
#include "term_helpers.h"

#include <map>
#include <random>

using namespace bropty;

namespace {

// Everything a row shows: cells, their styles, cluster tails, wrap flag.
std::string content(const RowView& v) {
    std::string s;
    if (!v.cells) return "<none>";
    for (int x = 0; x < v.cols; ++x) {
        const Cell& c = v.cells[x];
        const Style& st = v.style(x);
        s += std::to_string(c.bits) + ":" + std::to_string(st.fg.packed()) + "/" + std::to_string(st.bg.packed()) +
             "/" + std::to_string(st.attrs) + "/" + (st.link ? "L" : "");
        if (c.has_cluster() && v.clusters)
            for (char32_t t : v.clusters->find(x)) s += "+" + std::to_string(uint32_t(t));
        s += ' ';
    }
    s += "|" + std::to_string(v.flags);
    return s;
}

void numbers_survive_eviction() {
    th::T t(20, 4, 10);
    std::map<int64_t, std::string> seen;  // absolute row -> text
    int next = 0;
    uint64_t numbering = t.t.row_numbering();
    int64_t first = t.t.history_first_row();
    auto check_rows = [&] {
        CHECK(t.t.history_first_row() >= first);  // never moves back
        first = t.t.history_first_row();
        CHECK_EQ(t.t.first_row(), first);
        CHECK_EQ(t.t.row_numbering(), numbering);
        // Rows still held keep their number and text; history row i is
        // absolute row history_first_row() + i.
        for (size_t i = 0; i < t.t.history_rows(); ++i)
            CHECK_EQ(t.t.history_text(i), t.t.row_at(first + int64_t(i)).text());
        for (int64_t abs = t.t.first_row(); abs < t.t.screen_top_row(); ++abs) {
            const std::string text = t.t.row_at(abs).text();
            auto [it, fresh] = seen.emplace(abs, text);
            if (!fresh) CHECK_EQ(text, it->second);
        }
        // Evicted rows read empty.
        CHECK(t.t.row_at(first - 1).cells == nullptr);
    };
    for (int round = 0; round < 6; ++round) {
        for (int i = 0; i < 7; ++i) t << "L" + std::to_string(next++) + "\r\n";
        check_rows();
    }
    CHECK(first > 20);  // capacity 10: plenty evicted
    // ED 3 clears history: numbers move on, they are not reused.
    const int64_t top = t.t.screen_top_row();
    t << "\x1b[3J";
    CHECK_EQ(t.t.history_rows(), size_t(0));
    CHECK_EQ(t.t.history_first_row(), top);
    CHECK_EQ(t.t.screen_top_row(), top);
    check_rows();
    for (int i = 0; i < 9; ++i) t << "M" + std::to_string(i) + "\r\n";
    check_rows();
    CHECK_EQ(t.t.row_at(top).text(), t.t.history_text(0));

    // The alternate screen has no history of its own; the primary's keeps
    // its numbers meanwhile.
    const int64_t hfirst = t.t.history_first_row();
    const size_t hrows = t.t.history_rows();
    const std::string h0 = t.t.history_text(0);
    t << "\x1b[?1049h";
    CHECK(t.t.alt_screen_active());
    CHECK_EQ(t.t.first_row(), t.t.screen_top_row());
    CHECK_EQ(t.t.history_first_row(), hfirst);
    for (int i = 0; i < 12; ++i) t << "alt " + std::to_string(i) + "\r\n";
    CHECK_EQ(t.t.history_first_row(), hfirst);
    CHECK_EQ(t.t.history_rows(), hrows);
    CHECK_EQ(t.t.history_text(0), h0);
    CHECK_EQ(t.t.row_numbering(), numbering);
    t << "\x1b[?1049l";

    // A resize renumbers (the reflow): row_numbering() says so. Within one
    // numbering, rows are stable again.
    t.t.resize(9, 4);
    CHECK(t.t.row_numbering() != numbering);
    numbering = t.t.row_numbering();
    seen.clear();
    first = t.t.history_first_row();
    check_rows();
    for (int i = 0; i < 20; ++i) t << "after resize " + std::to_string(i) + "\r\n";
    check_rows();
    // A resize to the same size is no resize.
    t.t.resize(9, 4);
    CHECK_EQ(t.t.row_numbering(), numbering);
}

struct Reader {
    std::map<uint64_t, std::string>& serials;  // shared: stamps are global
    void read(Terminal& t, int& checks) {
        for (int y = 0; y < t.rows(); ++y) {
            const uint64_t s = t.row_stamp(y);
            CHECK(s != 0);
            CHECK_EQ(t.row_serial(t.screen_top_row() + y), s);
            const std::string c = content(t.row(y));
            auto [it, fresh] = serials.emplace(s, c);
            if (!fresh && it->second != c) {
                CHECK_EQ(it->second, c);  // equal stamps, different content
                return;
            }
            ++checks;
        }
        t.advance_generation();
    }
};

void stamps_are_serials() {
    std::mt19937 rng(1234);
    auto pick = [&](int n) { return int(rng() % uint32_t(n)); };
    th::T t(16, 6, 50);
    std::map<uint64_t, std::string> serials;
    Reader a{serials}, b{serials};
    int checks = 0;
    const char* ops[] = {"\r\n", "\x1b[2J", "\x1b[H", "\x1b[2;4r", "\x1b[r", "\x1b[L", "\x1b[M",
                         "\x1b[S", "\x1b[T", "\x1bM", "\x1b[3X", "\x1b[?1049h", "\x1b[?1049l", "\x1b[31m",
                         "\x1b[0m", "\x1b[4P", "\x1b[2@", "\x1b[K", "\xE4\xB8\xAD", "\x1b]8;;u\x1b\\x\x1b]8;;\x1b\\",
                         "e\xCC\x81", "\x1b[5;5H", "\x1b[?1047h", "\x1b[?1047l"};
    for (int step = 0; step < 4000; ++step) {
        const int k = pick(10);
        if (k < 4) {
            std::string w;
            for (int i = 0, n = 1 + pick(12); i < n; ++i) w.push_back(char('a' + pick(26)));
            t << w;
        } else if (k < 9) {
            t << ops[pick(int(sizeof ops / sizeof *ops))];
        } else if (pick(8) == 0) {
            t.t.resize(8 + pick(16), 3 + pick(6));
        }
        // Two readers at different times, each advancing after its read.
        if (step % 3 == 0) a.read(t.t, checks);
        if (step % 7 == 0) b.read(t.t, checks);
    }
    CHECK(checks > 10000);
    CHECK(serials.size() > 1000);  // content changed often: many serials

    // A scroll moves rows with their stamps; only the new row gets a new one.
    th::T s(10, 4, 100);
    s << "a\r\nb\r\nc\r\nd";
    std::vector<uint64_t> before;
    for (int y = 0; y < 4; ++y) before.push_back(s.t.row_stamp(y));
    s.t.advance_generation();
    s << "\r\ne";
    for (int y = 0; y < 3; ++y) CHECK_EQ(s.t.row_stamp(y), before[size_t(y) + 1]);
    for (uint64_t old : before) CHECK(s.t.row_stamp(3) != old);
    // The primary and alternate screens never share a stamp.
    std::map<uint64_t, int> owner;
    for (int y = 0; y < 4; ++y) owner[s.t.row_stamp(y)] = 0;
    s.t.advance_generation();
    s << "\x1b[?1049h";
    for (int y = 0; y < 4; ++y) CHECK(!owner.count(s.t.row_stamp(y)));
}

} // namespace

int main() {
    init_test();
    numbers_survive_eviction();
    stamps_are_serials();
    return check::finish("test_row_numbers");
}
