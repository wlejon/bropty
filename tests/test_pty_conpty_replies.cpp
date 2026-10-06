// What a native console program receives of the terminal's replies through
// ConPTY, pinned. The losses are conhost's, not bropty's: the bytes bropty
// writes to the console's input pipe are the replies exactly (the CSI and DCS
// ones arrive intact), but
//  * conhost's VT input parser discards every OSC sequence it is given
//    (BEL- or ST-terminated, OSC 4 / 11 / 52 alike), in every input mode:
//    ReadFile, ReadConsoleW and ReadConsoleInputW, with and without
//    ENABLE_VIRTUAL_TERMINAL_INPUT. The program receives nothing in its place.
//  * conhost's output side never forwards the OSC 10 / 11 / 12 colour
//    queries or the OSC 52 clipboard query to the terminal (nor answers
//    them), while OSC 4 queries, OSC 52 / OSC 10 sets and unknown CSI queries
//    do reach it.
//  * DA1, CPR (DSR 6) and DECRQSS are answered by conhost itself, from its
//    own state: they never reach the terminal.
//  * Without ENABLE_VIRTUAL_TERMINAL_INPUT, CSI / DCS replies are not passed
//    through either (conhost turns what it can into key events).
// Each check here fails if a Windows update changes one of these, so the
// documentation in pty.h can be revised (and replies that start arriving
// can be relied on).
//   test_pty_conpty_replies <path-to-pty_child>
#include "pty_harness.h"
#include "test_common.h"

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
    std::printf("  %-12s %-18s got=%s\n", reader.c_str(), query.c_str(), p.got.c_str());
    return p;
}

bool has(const std::string& s, const std::string& needle) { return s.find(needle) != std::string::npos; }

} // namespace

int main(int argc, char** argv) {
    init_test();
    if (argc < 2) {
        std::printf("usage: test_pty_conpty_replies <pty_child>\n");
        return 2;
    }
    g_child = argv[1];
    start_watchdog("test_pty_conpty_replies");

    // Replies as bropty writes them, between markers: DA1, CPR, OSC 11 (BEL),
    // OSC 11 (ST), OSC 52, OSC 4, DECRQSS. (Lower case: an upper-case
    // letter's key record comes with a Shift press.)
    const std::string inject = std::string("a\x1b[?62;22;52c") + "b\x1b[5;10R" + "c\x1b]11;rgb:0000/0000/0000\x07" +
                               "d\x1b]11;rgb:1111/2222/3333\x1b\\" + "e\x1b]52;c;aGVsbG8=\x1b\\" +
                               "f\x1b]4;1;rgb:cdcd/0000/0000\x1b\\" + "g\x1bP1$r0m\x1b\\" + "h";
    const std::string csi = "a<1b>[?62;22;52cb<1b>[5;10Rc";
    const std::string dcs = "g<1b>P1$r0m<1b>\\h";

    arm("VT input: CSI and DCS replies intact, every OSC reply gone", 60);
    for (const char* rd : {"file-vt", "consolew-vt", "records-vt"}) {
        const Probe p = probe(rd, "", inject);
        const std::string got = p.got;
        CHECK(has(got, csi));
        CHECK(has(got, "cdefg"));  // the OSC replies left nothing behind
        CHECK(has(got, dcs));
        CHECK(!has(got, "]11;") && !has(got, "]52;") && !has(got, "]4;") && !has(got, "aGVsbG8"));
    }

    arm("no VT input: no reply passes through", 60);
    for (const char* rd : {"file", "records"}) {
        const Probe p = probe(rd, "", inject);
        CHECK(has(p.got, "cdefg"));
        CHECK(!has(p.got, "<1b>[") && !has(p.got, "]11;") && !has(p.got, "]52;") && !has(p.got, "]4;"));
    }

    arm("queries the console forwards and swallows", 30);
    {
        const std::string seqs = "<1>\\e]10;?\\e\\<2>\\e]11;?\\a<3>\\e]12;?\\e\\<4>\\e]52;c;?\\e\\"
                                 "<5>\\e]4;1;?\\e\\<6>\\e]52;c;aGk=\\e\\<7>\\e]10;rgb:1/2/3\\e\\<8>\\e[?u<9>";
        const Probe p = probe("records-vt", seqs);
        CHECK(!has(p.fed, "]10;?") && !has(p.fed, "]11;?") && !has(p.fed, "]12;?") && !has(p.fed, "]52;c;?"));
        CHECK(has(p.fed, "\x1b]4;1;?\x1b\\"));
        CHECK(has(p.fed, "\x1b]52;c;aGk=\x1b\\"));
        CHECK(has(p.fed, "\x1b]10;rgb:1/2/3\x1b\\"));
        CHECK(has(p.fed, "\x1b[?u"));
    }

    arm("end to end", 120);
    for (const char* rd : {"file-vt", "records-vt"}) {
        // OSC 11 / OSC 52 queries: never seen, never answered.
        for (const char* q : {"\\e]11;?\\e\\", "\\e]11;?\\a", "\\e]52;c;?\\e\\"}) {
            const Probe p = probe(rd, q);
            CHECK(p.replied.empty());
            CHECK(p.got.empty());
        }
        // OSC 4: bropty answers, the answer is lost on the way in.
        Probe p = probe(rd, "\\e]4;1;?\\e\\");
        CHECK(has(p.replied, "\x1b]4;1;rgb:"));
        CHECK(p.got.empty());
        // DA1: conhost's own answer, bropty never asked.
        p = probe(rd, "\\e[c");
        CHECK(p.replied.empty());
        CHECK(p.got.rfind("<1b>[?", 0) == 0 && has(p.got, "c") && !has(p.got, "?62;22;52c"));
        // A CSI query conhost does not know: bropty's reply arrives.
        p = probe(rd, "\\e[?u");
        CHECK(p.replied == "\x1b[?0u");
        CHECK(p.got == "<1b>[?0u");
    }

    g_deadline_ms = 0;
    return check::finish("test_pty_conpty_replies");
}
