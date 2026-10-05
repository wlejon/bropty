// Real programs through the platform pty (ConPTY / POSIX): output into the
// emulator, exit status, argument and environment passing, cwd, spawn
// failures, resize as the child sees it, raw key bytes as the child reads
// them, and an interactive shell.
//   test_pty <path-to-pty_child>
#include "pty_harness.h"

#include <cstdlib>
#include <cstring>

using namespace bropty;
using namespace ph;

namespace {

std::string hex(std::string_view s) {
    static const char* d = "0123456789abcdef";
    std::string out;
    for (unsigned char c : s) {
        out.push_back(d[c >> 4]);
        out.push_back(d[c & 15]);
    }
    return out;
}

// Send `bytes` to a `pty_child keys` and return the hex it reports.
std::string keys_roundtrip(std::string_view bytes) {
    Run r(child({"keys", std::to_string(bytes.size())}));
    CHECK(r.ok);
    if (!r.ok) return "";
    if (!r.wait_for_text("READY")) return "no READY";
    r.pty->write(bytes);
    if (!r.wait_for_text("KEYS:")) {
        r.pty->write(std::string(bytes.size(), 'x'));  // unstick a child that got fewer bytes
        r.wait_for_text("KEYS:", 3000ms);
    }
    std::string s = r.screen();
    size_t at = s.find("KEYS:");
    if (at == std::string::npos) return "no KEYS";
    size_t end = s.find_first_of("|~", at);
    return s.substr(at + 5, end == std::string::npos ? std::string::npos : end - at - 5);
}

} // namespace

int main(int argc, char** argv) {
    init_test();
    if (argc < 2) {
        std::printf("usage: test_pty <pty_child>\n");
        return 2;
    }
    g_child = argv[1];
    start_watchdog("test_pty");

    {
        arm("shell echo", 30);
        Run r(shell("echo HelloFromPty"));
        CHECK(r.ok);
        CHECK(r.pty->pid() > 0);
        CHECK(r.drain());
        CHECK(r.has("HelloFromPty"));
        r.pty->wait();
        CHECK(!r.pty->is_running());
        CHECK(r.pty->exit_code() == std::optional<int>(0));
    }
    {
        arm("exit status", 30);
        Run r(child({"exit", "7"}));
        CHECK(r.ok);
        // Polling must not consume the status (the old POSIX code reaped in is_running()).
        auto end = Clock::now() + 10s;
        while (r.pty->is_running() && Clock::now() < end) {
            CHECK(!r.pty->exit_code().has_value() || !r.pty->is_running());
            std::this_thread::sleep_for(2ms);
        }
        CHECK(r.pty->exit_code() == std::optional<int>(7));
        CHECK(r.pty->exit_code() == std::optional<int>(7));
        r.pty->wait();
        CHECK(r.pty->exit_code() == std::optional<int>(7));
        CHECK(r.drain());
        CHECK(r.pty->eof());
    }
    {
        arm("shell exit status", 30);
        Run r(shell("exit 9"));
        CHECK(r.ok);
        CHECK(r.pty->wait_for(10s));
        CHECK(r.pty->exit_code() == std::optional<int>(9));
    }
    {
        arm("arguments", 30);
        const std::vector<std::string> args = {"plain", "with space", "quote\"inside", "trailing\\", "x\\\\\"y z",
                                               "", "%PATH%", "^&|<>", "$HOME", "*"};
        std::vector<std::string> a = {"args"};
        a.insert(a.end(), args.begin(), args.end());
        Run r(child(a), 120, 30);
        CHECK(r.ok);
        CHECK(r.drain());
        std::string expect;
        for (const std::string& s : args) expect += "[" + s + "]|";
        expect.pop_back();
        CHECK_EQ(r.screen(), expect);
    }
    {
        arm("environment", 30);
#if defined(_WIN32)
        _putenv_s("BROPTY_PARENT_VAR", "inherited");
        _putenv_s("COLUMNS", "999");
#else
        setenv("BROPTY_PARENT_VAR", "inherited", 1);
        setenv("COLUMNS", "999", 1);
#endif
        PtyConfig c = child({"env", "BROPTY_X", "BROPTY_PARENT_VAR", "TERM", "COLORTERM", "COLUMNS", "BROPTY_GONE"});
        c.env = {{"BROPTY_X", "xyz 42"}, {"BROPTY_GONE", "1"}};
        c.env_unset = {"BROPTY_GONE"};
        Run r(c);
        CHECK(r.ok);
        CHECK(r.drain());
        CHECK_EQ(r.screen(), std::string("BROPTY_X=xyz 42|BROPTY_PARENT_VAR=inherited|TERM=xterm-256color|"
                                         "COLORTERM=truecolor|COLUMNS!unset|BROPTY_GONE!unset"));

        PtyConfig c2 = child({"env", "BROPTY_PARENT_VAR", "TERM"});
        c2.inherit_env = false;
#if defined(_WIN32)
        if (const char* sr = std::getenv("SystemRoot")) c2.env = {{"SystemRoot", sr}};
#endif
        Run r2(c2);
        CHECK(r2.ok);
        CHECK(r2.drain());
        CHECK_EQ(r2.screen(), std::string("BROPTY_PARENT_VAR!unset|TERM=xterm-256color"));
    }
    {
        arm("cwd", 30);
#if defined(_WIN32)
        PtyConfig c = shell("cd");
        c.cwd = "C:\\Windows";
        const char* want = "C:\\Windows";
#else
        PtyConfig c = shell("pwd");
        c.cwd = "/usr";
        const char* want = "/usr";
#endif
        Run r(c);
        CHECK(r.ok);
        CHECK(r.drain());
        CHECK(r.has(want));
    }
    {
        arm("spawn failures", 30);
        auto p = create_pty();
        PtyConfig c;
        c.command = "bropty-no-such-program-xyz";
        CHECK(!p->spawn(c));
        CHECK(!p->last_error().empty());
        CHECK(!p->is_running());
        CHECK(!p->exit_code().has_value());
        CHECK_EQ(p->write("x"), size_t(0));
        std::printf("  info: missing program: %s\n", p->last_error().c_str());

        auto q = create_pty();
        PtyConfig d = child({"exit", "0"});
        d.cwd = "/bropty/no/such/dir";
        CHECK(!q->spawn(d));
        CHECK(!q->last_error().empty());
        std::printf("  info: missing cwd: %s\n", q->last_error().c_str());

        auto twice = create_pty();
        CHECK(twice->spawn(child({"exit", "0"})));
        CHECK(!twice->spawn(child({"exit", "0"})));
        twice->wait();
    }
    {
        arm("resize seen by the child", 30);
        Run r(child({"size"}), 80, 24);
        CHECK(r.ok);
        CHECK(r.wait_for_text("SIZE 24x80"));
        r.session.resize(100, 30);
        r.pty->write("x");
        CHECK(r.wait_for_text("SIZE 30x100"));
        r.session.resize(50, 12);
        r.pty->write("x");
        CHECK(r.wait_for_text("SIZE 12x50"));
        r.pty->write("q");
        CHECK(r.pty->wait_for(10s));
        if (!r.has("SIZE 12x50")) std::printf("  screen: %s\n", r.screen().c_str());
    }
    {
        arm("passthrough of colour, addressing and wide text", 30);
        Run r(child({"print", "\\e[1;31mRED\\e[0m \\e[38;2;10;20;30mTRUE\\e[0m\\n\\e[6;10HAT\\e[8;1H\\u4E2D\\u6587|\\n"}));
        CHECK(r.ok);
        CHECK(r.drain());
        const Terminal& t = r.session.terminal();
        int red_row = -1;
        for (int y = 0; y < t.rows(); ++y)
            if (t.row_text(y).find("RED") != std::string::npos) red_row = y;
        CHECK(red_row >= 0);
        if (red_row >= 0) {
            int x = int(t.row_text(red_row).find("RED"));
            const Style& s = t.style(t.row(red_row).cells[x].style);
            CHECK(s.has(Attr_Bold));
            CHECK(!s.fg.is_default());
            int tx = int(t.row_text(red_row).find("TRUE"));
            CHECK(tx > 0);
            if (tx > 0) {
                Color fg = t.style(t.row(red_row).cells[tx].style).fg;
                CHECK(fg.is_rgb() && fg.rgb_value().to_u32() == 0x0A141Eu);
            }
        }
        CHECK_EQ(int(t.row_text(5).find("AT")), 9);
        CHECK(t.row(7).cells[0].cp() == 0x4E2D && t.row(7).cells[0].wide() == Wide::Lead);
        CHECK(t.row(7).cells[2].cp() == 0x6587);
        CHECK(t.row(7).cells[4].cp() == '|');
    }
    {
        arm("raw key bytes reach the child", 60);
        // What a program in raw mode reads. POSIX ptys pass bytes through
        // untouched; ConPTY parses input into key events and re-encodes it, so
        // only sequences it understands survive byte-exact there.
        CHECK_EQ(keys_roundtrip("a\r"), std::string("610d"));
        CHECK_EQ(keys_roundtrip("\x1b[A"), hex("\x1b[A"));
        CHECK_EQ(keys_roundtrip("\x1b[1;5C"), hex("\x1b[1;5C"));
        CHECK_EQ(keys_roundtrip("\x1b[15~"), hex("\x1b[15~"));
        CHECK_EQ(keys_roundtrip("\xc3\xa9"), hex("\xc3\xa9"));
        const char* kitty = "\x1b[97;5u";
        const char* sgr_mouse = "\x1b[<0;3;4M";
        const char* paste = "\x1b[200~hi\x1b[201~";
#if defined(_WIN32)
        std::printf("  info: ConPTY delivers kitty %s as %s\n", hex(kitty).c_str(), keys_roundtrip(kitty).c_str());
        std::printf("  info: ConPTY delivers SGR mouse %s as %s\n", hex(sgr_mouse).c_str(),
                    keys_roundtrip(sgr_mouse).c_str());
        std::printf("  info: ConPTY delivers paste %s as %s\n", hex(paste).c_str(), keys_roundtrip(paste).c_str());
#else
        CHECK_EQ(keys_roundtrip(kitty), hex(kitty));
        CHECK_EQ(keys_roundtrip(sgr_mouse), hex(sgr_mouse));
        CHECK_EQ(keys_roundtrip(paste), hex(paste));
        CHECK_EQ(keys_roundtrip(std::string("\x03", 1)), std::string("03"));  // raw: ^C is a byte, not SIGINT
#endif
    }
    {
        arm("interactive shell", 60);
#if defined(_WIN32)
        PtyConfig c;
        c.command = "cmd.exe";
        c.args = {"/d", "/q", "/k", "prompt", "$g"};
        // Output "marker123" without the echoed command line containing it.
        const char* marker_cmd = "set /p=marker<nul& set /a 100+23\r";
        const char* exit_cmd = "exit\r";
#else
        PtyConfig c;
        c.command = "/bin/sh";
        c.args = {"-i"};
        c.env = {{"PS1", "$ "}, {"ENV", ""}};
        const char* marker_cmd = "echo marker$((100+23))\n";
        const char* exit_cmd = "exit 3\n";
#endif
        Run r(c);
        CHECK(r.ok);
        std::this_thread::sleep_for(300ms);
        r.session.update();
        r.pty->write(marker_cmd);
        CHECK(r.wait_for_text("marker123"));
        if (!r.has("marker123")) std::printf("  screen: %s\n", r.screen().c_str());
        r.pty->write(exit_cmd);
        CHECK(r.pty->wait_for(10s));
        CHECK(r.drain());
#if !defined(_WIN32)
        CHECK(r.pty->exit_code() == std::optional<int>(3));
#endif
    }
    {
        arm("wakeup hook", 30);
        std::atomic<int> wakes{0};
        auto p = create_pty();
        p->set_wakeup([&] { ++wakes; });
        CHECK(p->spawn(child({"args", "x"})));
        CHECK(p->wait_for(10s));
        auto end = Clock::now() + 5s;
        while (!p->eof() && Clock::now() < end) {
            char buf[256];
            p->read_timeout(buf, sizeof buf, 50ms);
        }
        CHECK(p->eof());
        CHECK(wakes.load() >= 2);  // output, exit
    }

    g_deadline_ms = 0;
    return check::finish("test_pty");
}
