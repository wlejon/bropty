// Random VT stream generator for the libvterm differential test.
//
// The alphabet is everything both emulators implement the same way per
// xterm. Deliberate exclusions (each covered by hand-written tests instead):
//  - DECLRMM / DECSLRM: libvterm wraps printing at the screen edge, not the
//    right margin.
//  - IL / DL are followed by CR: xterm and bropty home the column to the left
//    margin, libvterm leaves it.
//  - DL is preceded by a CUP to row >= 2: libvterm pushes rows deleted at the
//    top of the screen into scrollback, xterm does not.
//  - DEC graphics '_', 'y', 'z': libvterm maps y/z to slanted variants and
//    leaves '_' unmapped.
//  - Combining marks only directly after a printed character, REP only after
//    one: libvterm drops/handles detached marks differently (both are
//    implementation-defined).
//  - No REP: libvterm clamps it at the row end instead of wrapping, advances
//    one column per repeated wide character and does not attach a later
//    combining mark (xterm does all three like printing).
//  - SU / SD / IL / DL counts below the height, and SU / SD / IL by one at most
//    while a scroll region is set: libvterm takes an erase path when the
//    count covers the region, with different scrollback pushes.
//  - No CPR under DECOM: xterm reports the position relative to the origin,
//    libvterm the absolute one.
//  - No CUU / CUD / CNL / CPL / VPR / ICH / DCH while a scroll region is
//    set: libvterm ignores the margins for relative vertical motion (xterm
//    stops at them) and ignores ICH / DCH outside the region (xterm and
//    Ghostty only consult the left/right margins).
//  - No non-ASCII text in insert mode (IRM): libvterm shifts the line by one
//    column for every glyph, wide ones included.
//  - DECRC under DECOM only while the margins are unchanged since DECSC:
//    xterm re-applies the new top margin, libvterm restores the saved row.
//  - 1049h only on the primary screen, 1049l only on the alternate, no
//    DECSC / DECRC on the alternate: libvterm clears again on a repeated
//    1049h and shares one save slot between the screens (xterm has one each).
//  - DECAWM reset is preceded by CR, and no non-ASCII text while it is off:
//    xterm overwrites the last column from a pending wrap, and bropty backs a
//    non-fitting wide character up (kitty, WezTerm); libvterm drops both.
//  - DECSTBM only with top < bottom: xterm ignores an invalid region
//    entirely; libvterm still homes the cursor.
//  - ICH / DCH counts below the width: libvterm pushes a row into scrollback
//    when ICH covers the whole row.
//  - DECALN carries explicit DECOM reset, DECSTBM reset, SGR 0 and CUP:
//    xterm (and bropty) reset margins, fill with default attributes and
//    home the cursor, and bropty also clears DECOM (as Ghostty does);
//    libvterm does none of these.
//  - No non-ASCII text while DEC graphics is in GL: libvterm decodes ASCII
//    that directly follows a UTF-8 character as part of the UTF-8 run,
//    bypassing the G-set translation.
//  - No DECRC / 1049l before the first save: libvterm restores a zeroed pen
//    (RGB black on black) instead of the defaults.
//  - Grapheme clustering (mode 2027) is off for the comparison.
#include "vterm_diff.h"

namespace vdiff {

namespace {

void utf8(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out.push_back(char(cp));
    } else if (cp < 0x800) {
        out.push_back(char(0xC0 | (cp >> 6)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(char(0xE0 | (cp >> 12)));
        out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(char(0xF0 | (cp >> 18)));
        out.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    }
}

std::string param(Rng& r, int max) {
    int k = r.below(10);
    if (k == 0) return "";
    if (k == 1) return "0";
    return std::to_string(1 + r.below(max));
}

std::string csi(const std::string& body) { return "\x1b[" + body; }

// DECALN with the state xterm resets made explicit (margins, DECOM, SGR,
// cursor home) so libvterm, which resets none of it, agrees.
constexpr const char* kDecaln = "\x1b[?6l\x1b[r\x1b[0m\x1b#8\x1b[H";

// Tracks the charset / origin / save state that the exclusions depend on.
// libvterm's DECSC does not save charsets or DECOM (xterm's does), so a
// restore is only acceptable when it would not change them.
class Checker {
public:
    bool gfx() const { return cs_.gl == 0 ? cs_.g0 : cs_.g1; }

    // Accepts `t` (updating the state) or rejects it (state unchanged).
    bool accept(const std::string& t) {
        if ((gfx() || !autowrap_ || insert_) && has_non_ascii(t)) return false;
        if (t == "\x1b[4h") insert_ = true;
        else if (t == "\x1b[4l") insert_ = false;
        if (t == "\x1b(0") cs_.g0 = true;
        else if (t == "\x1b(B") cs_.g0 = false;
        else if (t == "\x1b)0\x0e") { cs_.g1 = true; cs_.gl = 1; }
        else if (t == "\x0f\x1b(B") { cs_.gl = 0; cs_.g0 = false; }
        else if (t == "\x1b[?6h") cs_.origin = true;
        else if (t == "\x1b[?6l" || t == kDecaln) cs_.origin = false;
        else if (t == "\x1b[r" || t == kDecaln) { region_ = false; ++cs_.region_gen; }
        else if (is_csi(t, "r")) { region_ = true; ++cs_.region_gen; }
        else if (region_ && is_csi(t, "ABEFe@P")) return false;
        else if (region_ && is_csi(t, "ST") && first_param(t) > 1) return false;
        else if (region_ && t.size() > 1 && t.back() == '\r' && is_csi(t.substr(0, t.size() - 1), "L") &&
                 first_param(t) > 1)
            return false;
        else if (cs_.origin && t == "\x1b[6n") return false;
        else if (t == "\x1b[?7h") autowrap_ = true;
        else if (t == "\r\x1b[?7l") autowrap_ = false;
        else if (t == "\x1b" "7") {
            if (alt_) return false;
            saved_ = cs_;
            have_save_ = true;
        } else if (t == "\x1b" "8") {
            return !alt_ && have_save_ && same(cs_, saved_);
        } else if (t == "\x1b[?1049h") {
            if (alt_) return false;
            saved_ = cs_;
            have_save_ = true;
            alt_ = true;
        } else if (t == "\x1b[?1049l") {
            if (!alt_ || !same(cs_, saved_)) return false;
            alt_ = false;
        }
        return true;
    }

private:
    struct Cs {
        bool g0 = false, g1 = false;
        int gl = 0;
        bool origin = false;
        int region_gen = 0;  // bumped by every DECSTBM
    };
    // Restoring `saved` from `cur` changes nothing libvterm handles differently.
    static bool same(const Cs& cur, const Cs& saved) {
        return cur.g0 == saved.g0 && cur.g1 == saved.g1 && cur.gl == saved.gl && cur.origin == saved.origin &&
               (!cur.origin || cur.region_gen == saved.region_gen);
    }
    // A single CSI with numeric parameters and one of `finals`.
    static bool is_csi(const std::string& t, const char* finals) {
        if (t.size() < 3 || t[0] != '\x1b' || t[1] != '[') return false;
        for (size_t i = 2; i + 1 < t.size(); ++i)
            if (!((t[i] >= '0' && t[i] <= '9') || t[i] == ';')) return false;
        for (const char* f = finals; *f; ++f)
            if (t.back() == *f) return true;
        return false;
    }
    static int first_param(const std::string& t) {
        int v = 0;
        for (size_t i = 2; i < t.size() && t[i] >= '0' && t[i] <= '9'; ++i) v = v * 10 + (t[i] - '0');
        return v;
    }
    static bool has_non_ascii(const std::string& t) {
        for (char c : t)
            if (uint8_t(c) >= 0x80) return true;
        return false;
    }
    Cs cs_, saved_;
    bool have_save_ = false;
    bool alt_ = false;
    bool autowrap_ = true;
    bool region_ = false;
    bool insert_ = false;
};

} // namespace

std::vector<std::string> generate(uint64_t seed, int cols, int rows) {
    Rng r{seed * 0x9E3779B97F4A7C15ull + 1};
    std::vector<std::string> toks;
    int n = 1 + r.below(120);
    Checker chk;
    bool after_print = false;
    int marks = 0;  // upper bound on combining marks glued since the last print
    const int maxc = cols + 3;
    const int maxr = rows + 3;
    for (int i = 0; i < n; ++i) {
        std::string t;
        bool printed = false;
        const bool gfx = chk.gfx();
        int kind = r.below(100);
        if (kind < 30) {  // text
            int len = 1 + r.below(10);
            static const char safe_gfx[] = "jklmnqtuvwx";
            for (int k = 0; k < len; ++k) {
                if (gfx) t.push_back(safe_gfx[r.below(11)]);
                else t.push_back(char(r.chance(15) ? ' ' : 0x21 + r.below(0x5E)));
            }
            printed = true;
        } else if (kind < 36) {  // wide characters
            int len = 1 + r.below(4);
            for (int k = 0; k < len; ++k) utf8(t, r.chance(70) ? char32_t(0x4E00 + r.below(500)) : char32_t(0x1F600 + r.below(60)));
            printed = true;
        } else if (kind < 39) {  // other scripts, narrow non-ASCII
            static const char32_t cps[] = {0xE9, 0x3B1, 0x416, 0x5D0, 0xE01, 0x2500, 0xFF61};
            utf8(t, cps[r.below(7)]);
            printed = true;
        } else if (kind < 42 && after_print) {  // combining marks
            static const char32_t marks[] = {0x301, 0x308, 0x20D7, 0x5B0, 0xE31};
            int m = 1 + r.below(2);
            for (int k = 0; k < m; ++k) utf8(t, marks[r.below(5)]);
            printed = true;
        } else if (kind < 52) {  // C0
            static const char* c0[] = {"\r", "\n", "\r\n", "\b", "\t", "\x0b", "\x0c"};
            t = c0[r.below(7)];
        } else if (kind < 64) {  // cursor movement
            static const char fin[] = "ABCDEFGHdfa`e";
            char f = fin[r.below(13)];
            if (f == 'H' || f == 'f') t = csi(param(r, maxr) + (r.chance(80) ? ";" + param(r, maxc) : "") + f);
            else t = csi(param(r, f == 'd' || f == 'A' || f == 'B' || f == 'e' ? maxr : maxc) + f);
        } else if (kind < 74) {  // editing
            switch (r.below(9)) {
            case 0: t = csi(std::to_string(r.below(3)) + "J"); break;
            case 1: t = csi(std::to_string(r.below(3)) + "K"); break;
            case 2: t = csi(param(r, maxc) + "X"); break;
            case 3: t = csi(param(r, cols - 1) + "@"); break;
            case 4: t = csi(param(r, cols - 1) + "P"); break;
            case 5: t = csi(param(r, rows - 1) + "L") + "\r"; break;
            case 6:
                t = csi(std::to_string(2 + r.below(rows)) + "H") + csi(param(r, rows - 1) + "M") + "\r";
                break;
            case 7: t = csi(param(r, rows - 1) + "S"); break;
            default: t = csi(param(r, rows - 1) + "T"); break;
            }
        } else if (kind < 77) {  // scroll region
            if (r.chance(30)) {
                t = csi("r");
            } else {
                int a = 1 + r.below(rows - 1);
                int b = a + 1 + r.below(rows - a);
                t = csi(std::to_string(a) + ";" + std::to_string(b) + "r");
            }
        } else if (kind < 82) {  // index family and tabs
            static const char* seqs[] = {"\x1b" "D", "\x1b" "E", "\x1bM", "\x1bH", "\x1b[g", "\x1b[3g"};
            int k = r.below(8);
            if (k < 6) t = seqs[k];
            else t = csi(param(r, 4) + (k == 6 ? "I" : "Z"));
        } else if (kind < 90) {  // SGR
            static const char* sgr[] = {"0", "1", "3", "4", "7", "9", "22", "23", "24", "27", "29", "39", "49",
                                        "4:2", "4:3", "5", "8", "25", "28"};
            std::string body;
            int m = 1 + r.below(3);
            for (int k = 0; k < m; ++k) {
                if (k) body += ";";
                int c = r.below(6);
                if (c == 0) body += std::to_string(30 + r.below(8));
                else if (c == 1) body += std::to_string(40 + r.below(8));
                else if (c == 2) body += std::to_string((r.chance(50) ? 90 : 100) + r.below(8));
                else if (c == 3) body += (r.chance(50) ? "38;5;" : "48;5;") + std::to_string(r.below(256));
                else if (c == 4) body += "38;2;" + std::to_string(r.below(256)) + ";" + std::to_string(r.below(256)) + ";" + std::to_string(r.below(256));
                else body += sgr[r.below(19)];
            }
            t = csi(body + "m");
        } else if (kind < 93) {  // save / restore
            t = r.chance(50) ? "\x1b" "7" : "\x1b" "8";
        } else if (kind < 96) {  // modes
            static const char* modes[] = {"?7h", "\r\x1b[?7l", "4h", "4l", "?6h", "?6l", "?25l", "?25h"};
            int m = r.below(8);
            t = m == 1 ? std::string(modes[1]) : csi(modes[m]);
        } else if (kind < 98) {  // charsets
            static const char* sets[] = {"\x1b(0", "\x1b(B", "\x1b)0\x0e", "\x0f\x1b(B"};
            t = sets[r.below(4)];
        } else if (kind < 99) {
            t = csi(r.chance(50) ? "?1049h" : "?1049l");
        } else {
            t = r.chance(50) ? std::string(kDecaln) : csi("6n");
        }
        if (kind >= 39 && kind < 42) {
            // Combining marks: glued to the print they follow, so a minimizer
            // can never detach them.
            // At most 4 marks per cluster: libvterm keeps 6 code points a cell.
            if (!after_print || toks.empty() || marks + 2 > 4 || !chk.accept(toks.back() + t)) continue;
            toks.back() += t;
            marks += 2;
            continue;
        }
        if (!chk.accept(t)) continue;
        toks.push_back(t);
        after_print = printed;
        marks = 0;
    }
    return toks;
}

bool valid(const std::vector<std::string>& toks) {
    Checker chk;
    for (const std::string& t : toks)
        if (!chk.accept(t)) return false;
    return true;
}

} // namespace vdiff
