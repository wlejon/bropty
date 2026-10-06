// What a native console program receives of the terminal's replies through
// ConPTY. A console program talks to conhost, not to the terminal, and which
// queries conhost forwards and which replies it passes on differs between
// Windows builds, so this test asserts only what is bropty's and reports the
// rest:
//  * bropty's: the bytes written to the console's input pipe are delivered in
//    order, untouched (the markers between the replies all arrive, and each
//    reply arrives either intact or not at all: conhost passes or drops whole
//    sequences); a query that reaches the terminal is answered, with the
//    right answer; nothing is answered that was not asked; and where this
//    build's conhost passes a kind of reply through, a forwarded query's
//    answer reaches the program.
//  * conhost's, reported per build (see "Replies under ConPTY" in pty.h for
//    what was observed where): which queries reach the terminal, which reply
//    kinds reach the program, and who answers DA1 / DECRQSS.
// One delivery guarantee is asserted although it is conhost's, because the
// docs and bro rely on it and every build observed holds it: with
// ENABLE_VIRTUAL_TERMINAL_INPUT, CSI replies arrive intact, and a CSI query
// conhost does not know (kitty's CSI ? u) is forwarded and answered end to end.
//   test_pty_conpty_replies <path-to-pty_child>
#include "pty_harness.h"
#include "test_common.h"

#include <map>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

using namespace bropty;
using namespace ph;

namespace {

struct Clip : TerminalHost {
    std::optional<std::string> clipboard_read(std::string_view) override { return std::string("hello"); }
};

std::string got_of(const std::string& screen) {
    const size_t a = screen.find("GOT ");
    const size_t b = screen.find(" END", a == std::string::npos ? 0 : a);
    if (a == std::string::npos || b == std::string::npos) return "<none>";
    return screen.substr(a + 4, b - a - 4);
}

// The form pty_child prints input in: ESC as <1b>, other controls as <hh>.
std::string printable(const std::string& bytes) {
    static const char* digits = "0123456789abcdef";
    std::string o;
    for (unsigned char u : bytes) {
        if (u >= 0x20 && u < 0x7f && u != '<' && u != '{') {
            o.push_back(char(u));
            continue;
        }
        o += '<';
        o += digits[u >> 4];
        o += digits[u & 15];
        o += '>';
    }
    return o;
}

std::string windows_build() {
    using Fn = LONG(WINAPI*)(OSVERSIONINFOW*);
    auto fn = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    OSVERSIONINFOW v{};
    v.dwOSVersionInfoSize = sizeof v;
    if (!fn || fn(&v) != 0) return "unknown";
    return std::to_string(v.dwMajorVersion) + "." + std::to_string(v.dwMinorVersion) + "." + std::to_string(v.dwBuildNumber);
}

// Run `pty_child replies <reader> <query>`, write `inject` and the sentinel
// once it is reading, and return what it received (GOT ...). `fed` is what
// the terminal got from the console, `replied` what it answered.
struct Probe {
    std::string got, fed, replied;
};

Probe probe(const std::string& reader, const std::string& query, const std::string& inject = "") {
    Probe p;
    Run r(child({"replies", reader, query}), 250, 30);
    Clip clip;
    r.session.set_delegate(&clip);
    r.session.set_output_callback([&](std::string_view b) { p.replied.append(b); });
    r.session.set_feed_tap([&](std::string_view b) { p.fed.append(b); });
    // The query precedes READY, so its reply is queued before the sentinel,
    // and conhost reads the input pipe in order.
    if (!r.ok || !r.wait_for_text("READY")) {
        p.got = "<no READY>";
        return p;
    }
    CHECK(r.pty->write(inject + "q") == inject.size() + 1);
    p.got = r.wait_for_text(" END") ? got_of(r.screen()) : "<no END: " + r.screen() + ">";
    return p;
}

bool has(const std::string& s, const std::string& needle) { return s.find(needle) != std::string::npos; }

// The replies written between markers a..h, as bropty writes them.
struct Reply {
    const char* kind;  // "csi", "osc", "dcs"
    const char* name;
    std::string bytes;
};

const std::vector<Reply>& replies() {
    static const std::vector<Reply> r = {
        {"csi", "DA1", "\x1b[?62;22;52c"},
        {"csi", "CPR", "\x1b[5;10R"},
        {"osc", "OSC 11 (BEL)", "\x1b]11;rgb:0000/0000/0000\x07"},
        {"osc", "OSC 11 (ST)", "\x1b]11;rgb:1111/2222/3333\x1b\\"},
        {"osc", "OSC 52", "\x1b]52;c;aGVsbG8=\x1b\\"},
        {"osc", "OSC 4", "\x1b]4;1;rgb:cdcd/0000/0000\x1b\\"},
        {"dcs", "DECRQSS", "\x1bP1$r0m\x1b\\"},
    };
    return r;
}

// For each reply, what the program received in its place: "intact",
// "dropped", or what arrived instead. Empty if the markers are not all there
// in order (the delivery itself is broken).
std::vector<std::string> outcomes(const std::string& got) {
    const auto& rs = replies();
    std::vector<std::string> out;
    size_t pos = 0;
    for (size_t i = 0; i < rs.size(); ++i) {
        const char mark = char('a' + i), next = char('a' + i + 1);
        if (pos >= got.size() || got[pos] != mark) return {};
        ++pos;
        const std::string want = printable(rs[i].bytes);
        if (got.compare(pos, want.size(), want) == 0 && pos + want.size() < got.size() && got[pos + want.size()] == next) {
            out.push_back("intact");
            pos += want.size();
        } else if (pos < got.size() && got[pos] == next) {
            out.push_back("dropped");
        } else {
            const size_t n = got.find(next, pos);
            if (n == std::string::npos) return {};
            out.push_back("became " + got.substr(pos, n - pos));
            pos = n;
        }
    }
    if (got.substr(pos) != "h") return {};
    return out;
}

// A query the program writes, how it looks when it reaches the terminal, and
// the start of bropty's answer ("" for a set, which has none).
struct Query {
    const char* kind;  // the kind of its reply: "csi", "osc", "dcs", "" (none)
    const char* name;
    const char* escaped;  // for pty_child
    std::string raw;
    std::string answer;
};

} // namespace

int main(int argc, char** argv) {
    init_test();
    if (argc < 2) {
        std::printf("usage: test_pty_conpty_replies <pty_child>\n");
        return 2;
    }
    g_child = argv[1];
    start_watchdog("test_pty_conpty_replies");
    std::printf("Windows %s\n", windows_build().c_str());

    // Replies as bropty writes them, between markers (lower case: an
    // upper-case letter's key record comes with a Shift press).
    std::string inject;
    for (size_t i = 0; i < replies().size(); ++i) inject += char('a' + i) + replies()[i].bytes;
    inject += "h";

    // Which reply kinds this build's conhost passes to a VT-input program.
    std::map<std::string, bool> passes = {{"csi", true}, {"osc", true}, {"dcs", true}};

    arm("replies written to the console input", 90);
    for (const char* rd : {"file-vt", "consolew-vt", "records-vt", "file", "records"}) {
        const bool vt = has(rd, "-vt");
        const Probe p = probe(rd, "", inject);
        const std::vector<std::string> o = outcomes(p.got);
        CHECK(!o.empty());  // every marker, in order: the delivery is ours
        if (o.empty()) {
            std::printf("  %-12s got=%s\n", rd, p.got.c_str());
            continue;
        }
        for (size_t i = 0; i < o.size(); ++i) {
            const Reply& r = replies()[i];
            std::printf("  %-12s %-13s %s\n", rd, r.name, o[i].c_str());
            if (!vt) continue;  // conhost turns what it can into key events
            // Under VT input conhost passes or drops a sequence whole.
            CHECK(o[i] == "intact" || o[i] == "dropped");
            if (std::string(r.kind) == "csi") CHECK(o[i] == "intact");
            if (o[i] != "intact") passes[r.kind] = false;
        }
    }
    for (const auto& [kind, ok] : passes)
        std::printf("  this conhost %s %s replies\n", ok ? "passes" : "drops", kind.c_str());

    const std::vector<Query> queries = {
        {"osc", "OSC 10 ?", "\\e]10;?\\e\\", "\x1b]10;?", "\x1b]10;rgb:"},
        {"osc", "OSC 11 ? (ST)", "\\e]11;?\\e\\", "\x1b]11;?", "\x1b]11;rgb:"},
        {"osc", "OSC 11 ? (BEL)", "\\e]11;?\\a", "\x1b]11;?", "\x1b]11;rgb:"},
        {"osc", "OSC 12 ?", "\\e]12;?\\e\\", "\x1b]12;?", "\x1b]12;rgb:"},
        {"osc", "OSC 52 ?", "\\e]52;c;?\\e\\", "\x1b]52;c;?", "\x1b]52;c;aGVsbG8=\x1b\\"},
        {"osc", "OSC 4 ?", "\\e]4;1;?\\e\\", "\x1b]4;1;?", "\x1b]4;1;rgb:"},
        {"", "OSC 52 set", "\\e]52;c;aGk=\\e\\", "\x1b]52;c;aGk=\x1b\\", ""},
        {"", "OSC 10 set", "\\e]10;rgb:1/2/3\\e\\", "\x1b]10;rgb:1/2/3\x1b\\", ""},
        {"csi", "DA1", "\\e[c", "\x1b[c", "\x1b[?62;"},
        {"csi", "CPR", "\\e[6n", "\x1b[6n", "\x1b["},
        {"dcs", "DECRQSS", "\\eP$qm\\e\\", "\x1bP$qm", "\x1bP1$r"},
        {"csi", "CSI ? u", "\\e[?u", "\x1b[?u", "\x1b[?0u"},
    };

    arm("queries, end to end", 180);
    for (const char* rd : {"file-vt", "records-vt"}) {
        for (const Query& q : queries) {
            const Probe p = probe(rd, q.escaped);
            const bool forwarded = has(p.fed, q.raw);
            const std::string ours = q.answer.empty() ? "" : printable(p.replied);
            std::string reached = "nothing";
            if (!p.got.empty()) reached = !ours.empty() && p.got == ours ? "our answer" : "\"" + p.got + "\"";
            std::printf("  %-12s %-15s %-13s %-10s program got %s\n", rd, q.name,
                        forwarded ? "forwarded" : "not forwarded",
                        q.answer.empty() ? "" : p.replied.empty() ? "unanswered" : "answered", reached.c_str());
            CHECK(p.got.rfind("<no ", 0) != 0);
            if (q.answer.empty()) continue;
            // bropty answers what reaches it, and only that.
            if (forwarded) CHECK(p.replied.rfind(q.answer, 0) == 0);
            else CHECK(!has(p.replied, q.answer));
            if (std::string(q.kind) == "osc") {
                // conhost answers none of these itself: the program gets our
                // answer or nothing, and our answer when this conhost passes
                // OSC replies and forwarded the query.
                CHECK(p.got.empty() || (forwarded && p.got == ours));
                if (forwarded && passes["osc"]) CHECK(p.got == ours);
            }
            if (q.name == std::string("CSI ? u")) {
                CHECK(forwarded);
                CHECK(p.got == "<1b>[?0u");
            }
        }
    }

    g_deadline_ms = 0;
    return check::finish("test_pty_conpty_replies");
}
