// Scrollback store: exact round trip of rows through the compact encoding,
// capacity, logical-line joining, and memory cost.
#include "bropty/scrollback.h"
#include "check.h"
#include "term_helpers.h"

#include <cstdint>

using namespace bropty;

namespace {
struct Rng {
    uint64_t s;
    uint32_t next() {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return uint32_t(s >> 33);
    }
    int below(int n) { return int(next() % uint32_t(n)); }
};
} // namespace

static void round_trip() {
    StyleTable table;
    std::vector<Style> styles(6);
    styles[1].attrs = Attr_Bold;
    styles[2].fg = Color::rgb(1, 2, 3);
    styles[2].bg = Color::indexed(200);
    styles[3].underline = Underline::Curly;
    styles[3].underline_color = Color::indexed(5);
    styles[4].link = 77777;
    styles[5].bg = Color::indexed(4);  // e.g. a BCE-erased cell
    std::vector<uint32_t> ids;
    for (const Style& s : styles) ids.push_back(table.intern(s));

    for (int seed = 1; seed <= 200; ++seed) {
        Rng r{uint64_t(seed)};
        const int cols = 2 + r.below(30);
        Scrollback sb(1000, cols);
        std::vector<Cell> row(static_cast<size_t>(cols));
        ClusterMap clusters;
        for (int x = 0; x < cols;) {
            uint32_t st = ids[size_t(r.below(int(ids.size())))];
            int kind = r.below(10);
            if (kind == 0) {
                row[size_t(x)] = Cell::blank(st);
                ++x;
            } else if (kind == 1 && x + 1 < cols) {
                row[size_t(x)] = Cell::make(0x4E00 + char32_t(r.below(100)), st, Wide::Lead);
                row[size_t(x) + 1] = Cell::make(0, st, Wide::SpacerTail);
                x += 2;
            } else if (kind == 2) {
                row[size_t(x)] = Cell::make('a' + char32_t(r.below(26)), st);
                row[size_t(x)].set_cluster(true);
                clusters.set(x, U"\u0301\u0302");
                ++x;
            } else {
                row[size_t(x)] = Cell::make(char32_t(0x21 + r.below(0x5E)), st);
                ++x;
            }
        }
        sb.push_row(row.data(), cols, Row_Wrapped, &clusters, table.data());
        CHECK_EQ(sb.rows(), size_t(1));
        RowView v = sb.row(0);
        std::string why = v.cols == cols ? "" : "cols " + std::to_string(v.cols);
        for (int x = 0; why.empty() && x < cols; ++x) {
            const Cell& a = row[size_t(x)];
            const Cell& b = v[x];
            std::string at = "seed " + std::to_string(seed) + " col " + std::to_string(x) + ": ";
            if (a.wide() != b.wide() || a.cp() != b.cp() || a.has_cluster() != b.has_cluster())
                why = at + "cell cp " + std::to_string(a.cp()) + "/" + std::to_string(b.cp()) + " wide " +
                      std::to_string(int(a.wide())) + "/" + std::to_string(int(b.wide())) + " cluster " +
                      std::to_string(a.has_cluster()) + "/" + std::to_string(b.has_cluster());
            else if (!(table.get(a.style) == v.style(x))) why = at + "style";
            else if (a.has_cluster() && v.cluster(x) != std::u32string(1, a.cp()) + U"\u0301\u0302") why = at + "cluster text";
        }
        CHECK_EQ(why, std::string());
        CHECK(v.wrapped());
        std::vector<uint32_t> links;
        sb.collect_links(links);
        bool has_link = false;
        for (const Cell& c : row) has_link |= (table.get(c.style).link == 77777);
        CHECK_EQ(!links.empty(), has_link);
    }
}

static void joining_and_capacity() {
    StyleTable table;
    Scrollback sb(10, 4);
    Cell row[4];
    for (int i = 0; i < 4; ++i) row[i] = Cell::make(char32_t('a' + i), 0);
    sb.push_row(row, 4, Row_Wrapped, nullptr, table.data());
    sb.push_row(row, 4, Row_Wrapped, nullptr, table.data());
    sb.push_row(row, 4, 0, nullptr, table.data());
    CHECK_EQ(sb.lines(), size_t(1));
    CHECK_EQ(sb.rows(), size_t(3));
    sb.set_cols(6);
    CHECK_EQ(sb.rows(), size_t(2));
    CHECK_EQ(sb.row(0).text(), std::string("abcdab"));
    CHECK_EQ(sb.row(1).text(), std::string("cdabcd"));
    sb.set_cols(4);
    for (int i = 0; i < 20; ++i) sb.push_row(row, 4, 0, nullptr, table.data());
    CHECK_EQ(sb.rows(), size_t(10));
    CHECK_EQ(sb.lines(), size_t(10));
    sb.set_max_rows(3);
    CHECK_EQ(sb.rows(), size_t(3));
    sb.clear();
    CHECK(sb.empty());
    CHECK_EQ(sb.rows(), size_t(0));

    // A runaway logical line is split instead of growing without bound.
    Scrollback big(40, 4);
    for (int i = 0; i < 100; ++i) big.push_row(row, 4, Row_Wrapped, nullptr, table.data());
    CHECK_EQ(big.rows(), size_t(40));
    CHECK(big.lines() > 1);
}

static void displayed_rows_survive() {
    // A row that wrapped early for a wide character (SpacerHead) whose
    // continuation was then overwritten with narrow text must reach history
    // as displayed, not rewrapped into one row.
    th::T t(5, 2, 100);
    t << "abcd\xe4\xb8\xad";  // 中 does not fit: SpacerHead at col 4
    CHECK(t.cell(0, 4).wide() == Wide::SpacerHead);
    t << "\x1b[2;1Hx\r\n\r\n\r\n";
    CHECK_EQ(t.t.history_rows(), size_t(3));
    CHECK_EQ(t.t.history_text(0), std::string("abcd"));
    CHECK_EQ(t.t.history_text(1), std::string("x"));
    CHECK(t.t.history_row(0)[4].wide() == Wide::SpacerHead);

    // An intact early wrap stays one logical line and keeps its SpacerHead.
    th::T u(5, 2, 100);
    u << "abcd\xe4\xb8\xad\r\n\r\n\r\n";
    CHECK_EQ(u.t.history_text(0), std::string("abcd"));
    CHECK(u.t.history_row(0).wrapped());
    CHECK(u.t.history_row(0)[4].wide() == Wide::SpacerHead);
    CHECK_EQ(u.t.history_text(1), std::string("\xe4\xb8\xad"));
}

static void memory() {
    // 10k rows of 80-column colored shell output: well under 2 MB.
    th::T t(80, 24, 10000);
    for (int i = 0; i < 10100; ++i) {
        t << "\x1b[32muser@host\x1b[0m:\x1b[34m~/src/project\x1b[0m$ ls -la file" + std::to_string(i) + ".txt\r\n";
    }
    CHECK_EQ(t.t.history_rows(), size_t(10000));
    size_t bytes = t.t.scrollback().memory_bytes();
    std::printf("  scrollback: %zu rows, %zu bytes (%.1f bytes/row)\n", t.t.history_rows(), bytes,
                double(bytes) / double(t.t.history_rows()));
    CHECK(bytes < 2u * 1024u * 1024u);
    CHECK_EQ(t.t.history_text(9999), std::string("user@host:~/src/project$ ls -la file10076.txt"));
    // Style table stays bounded however many styles scroll through.
    for (int i = 0; i < 20000; ++i) t << "\x1b[38;2;" + std::to_string(i % 256) + ";" + std::to_string(i / 256) + ";7mX";
    CHECK(t.t.styles().live() < 20000);
}

int main() {
    init_test();
    round_trip();
    joining_and_capacity();
    displayed_rows_survive();
    memory();
    return check::finish("test_scrollback");
}
