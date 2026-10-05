// Concurrency stress: the parser thread floods a terminal with self-describing
// lines in random chunks (split anywhere, mid-escape and mid-UTF-8) while it
// resizes, scrolls, selects, searches and publishes frames; a renderer thread
// acquires frames as fast as it can and checks every one of them.
//
// Each line is "L<8-digit number>:" plus a body generated from the number
// (lowercase letters, wide CJK, emoji, combining clusters, SGR colours,
// OSC 8 links; never a space and never 'L'), so a frame alone says what
// every complete line in it must read. Per frame the reader checks:
//  * structure: row count and widths, style and link references resolve,
//    highlights and damage are in range, sequence numbers increase;
//  * content: every complete logical line in the viewport (joined over soft
//    wraps) is exactly the line its number names, and consecutive complete
//    lines have consecutive numbers (nothing lost or duplicated by reflow,
//    eviction or the row cache);
//  * immutability: frames the reader keeps hashing the same later on.
// Run it under TSan / ASan to check the handoff itself.
//   test_frame_stress [megabytes]
#include "bropty/view.h"
#include "term_helpers.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>

using namespace bropty;

namespace {

struct Rng {
    uint64_t s;
    uint32_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return uint32_t(s >> 11);
    }
    int below(int n) { return int(next() % uint32_t(n)); }
    bool chance(int pct) { return below(100) < pct; }
};

std::string prefix(uint64_t n) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "L%08llu:", (unsigned long long)n);
    return buf;
}

// The bytes written for line n, and the text it must read as.
void line_of(uint64_t n, std::string& bytes, std::string& text) {
    Rng r{n * 0x9E3779B97F4A7C15ull + 1};
    bytes = prefix(n);
    text = bytes;
    const int len = r.below(r.chance(20) ? 160 : 40);
    bool link = false;
    for (int i = 0; i < len; ++i) {
        const int k = r.below(100);
        if (k < 6) {
            bytes += "\x1b[3" + std::to_string(r.below(8)) + (r.chance(30) ? ";1m" : "m");
        } else if (k < 8) {
            bytes += "\x1b[0m";
        } else if (k < 10) {
            bytes += link ? "\x1b]8;;\x1b\\" : "\x1b]8;;https://x.example/" + std::to_string(n) + "\x1b\\";
            link = !link;
        } else if (k < 18) {
            const char* w[] = {"\xe4\xb8\xad", "\xe6\x96\x87", "\xf0\x9f\x98\x80"};
            const char* c = w[r.below(3)];
            bytes += c;
            text += c;
        } else if (k < 22) {
            bytes += "e\xcc\x81";
            text += "e\xcc\x81";
        } else {
            const char c = char('a' + r.below(26));
            bytes += c;
            text += c;
        }
    }
    if (link) bytes += "\x1b]8;;\x1b\\";
    bytes += "\x1b[0m\r\n";
}

std::string strip_spaces(std::string s) {
    s.erase(std::remove(s.begin(), s.end(), ' '), s.end());
    return s;
}

uint64_t frame_hash(const Frame& f) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t x) {
        h ^= x;
        h *= 1099511628211ull;
    };
    mix(uint64_t(f.top_row));
    for (const auto& row : f.lines) {
        mix(row->serial);
        mix(row->flags);
        for (const Cell& c : row->cells) mix(c.bits);
        for (const Style& s : row->styles) mix(StyleHash{}(s));
        const std::string t = row->view().text(false);
        for (char c : t) mix(uint8_t(c));
    }
    for (const Highlight& hl : f.highlights) mix(uint64_t(hl.y) << 32 | uint32_t(hl.col0) << 16 | uint32_t(hl.col1));
    return h;
}

struct Reader {
    FrameChannel& ch;
    std::atomic<bool>& done;
    std::vector<std::string> errors;
    uint64_t frames = 0, lines_checked = 0, last_seq = 0;

    void error(std::string e) {
        if (errors.size() < 10) errors.push_back(std::move(e));
    }

    // Returns the number of the last complete line in the frame (0 if none).
    uint64_t validate(const Frame& f) {
        const std::string where = "frame " + std::to_string(f.seq) + ": ";
        if (f.seq <= last_seq && frames) error(where + "sequence went backwards");
        last_seq = f.seq;
        if (int(f.lines.size()) != f.rows || int(f.damage.size()) != f.rows) {
            error(where + "row count");
            return 0;
        }
        if (f.top_row < f.first_row || f.top_row > f.screen_top_row || !f.palette) error(where + "viewport");
        for (int y = 0; y < f.rows; ++y) {
            const FrameRow* row = f.lines[size_t(y)].get();
            if (!row || row->cols != f.cols || int(row->cells.size()) != f.cols) {
                error(where + "row " + std::to_string(y) + " width");
                return 0;
            }
            for (const Cell& c : row->cells) {
                if (c.style >= row->styles.size()) {
                    error(where + "style index out of range");
                    return 0;
                }
                const uint32_t link = row->styles[c.style].link;
                if (link && !row->link_uri(link)) error(where + "unresolved link");
            }
        }
        int prev_y = -1;
        for (const Highlight& h : f.highlights) {
            if (h.y < 0 || h.y >= f.rows || h.col0 < 0 || h.col0 >= h.col1 || h.col1 > f.cols)
                error(where + "highlight out of range");
            if (h.y < prev_y) error(where + "highlights unsorted");
            prev_y = h.y;
        }
        // The segment holding the cursor (being written) and anything below it
        // are not complete.
        return check_lines(where, f.cursor_y >= 0 ? f.cursor_y : f.rows,
                           [&](int y) { return f.lines[size_t(y)]->view(); });
    }

    // Logical lines in rows [0, limit): rows joined over soft wraps; a
    // segment cut off at `limit` is not complete.
    template <class RowAt>
    uint64_t check_lines(const std::string& where, int limit, RowAt&& row_at) {
        uint64_t last = 0;
        std::string text;
        int start = 0;
        for (int y = 0; y < limit; ++y) {
            const RowView v = row_at(y);
            text += v.text(false);
            if (v.wrapped()) continue;
            const int first = start;
            start = y + 1;
            std::string got = strip_spaces(std::move(text));
            text.clear();
            if (got.empty()) {
                error(where + "blank row " + std::to_string(y) + " above the cursor");
                continue;
            }
            if (got[0] != 'L') {
                if (first != 0) error(where + "line at row " + std::to_string(first) + " does not start with its number");
                continue;  // the view starts inside a line
            }
            const uint64_t n = std::strtoull(got.c_str() + 1, nullptr, 10);
            std::string bytes, want;
            line_of(n, bytes, want);
            ++lines_checked;
            if (got != want) error(where + "line " + std::to_string(n) + " reads \"" + got + "\"");
            if (last && n != last + 1) error(where + "line " + std::to_string(n) + " follows " + std::to_string(last));
            last = n;
        }
        return last;
    }

    void run() {
        std::deque<std::pair<std::shared_ptr<const Frame>, uint64_t>> kept;
        std::shared_ptr<const Frame> cur;
        Rng r{7};
        for (;;) {
            const bool finished = done.load(std::memory_order_acquire);
            if (ch.has_new()) {
                auto f = ch.acquire();
                if (f && f != cur) {
                    validate(*f);
                    ++frames;
                    cur = f;
                    if (r.chance(10)) {
                        kept.emplace_back(f, frame_hash(*f));
                        if (kept.size() > 16) kept.pop_front();
                    }
                    if (r.chance(5) && !kept.empty()) {
                        const auto& [old, h] = kept[size_t(r.below(int(kept.size())))];
                        if (frame_hash(*old) != h) error("a kept frame changed");
                    }
                }
            } else if (finished) {
                break;
            } else {
                std::this_thread::yield();
            }
        }
        for (const auto& [old, h] : kept)
            if (frame_hash(*old) != h) error("a kept frame changed");
    }
};

} // namespace

int main(int argc, char** argv) {
    init_test();
#if defined(NDEBUG)
    double mb = 24;
#else
    double mb = 3;
#endif
    if (argc > 1) mb = std::atof(argv[1]);
    const size_t total = size_t(mb * 1024 * 1024);

    th::T t(40, 12, 400);
    TerminalView view(t.t);
    FrameChannel ch;
    std::atomic<bool> done{false};
    Reader reader{ch, done, {}};
    std::thread rt([&] { reader.run(); });

    Rng rng{12345};
    std::string stream, bytes, text;
    size_t pos = 0, fed = 0;
    uint64_t next_line = 1;
    uint64_t publishes = 0, step = 0;
    const bool direct = std::getenv("STRESS_DIRECT") != nullptr;
    std::deque<std::string> recent;
    std::string last_chunk;
    const uint64_t trace_from = std::getenv("STRESS_TRACE") ? std::strtoull(std::getenv("STRESS_TRACE"), nullptr, 10) : 0;
    while (fed < total) {
        while (stream.size() - pos < 8192) {
            line_of(next_line++, bytes, text);
            stream += bytes;
        }
        const size_t n = 1 + size_t(rng.below(rng.chance(10) ? 8192 : 512));
        t.t.feed(std::string_view(stream).substr(pos, n));
        if (trace_from) last_chunk = stream.substr(pos, n);
        pos += n;
        fed += n;
        if (pos > (1u << 20)) {
            stream.erase(0, pos - 1);  // keep the last byte fed (see the resize below)
            pos = 1;
        }
        const int op = rng.below(1000);
        // Random draws are sequenced one per statement: the order compilers
        // evaluate function arguments in differs, and the run must not.
        if (op < 15) {
            // Not between a line's CR and LF: after the CR the cursor is at
            // the start of the line's last row, and reflow keeps it on that
            // cell of the line, which a wider screen moves to the middle of
            // an earlier row; the LF then lands inside the same line and the
            // next line is written into it. That is what reflowing
            // terminals do (kitty keeps the cursor's place in the line the
            // same way), but it would break this test's oracle.
            if (pos > 0 && stream[pos - 1] == '\r') {
                t.t.feed(std::string_view(stream).substr(pos, 1));
                ++pos;
                ++fed;
            }
            const int cols = 10 + rng.below(70);
            const int rows = 3 + rng.below(30);
            t.t.resize(cols, rows);
        } else if (op < 60) {
            view.scroll_by(rng.below(41) - 30);
        } else if (op < 80) {
            view.scroll_to_bottom();
        } else if (op < 100) {
            const int64_t a = view.top_row() + rng.below(t.t.rows());
            const int c0 = rng.below(t.t.cols());
            const int64_t b = a + rng.below(4);
            const int c1 = 1 + rng.below(t.t.cols());
            view.selection().select_range(RowRange{RowPos{a, c0}, RowPos{b, c1}});
        } else if (op < 105) {
            view.selection().clear();
        } else if (op < 110) {
            view.search().start(std::make_shared<LiteralMatcher>(rng.chance(50) ? "e\xcc\x81" : "\xe4\xb8\xad"));
        } else if (op < 120) {
            const int64_t row = view.top_row() + rng.below(t.t.rows());
            view.set_hover(RowPos{row, rng.below(t.t.cols())});
        }
        if (view.search().active()) view.search().step(std::chrono::microseconds(50));
        publishes += view.publish(ch, rng.chance(70)) ? 1 : 0;
        if (direct) {
            // STRESS_DIRECT: check the terminal itself (history and screen
            // above the cursor) after every step, on this thread.
            ++step;
            char buf[200];
            std::snprintf(buf, sizeof buf, "step %llu op %d fed %zu bytes -> %dx%d cursor %d,%d pending %d",
                          (unsigned long long)step, op, n, t.t.cols(), t.t.rows(), t.t.cursor().row, t.t.cursor().col,
                          int(t.t.cursor().pending_wrap));
            recent.emplace_back(buf);
            if (recent.size() > 12) recent.pop_front();
            if (trace_from && step >= trace_from && step < trace_from + 12) {
                std::printf("  %s\n    chunk \"%s\"\n", buf, check::escape(last_chunk).c_str());
                for (int y = std::max(0, t.t.cursor().row - 3); y <= t.t.cursor().row; ++y) {
                    const RowView v = t.t.row(y);
                    std::printf("    %d |%s|%s\n", y, v.text(false).c_str(), v.wrapped() ? "~" : "");
                }
            }
            const int64_t first = t.t.first_row();
            const int limit = int(t.t.screen_top_row() + t.t.cursor().row - first);
            Reader d{ch, done, {}};
            d.check_lines("step " + std::to_string(step) + " op " + std::to_string(op) + ": ", limit,
                          [&](int y) { return t.t.row_at(first + y); });
            if (!d.errors.empty()) {
                for (const std::string& e : d.errors) std::printf("  direct: %s\n", e.c_str());
                for (const std::string& e : recent) std::printf("  %s\n", e.c_str());
                std::printf("  %dx%d, screen %lld, cursor %d,%d\n", t.t.cols(), t.t.rows(),
                            (long long)t.t.screen_top_row(), t.t.cursor().row, t.t.cursor().col);
                for (int64_t r = std::max(first, first + limit - 30); r <= first + limit; ++r) {
                    const RowView v = t.t.row_at(r);
                    std::printf("  %lld%s |%s|%s\n", (long long)r, r >= t.t.screen_top_row() ? "*" : " ",
                                v.text(false).c_str(), v.wrapped() ? "~" : "");
                }
                CHECK(false);
                break;
            }
        }
    }
    // Finish the line in progress, show the bottom (wide enough that the last
    // line starts in view), publish the last frame.
    t.t.feed(std::string_view(stream).substr(pos));
    t.t.resize(200, 40);
    view.scroll_to_bottom();
    view.publish(ch);
    done.store(true, std::memory_order_release);
    rt.join();

    for (const std::string& e : reader.errors) std::printf("  reader: %s\n", e.c_str());
    CHECK(reader.errors.empty());
    CHECK(reader.frames > 10);
    CHECK(reader.lines_checked > 100);
    // The final frame (now the main thread's to read) ends on the last line.
    auto last = ch.acquire();
    CHECK(last != nullptr);
    if (last) {
        Reader check_last{ch, done, {}};
        CHECK_EQ(check_last.validate(*last), next_line - 1);
        CHECK(check_last.errors.empty());
        for (const std::string& e : check_last.errors) std::printf("  final: %s\n", e.c_str());
    }
    std::printf("  %.1f MB fed, %llu publishes, %llu frames read, %llu lines checked\n", double(fed) / (1024 * 1024),
                (unsigned long long)publishes, (unsigned long long)reader.frames,
                (unsigned long long)reader.lines_checked);
    return check::finish("test_frame_stress");
}
