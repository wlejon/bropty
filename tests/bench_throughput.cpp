// Parse/emulate throughput over representative output corpora. Prints MB/s
// per corpus; checks that each run left the screen in the expected state so
// the measurement cannot silently test a broken path.
//   bench_throughput [megabytes-per-corpus [corpus]]   (default 32 in Release, 4 in Debug)
// BENCH_SB=<rows> overrides the scrollback size (0 isolates the screen path).
#include "bropty/terminal.h"
#include "check.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace bropty;

namespace {

struct Rng {
    uint64_t s;
    uint32_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return uint32_t(s);
    }
    int below(int n) { return int(next() % uint32_t(n)); }
};

const char* kWords[] = {"the", "quick", "brown", "fox", "jumps", "over", "lazy", "dog", "terminal",
                        "emulator", "throughput", "compile", "warning:", "src/main.cpp", "error", "0x7ffe"};

std::string words(Rng& r, int len) {
    std::string s;
    while (int(s.size()) < len) {
        if (!s.empty()) s += ' ';
        s += kWords[r.below(16)];
    }
    s.resize(size_t(len));
    return s;
}

// Plain text lines, like cat of a source file or a log.
std::string corpus_ascii(Rng& r) {
    std::string out;
    for (int i = 0; i < 4000; ++i) {
        out += words(r, 20 + r.below(100));
        out += "\r\n";
    }
    return out;
}

// SGR-heavy coloured output (ls --color, compilers, git log --graph).
std::string corpus_sgr(Rng& r) {
    std::string out;
    for (int i = 0; i < 4000; ++i) {
        int n = 3 + r.below(6);
        for (int k = 0; k < n; ++k) {
            switch (r.below(4)) {
            case 0: out += "\x1b[" + std::to_string(31 + r.below(7)) + "m"; break;
            case 1: out += "\x1b[1;38;5;" + std::to_string(r.below(256)) + "m"; break;
            case 2:
                out += "\x1b[38;2;" + std::to_string(r.below(256)) + ";" + std::to_string(r.below(256)) + ";" +
                       std::to_string(r.below(256)) + "m";
                break;
            default: out += "\x1b[0m"; break;
            }
            out += words(r, 4 + r.below(12));
            out += ' ';
        }
        out += "\x1b[0m\r\n";
    }
    return out;
}

// Mixed scripts: CJK (wide), accented Latin, combining marks, emoji clusters.
std::string corpus_unicode(Rng& r) {
    const char* pieces[] = {"中文字符", "日本語のテキスト", "naïve café ", "e\xcc\x81", "Ελληνικά ",
                            "\xf0\x9f\x98\x80", "\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x92\xbb", "русский ",
                            "\xf0\x9f\x87\xaf\xf0\x9f\x87\xb5", "ascii text "};
    std::string out;
    for (int i = 0; i < 4000; ++i) {
        int n = 4 + r.below(8);
        for (int k = 0; k < n; ++k) out += pieces[r.below(10)];
        out += "\r\n";
    }
    return out;
}

// Full-screen TUI redraws: cursor addressing, erase, short styled spans.
std::string corpus_tui(Rng& r, int cols, int rows) {
    std::string out;
    for (int frame = 0; frame < 60; ++frame) {
        out += "\x1b[?2026h\x1b[H";
        for (int y = 1; y <= rows; ++y) {
            out += "\x1b[" + std::to_string(y) + ";1H\x1b[2K";
            int x = 1;
            while (x < cols - 20) {
                out += "\x1b[" + std::to_string(30 + r.below(8)) + ";" + std::to_string(40 + r.below(8)) + "m";
                int len = 3 + r.below(15);
                out += words(r, len);
                x += len;
            }
            out += "\x1b[m";
        }
        out += "\x1b[?2026l";
    }
    return out;
}

// Long unbroken lines that soft-wrap many times (minified JSON, base64).
std::string corpus_wrap(Rng& r) {
    std::string out;
    for (int i = 0; i < 200; ++i) {
        out += words(r, 2000 + r.below(4000));
        out += "\r\n";
    }
    return out;
}

// Parser alone (sink does nothing): the floor the emulator is measured against.
struct NullSink final : ParserSink {
    size_t n = 0;
    void print(char32_t) override { ++n; }
    void print_ascii(const char*, size_t k) override { n += k; }
    void execute(uint8_t) override { ++n; }
    void esc_dispatch(const EscSeq&) override { ++n; }
    void csi_dispatch(const CsiSeq&) override { ++n; }
    void osc_dispatch(std::string_view, bool) override { ++n; }
    void dcs_hook(const CsiSeq&) override {}
    void dcs_put(std::string_view) override {}
    void dcs_unhook(bool) override {}
    void string_dispatch(StringKind, std::string_view) override {}
};

constexpr size_t kChunk = 64 * 1024;

template <class Feed>
double timed(const std::string& corpus, size_t reps, Feed&& feed) {
    auto start = std::chrono::steady_clock::now();
    for (size_t rep = 0; rep < reps; ++rep)
        for (size_t off = 0; off < corpus.size(); off += kChunk) feed(std::string_view(corpus).substr(off, kChunk));
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return double(reps * corpus.size()) / (1024.0 * 1024.0) / secs;
}

const char* g_only = nullptr;  // run just this corpus (and skip the parser-alone pass)

void run(const char* name, const std::string& corpus, size_t target_bytes, int cols, int rows) {
    if (g_only && std::strcmp(g_only, name) != 0) return;
    TerminalOptions o;
    o.cols = cols;
    o.rows = rows;
    o.scrollback_rows = std::getenv("BENCH_SB") ? size_t(std::atoi(std::getenv("BENCH_SB"))) : 10000;
    Terminal t(o);
    size_t reps = std::max<size_t>(1, target_bytes / corpus.size());
    t.feed(corpus);  // warm up (style table, scrollback blocks)
    double rate = timed(corpus, reps, [&](std::string_view s) { t.feed(s); });
    CHECK(t.cursor().row >= 0 && t.cursor().row < rows);
    if (g_only) {
        std::printf("  %-8s %8.1f MB/s emulated\n", name, rate);
        return;
    }
    NullSink sink;
    Parser p(&sink);
    double prate = timed(corpus, reps, [&](std::string_view s) { p.feed(s); });
    std::printf("  %-8s %8.1f MB/s emulated   (parser alone %8.1f MB/s)\n", name, rate, prate);
    CHECK(sink.n > 0);
}

} // namespace

int main(int argc, char** argv) {
    init_test();
#if defined(NDEBUG)
    size_t mb = 32;
#else
    size_t mb = 4;
#endif
    if (argc > 1) mb = size_t(std::atoi(argv[1]));
    if (argc > 2) g_only = argv[2];
    size_t target = mb << 20;
    Rng r{0x9E3779B97F4A7C15ull};
    const int cols = 160, rows = 50;
    std::printf("[bench_throughput] %zu MB per corpus, %dx%d, 64 KiB feeds\n", mb, cols, rows);
    std::string ascii = corpus_ascii(r), sgr = corpus_sgr(r), uni = corpus_unicode(r);
    std::string tui = corpus_tui(r, cols, rows), wrap = corpus_wrap(r);
    run("ascii", ascii, target, cols, rows);
    run("sgr", sgr, target, cols, rows);
    run("unicode", uni, target, cols, rows);
    run("tui", tui, target, cols, rows);
    run("wrap", wrap, target, cols, rows);

    // The corpora must have produced what they say.
    {
        Terminal t(cols, rows, 100);
        t.feed(ascii);
        CHECK(t.history_rows() == 100);
        CHECK(!t.row_text(rows - 2).empty());
    }
    {
        Terminal t(cols, rows, 100);
        t.feed(uni);
        bool wide = false;
        for (int y = 0; y < rows && !wide; ++y)
            for (int x = 0; x < cols; ++x)
                if (t.row(y).cells[x].wide() == Wide::Lead) wide = true;
        CHECK(wide);
    }
    {
        Terminal t(cols, rows, 100);
        t.feed(tui);
        CHECK(!t.row_text(rows - 1).empty());
        CHECK(!t.modes().synchronized_output);
    }
    return check::finish("bench_throughput");
}
