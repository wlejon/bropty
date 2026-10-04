// Parser: Williams state-machine edge cases and strict UTF-8, checked as the
// full event log a byte stream produces.
#include "bropty/parser.h"
#include "check.h"

#include <cstdio>
#include <string>

using namespace bropty;

namespace {

struct Recorder : ParserSink {
    std::string log;
    void print(char32_t cp) override {
        if (cp < 0x80) {
            log += "P(";
            log.push_back(char(cp));
            log += ")";
        } else {
            char b[16];
            std::snprintf(b, sizeof b, "P(U+%04X)", unsigned(cp));
            log += b;
        }
    }
    void print_ascii(const char* s, size_t n) override {
        for (size_t i = 0; i < n; ++i) print(char32_t(uint8_t(s[i])));
    }
    void execute(uint8_t b) override {
        char buf[16];
        std::snprintf(buf, sizeof buf, "X(%02x)", b);
        log += buf;
    }
    static std::string params(const CsiSeq& s) {
        std::string out;
        if (s.prefix) out.push_back(s.prefix);
        for (int i = 0; i < s.count; ++i) {
            if (i) out += (s.sub >> i & 1u) ? ":" : ";";
            if (s.present >> i & 1u) out += std::to_string(s.params[i]);
        }
        out += "|";
        for (int i = 0; i < s.ninter; ++i) out.push_back(s.inter[i]);
        out += "|";
        out.push_back(s.final);
        return out;
    }
    void csi_dispatch(const CsiSeq& s) override { log += "CSI(" + params(s) + ")"; }
    void esc_dispatch(const EscSeq& e) override {
        log += "ESC(";
        for (int i = 0; i < e.ninter; ++i) log.push_back(e.inter[i]);
        log.push_back(e.final);
        log += ")";
    }
    void osc_dispatch(std::string_view payload, bool bel) override {
        log += "OSC(" + std::string(payload) + (bel ? ",BEL)" : ",ST)");
    }
    void dcs_hook(const CsiSeq& s) override { log += "DCS(" + params(s) + ")["; }
    void dcs_put(std::string_view d) override { log += std::string(d); }
    void dcs_unhook(bool aborted) override { log += aborted ? "]ABORT" : "]"; }
    void string_dispatch(StringKind k, std::string_view p) override {
        log += k == StringKind::Apc ? "APC(" : k == StringKind::Pm ? "PM(" : "SOS(";
        log += std::string(p) + ")";
    }
};

std::string run(std::string_view bytes) {
    Recorder r;
    Parser p(&r);
    p.feed(bytes);
    return r.log;
}

// Same stream, one byte per feed() call: results must not depend on chunking.
std::string run_bytewise(std::string_view bytes) {
    Recorder r;
    Parser p(&r);
    for (char c : bytes) p.feed(std::string_view(&c, 1));
    return r.log;
}

void expect(std::string_view in, const std::string& want, int line) {
    std::string got = run(in);
    std::string got2 = run_bytewise(in);
    ++check::g_checks;
    if (got != want) check::fail(__FILE__, line, "input " + check::show(in) + "\n     got  " + check::show(got) + "\n     want " + check::show(want));
    ++check::g_checks;
    if (got2 != want) check::fail(__FILE__, line, "bytewise input " + check::show(in) + "\n     got  " + check::show(got2));
}
#define EXPECT(in, want) expect(in, want, __LINE__)

} // namespace

int main() {
    init_test();

    // C0 / CAN / SUB / ESC restarts.
    EXPECT("\x1b[31\x18m", "X(18)P(m)");
    EXPECT("\x1b[31\x1am", "X(1a)P(m)");
    EXPECT("\x1b]0;abc\x18X", "X(18)P(X)");
    EXPECT("\x1bP1$r\x18Z", "DCS(1|$|r)[]ABORTX(18)P(Z)");
    EXPECT("\x1b[31\x1b[32m", "CSI(32||m)");
    EXPECT("\x1b[1\n;2H", "X(0a)CSI(1;2||H)");
    EXPECT("\x1b[1\x7f;2H", "CSI(1;2||H)");
    EXPECT("\x1b[1?h", "");
    EXPECT("\x1b[1?hA", "P(A)");
    EXPECT("\x1b[1 2h", "");  // parameter after intermediate -> ignore

    // Parameters.
    EXPECT("\x1b[;5H", "CSI(;5||H)");
    EXPECT("\x1b[1;H", "CSI(1;||H)");
    EXPECT("\x1b[m", "CSI(||m)");
    EXPECT("\x1b[0m", "CSI(0||m)");
    EXPECT("\x1b[4:3m", "CSI(4:3||m)");
    EXPECT("\x1b[38:2::10:20:30m", "CSI(38:2::10:20:30||m)");
    EXPECT("\x1b[?25h", "CSI(?25||h)");
    EXPECT("\x1b[>4;1m", "CSI(>4;1||m)");
    EXPECT("\x1b[2 q", "CSI(2| |q)");
    EXPECT("\x1b[?2026$p", "CSI(?2026|$|p)");
    EXPECT("\x1b[99999999999999999999;2H", "CSI(65535;2||H)");
    {
        std::string many = "\x1b[";
        for (int i = 0; i < 5000; ++i) many += "1;";
        many += "7m";
        std::string want = "CSI(";
        for (int i = 0; i < CsiSeq::kMaxParams; ++i) want += i ? ";1" : "1";
        want += "||m)";
        EXPECT(many, want);
    }

    // ESC sequences.
    EXPECT("\x1b(0", "ESC((0)");
    EXPECT("\x1b#8", "ESC(#8)");
    EXPECT("\x1b" "7\x1b" "8", "ESC(7)ESC(8)");
    EXPECT("\x1b\x07" "D", "X(07)ESC(D)");  // C0 inside ESC executes, ESC continues

    // OSC.
    EXPECT("\x1b]0;abc\x07", "OSC(0;abc,BEL)");
    EXPECT("\x1b]2;xyz\x1b\\", "OSC(2;xyz,ST)");
    EXPECT("\x1b]8;id=1;http://a/b;c\x07", "OSC(8;id=1;http://a/b;c,BEL)");
    EXPECT("\x1b]0;stale\x1b[1mX\x1b\\", "CSI(1||m)P(X)ESC(\\)");
    EXPECT("\x1b]0;\xe2\x80\x9chi\xe2\x80\x9d\x07", "OSC(0;\xe2\x80\x9chi\xe2\x80\x9d,BEL)");
    EXPECT("\x1b]0;a\x01" "b\x07", "OSC(0;ab,BEL)");  // C0 ignored inside OSC

    // DCS / SOS / PM / APC.
    EXPECT("\x1bP1$r0m\x1b\\Z", "DCS(1|$|r)[0m]P(Z)");
    EXPECT("\x1bPq#0;2;0\x07" "abc\x1b\\Z", "DCS(||q)[#0;2;0\x07" "abc]P(Z)");
    EXPECT("\x1bP+q544e\x1b\\", "DCS(|+|q)[544e]");
    EXPECT("\x1bPx\x1b[1m", "DCS(||x)[]ABORTCSI(1||m)");
    EXPECT("\x1b_Gf=24;AAAA\x1b\\Z", "APC(Gf=24;AAAA)P(Z)");
    EXPECT("\x1b^privacy\x1b\\Z", "PM(privacy)P(Z)");
    EXPECT("\x1bXsos\x07more\x1b\\Z", "SOS(sosmore)P(Z)");

    // UTF-8: valid, split, and every class of ill-formed input.
    EXPECT("\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80", "P(U+00E9)P(U+20AC)P(U+1F600)");
    EXPECT("\xc3\x9b", "P(U+00DB)");
    EXPECT("\xe2\x80\x9c", "P(U+201C)");
    EXPECT("\xc2\x90X", "P(U+0090)P(X)");
    EXPECT("\xc3(", "P(U+FFFD)P(()");
    EXPECT("\xff" "A", "P(U+FFFD)P(A)");
    EXPECT("\x9b" "31mA", "P(U+FFFD)P(3)P(1)P(m)P(A)");  // raw C1 is not CSI
    EXPECT("\xc0\xaf" "A", "P(U+FFFD)P(U+FFFD)P(A)");    // overlong
    EXPECT("\xe0\x80\xaf", "P(U+FFFD)P(U+FFFD)P(U+FFFD)");
    EXPECT("\xed\xa0\x80" "A", "P(U+FFFD)P(U+FFFD)P(U+FFFD)P(A)");  // surrogate
    EXPECT("\xf4\x90\x80\x80" "A", "P(U+FFFD)P(U+FFFD)P(U+FFFD)P(U+FFFD)P(A)");  // > U+10FFFF
    EXPECT("\xf0\x9f\x98", "");  // incomplete at end of input: held, not flushed
    EXPECT("\xf0\x9f\x98" "A", "P(U+FFFD)P(A)");  // one FFFD for the maximal subpart
    EXPECT("\xe2\x82\x1b[mA", "P(U+FFFD)CSI(||m)P(A)");
    EXPECT("\xe2\x82\nA", "P(U+FFFD)X(0a)P(A)");
    EXPECT("\xe2\x82\x7f" "A", "P(U+FFFD)P(A)");
    EXPECT("\xf4\x8f\xbf\xbf", "P(U+10FFFF)");
    EXPECT("\xef\xbb\xbf", "P(U+FEFF)");

    // String payload cap: oversized OSC discarded whole, parser recovers.
    {
        Recorder r;
        Parser p(&r);
        p.set_max_string_bytes(16);
        p.feed("\x1b]0;" + std::string(100, 'x') + "\x07Z\x1b]0;ok\x07");
        CHECK_EQ(r.log, std::string("P(Z)OSC(0;ok,BEL)"));
    }

    return check::finish("test_parser");
}
