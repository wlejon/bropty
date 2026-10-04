// Differential oracle: random VT streams (vterm_diff_gen.cpp) run through
// bropty and through libvterm; the screens (text, widths, attributes, colors),
// cursor, history and CPR replies must match. A failing stream is shrunk to a
// minimal reproducer before it is reported.
//
// BROPTY_DIFF_ITERS overrides the number of streams (default 10000),
// BROPTY_DIFF_SEED the first seed; BROPTY_DIFF_TRACE prints each stream.
// Remaining libvterm/xterm differences are excluded by the generator
// (vterm_diff_gen.cpp), normalized below, or patched in the vendored copy
// (third_party/libvterm/CMakeLists.txt lists the patches).
#include "bropty/terminal.h"
#include "bropty/unicode.h"
#include "check.h"
#include "vterm_diff.h"

extern "C" {
#include "vterm.h"
}

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

extern "C" int bropty_test_width(uint32_t cp) { return bropty::unicode::width(char32_t(cp)); }

namespace {

using namespace bropty;

constexpr int kAnyCell = -1;  // CellSnap::width of a libvterm cell excluded from comparison

struct CellSnap {
    std::u32string text;
    int width = 1;
    uint32_t fg = 0, bg = 0;
    int bold = 0, italic = 0, underline = 0, reverse = 0, strike = 0, blink = 0, conceal = 0;
    bool operator==(const CellSnap&) const = default;
};

struct Snap {
    int crow = 0, ccol = 0;
    std::vector<CellSnap> cells;
    std::vector<std::string> history;
    std::string replies;
};

uint32_t enc(Color c) {
    if (c.is_indexed()) return 0x1000000u | c.index();
    if (c.is_rgb()) return 0x2000000u | c.rgb_value().to_u32();
    return 0;
}

uint32_t enc(const VTermColor& c, bool fg) {
    if (fg ? VTERM_COLOR_IS_DEFAULT_FG(&c) : VTERM_COLOR_IS_DEFAULT_BG(&c)) return 0;
    if (VTERM_COLOR_IS_INDEXED(&c)) return 0x1000000u | c.indexed.idx;
    return 0x2000000u | (uint32_t(c.rgb.red) << 16) | (uint32_t(c.rgb.green) << 8) | c.rgb.blue;
}

std::string u8(const std::u32string& s) {
    std::string out;
    for (char32_t c : s) append_utf8(out, c);
    return out;
}

// ---- libvterm side ---------------------------------------------------------

enum class Orphan { None, Lead, Tail, Blank, Broken };

// A libvterm cell given whether the previous cell was a whole wide lead:
// Lead (a wide character with its continuation), Tail (that continuation),
// Blank (empty), Broken (half of a wide pair whose other half was
// overwritten), None (an ordinary narrow cell).
Orphan classify(const VTermScreenCell& c, bool prev_lead) {
    const uint32_t cp = c.chars[0];
    if (cp == uint32_t(-1)) return prev_lead ? Orphan::Tail : Orphan::Broken;
    if (cp == 0) return Orphan::Blank;
    const int w = bropty::unicode::width(char32_t(cp));
    if (w == 2) return c.width == 2 ? Orphan::Lead : Orphan::Broken;
    return Orphan::None;
}

struct Ref {
    VTerm* vt;
    VTermScreen* scr;
    std::vector<std::string> history;
    std::string out;
    int cols;

    static int pushline(int cols, const VTermScreenCell* cells, void* user) {
        auto* self = static_cast<Ref*>(user);
        std::string line;
        bool prev_lead = false;  // see snap()
        for (int x = 0; x < cols; ++x) {
            const Orphan o = classify(cells[x], prev_lead);
            prev_lead = o == Orphan::Lead;
            if (o == Orphan::Tail) continue;
            if (o == Orphan::Blank || o == Orphan::Broken) {
                line.push_back(' ');
                continue;
            }
            for (int k = 0; k < VTERM_MAX_CHARS_PER_CELL && cells[x].chars[k]; ++k) append_utf8(line, cells[x].chars[k]);
        }
        while (!line.empty() && line.back() == ' ') line.pop_back();
        self->history.push_back(line);
        return 1;
    }
    static void output(const char* s, size_t len, void* user) { static_cast<Ref*>(user)->out.append(s, len); }

    Ref(int c, int rows) : cols(c) {
        vt = vterm_new(rows, cols);
        vterm_set_utf8(vt, 1);
        vterm_output_set_callback(vt, &Ref::output, this);
        scr = vterm_obtain_screen(vt);
        static VTermScreenCallbacks cbs = {};
        cbs.sb_pushline = &Ref::pushline;
        vterm_screen_set_callbacks(scr, &cbs, this);
        vterm_screen_enable_altscreen(scr, 1);
        vterm_screen_reset(scr, 1);
    }
    ~Ref() { vterm_free(vt); }

    Snap snap(int rows) {
        Snap s;
        VTermPos pos;
        vterm_state_get_cursorpos(vterm_obtain_state(vt), &pos);
        s.crow = pos.row;
        s.ccol = pos.col;
        for (int y = 0; y < rows; ++y) {
            bool prev_lead = false;
            for (int x = 0; x < cols; ++x) {
                VTermScreenCell c;
                VTermPos p{y, x};
                vterm_screen_get_cell(scr, p, &c);
                CellSnap cs;
                // libvterm does not keep wide characters whole: overwriting or
                // erasing one half leaves the other (a continuation cell after
                // a narrow or blank cell, or a wide lead reported as width 1).
                // xterm and Ghostty (and bropty) blank the orphaned half, with
                // the current background; libvterm has no equivalent, so the
                // cell is not compared.
                const Orphan o = classify(c, prev_lead);
                prev_lead = o == Orphan::Lead;
                if (o == Orphan::Tail) {
                    cs.width = 0;  // right half of a wide cell
                    s.cells.push_back(cs);
                    continue;
                }
                if (o == Orphan::Broken) {
                    cs.width = kAnyCell;
                    s.cells.push_back(cs);
                    continue;
                }
                if (o == Orphan::Blank) {
                    cs.bg = enc(c.bg, false);
                    s.cells.push_back(cs);
                    continue;
                }
                for (int k = 0; k < VTERM_MAX_CHARS_PER_CELL && c.chars[k]; ++k) cs.text.push_back(char32_t(c.chars[k]));
                cs.width = o == Orphan::Lead ? 2 : 1;
                cs.bg = enc(c.bg, false);
                if (!cs.text.empty()) {
                    cs.fg = enc(c.fg, true);
                    cs.bold = c.attrs.bold;
                    cs.italic = c.attrs.italic;
                    cs.underline = c.attrs.underline;
                    cs.reverse = c.attrs.reverse;
                    cs.strike = c.attrs.strike;
                    cs.blink = c.attrs.blink;
                    cs.conceal = c.attrs.conceal;
                } else {
                    cs.width = 1;
                }
                s.cells.push_back(cs);
            }
        }
        s.history = history;
        s.replies = out;
        return s;
    }
};

// ---- bropty side -------------------------------------------------------------

struct Mine : TerminalHost {
    Terminal t;
    std::string out;
    void write_to_pty(std::string_view b) override { out.append(b); }
    Mine(int cols, int rows) : t(make(cols, rows)) { t.set_host(this); }
    static TerminalOptions make(int cols, int rows) {
        TerminalOptions o;
        o.cols = cols;
        o.rows = rows;
        o.scrollback_rows = 100000;
        o.grapheme_clustering = false;
        return o;
    }
    Snap snap() {
        Snap s;
        s.crow = t.cursor().row;
        s.ccol = t.cursor().col;
        for (int y = 0; y < t.rows(); ++y) {
            RowView v = t.row(y);
            for (int x = 0; x < v.cols; ++x) {
                CellSnap cs;
                const Cell& c = v[x];
                if (c.wide() == Wide::SpacerTail) {
                    cs.width = 0;
                    s.cells.push_back(cs);
                    continue;
                }
                const Style& st = v.style(x);
                cs.text = v.cluster(x);
                // libvterm keeps at most VTERM_MAX_CHARS_PER_CELL code points.
                if (cs.text.size() > VTERM_MAX_CHARS_PER_CELL) cs.text.resize(VTERM_MAX_CHARS_PER_CELL);
                cs.width = c.wide() == Wide::Lead ? 2 : 1;
                cs.bg = enc(st.bg);
                if (!cs.text.empty()) {
                    cs.fg = enc(st.fg);
                    cs.bold = st.has(Attr_Bold);
                    cs.italic = st.has(Attr_Italic);
                    cs.underline = int(st.underline);
                    cs.reverse = st.has(Attr_Inverse);
                    cs.strike = st.has(Attr_Strike);
                    cs.blink = st.has(Attr_Blink);
                    cs.conceal = st.has(Attr_Invisible);
                }
                s.cells.push_back(cs);
            }
        }
        for (size_t i = 0; i < t.history_rows(); ++i) s.history.push_back(t.history_text(i));
        s.replies = out;
        return s;
    }
};

std::string describe(const CellSnap& c) {
    std::string s = "\"" + u8(c.text) + "\" w" + std::to_string(c.width) + " fg" + std::to_string(c.fg) + " bg" +
                    std::to_string(c.bg) + " b" + std::to_string(c.bold) + "i" + std::to_string(c.italic) + "u" +
                    std::to_string(c.underline) + "r" + std::to_string(c.reverse) + "s" + std::to_string(c.strike) +
                    "k" + std::to_string(c.blink) + "c" + std::to_string(c.conceal);
    return s;
}

// Returns an empty string when both emulators agree.
// A stream that leaves libvterm's defined behaviour: DCH starting on the right
// half of a wide character. bropty (like Ghostty) erases the whole character;
// libvterm deletes only that half and then reports a bogus pair made of the
// remaining lead and whatever continuation cell shifts in after it, which
// its cell API cannot tell from a real one.
const std::string kSkip = "\x01skip";

bool on_continuation(Ref& ref) {
    VTermPos pos;
    vterm_state_get_cursorpos(vterm_obtain_state(ref.vt), &pos);
    VTermScreenCell c;
    vterm_screen_get_cell(ref.scr, pos, &c);
    return c.chars[0] == uint32_t(-1);
}

bool is_dch(const std::string& t) {
    if (t.size() < 3 || t[0] != '\x1b' || t[1] != '[' || t.back() != 'P') return false;
    for (size_t i = 2; i + 1 < t.size(); ++i)
        if (t[i] < '0' || t[i] > '9') return false;
    return true;
}

// Empty when both emulators agree, kSkip when the stream is outside
// libvterm's defined behaviour, else a description of the first difference.
std::string compare(const std::vector<std::string>& toks, int cols, int rows, uint64_t split_seed) {
    Ref ref(cols, rows);
    Mine mine(cols, rows);
    std::string all;
    for (const auto& t : toks) {
        if (is_dch(t) && on_continuation(ref)) return kSkip;
        vterm_input_write(ref.vt, t.data(), t.size());
        all += t;
    }
    if (std::getenv("BROPTY_DIFF_TRACE")) std::fprintf(stderr, "  stream %s\n", check::show(all).c_str());
    // Feed bropty in random chunks: results must not depend on chunking.
    uint64_t s = split_seed;
    size_t pos = 0;
    while (pos < all.size()) {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        size_t len = 1 + size_t((s >> 33) % 17);
        len = std::min(len, all.size() - pos);
        mine.t.feed(std::string_view(all).substr(pos, len));
        pos += len;
    }
    Snap a = mine.snap();
    Snap b = ref.snap(rows);
    for (size_t i = 0; i < a.cells.size() && i < b.cells.size(); ++i) {
        if (b.cells[i].width == kAnyCell) continue;
        if (!(a.cells[i] == b.cells[i])) {
            return "cell (" + std::to_string(i / size_t(cols)) + "," + std::to_string(i % size_t(cols)) +
                   ")\n       bropty   " + describe(a.cells[i]) + "\n       libvterm " + describe(b.cells[i]);
        }
    }
    if (a.crow != b.crow || a.ccol != b.ccol) {
        return "cursor bropty (" + std::to_string(a.crow) + "," + std::to_string(a.ccol) + ") libvterm (" +
               std::to_string(b.crow) + "," + std::to_string(b.ccol) + ")";
    }
    if (a.history != b.history) {
        std::string d = "history: bropty " + std::to_string(a.history.size()) + " rows, libvterm " +
                        std::to_string(b.history.size());
        for (size_t i = 0; i < a.history.size() && i < b.history.size(); ++i) {
            if (a.history[i] != b.history[i]) {
                d += "; first diff at " + std::to_string(i) + ": " + check::show(a.history[i]) + " vs " + check::show(b.history[i]);
                break;
            }
        }
        return d;
    }
    // Only CPR replies are comparable (DA contents legitimately differ).
    auto cprs = [](const std::string& r) {
        std::string out;
        size_t p = 0;
        while ((p = r.find("\x1b[", p)) != std::string::npos) {
            size_t e = r.find_first_of("Rc", p + 2);
            if (e == std::string::npos) break;
            if (r[e] == 'R') out += r.substr(p, e - p + 1);
            p = e + 1;
        }
        return out;
    };
    if (cprs(a.replies) != cprs(b.replies)) {
        return "replies bropty " + check::show(cprs(a.replies)) + " libvterm " + check::show(cprs(b.replies));
    }
    return {};
}

// Shrink a failing token list while it keeps failing.
std::vector<std::string> minimize(std::vector<std::string> toks, int cols, int rows) {
    size_t chunk = toks.size() / 2;
    while (chunk >= 1) {
        bool removed = false;
        for (size_t i = 0; i + chunk <= toks.size();) {
            std::vector<std::string> trial(toks.begin(), toks.begin() + long(i));
            trial.insert(trial.end(), toks.begin() + long(i + chunk), toks.end());
            std::string d;
            if (!trial.empty() && vdiff::valid(trial)) d = compare(trial, cols, rows, 1);
            if (!d.empty() && d != kSkip) {
                toks = std::move(trial);
                removed = true;
            } else {
                i += chunk;
            }
        }
        if (!removed) chunk /= 2;
    }
    return toks;
}

} // namespace

// BROPTY_DIFF_STREAM="cols,rows,<stream>" (with \e and \xNN escapes) compares
// one literal stream and prints the result: a triage aid, not a test.
int run_stream(const char* spec) {
    int cols = 0, rows = 0, used = 0;
    if (std::sscanf(spec, "%d,%d,%n", &cols, &rows, &used) < 2 || cols < 2 || rows < 1) return 2;
    std::string s;
    for (const char* p = spec + used; *p; ++p) {
        if (p[0] == '\\' && p[1] == 'e') { s.push_back('\x1b'); ++p; }
        else if (p[0] == '\\' && p[1] == 'x' && p[2] && p[3]) {
            s.push_back(char(std::strtol(std::string(p + 2, 2).c_str(), nullptr, 16)));
            p += 3;
        } else s.push_back(*p);
    }
    std::string d = compare({s}, cols, rows, 1);
    std::printf("%s\n", d.empty() ? "agree" : d.c_str());
    return 0;
}

int main() {
    init_test();
    if (const char* spec = std::getenv("BROPTY_DIFF_STREAM")) return run_stream(spec);
    int iters = 10000;
    if (const char* e = std::getenv("BROPTY_DIFF_ITERS")) iters = std::max(1, std::atoi(e));
    int first = 1;
    if (const char* e = std::getenv("BROPTY_DIFF_SEED")) first = std::max(1, std::atoi(e));
    const bool trace = std::getenv("BROPTY_DIFF_TRACE") != nullptr;
    int reported = 0;
    int skipped = 0;
    int ran = 0;
    for (int seed = first; seed < first + iters && reported < 5; ++seed) {
        vdiff::Rng r{uint64_t(seed) * 31337};
        int cols = 5 + r.below(26);
        int rows = 2 + r.below(9);
        std::vector<std::string> toks = vdiff::generate(uint64_t(seed), cols, rows);
        if (trace) {
            std::string stream;
            for (const auto& t : toks) stream += t;
            std::fprintf(stderr, "seed %d %dx%d %s\n", seed, cols, rows, check::show(stream).c_str());
        }
        ++check::g_checks;
        ++ran;
        std::string diff = compare(toks, cols, rows, uint64_t(seed));
        if (diff.empty()) continue;
        if (diff == kSkip) {
            ++skipped;
            continue;
        }
        toks = minimize(toks, cols, rows);
        std::string stream;
        for (const auto& t : toks) stream += t;
        check::fail(__FILE__, __LINE__,
                    "seed " + std::to_string(seed) + " (" + std::to_string(cols) + "x" + std::to_string(rows) +
                        ")\n     stream " + check::show(stream) + "\n     " + compare(toks, cols, rows, 1));
        ++reported;
    }
    std::printf("  %d streams compared, %d outside libvterm's defined behaviour\n", ran - skipped, skipped);
    // The oracle must not quietly decay into skipping.
    CHECK(skipped * 50 <= ran);
    return check::finish("test_vterm_diff");
}
