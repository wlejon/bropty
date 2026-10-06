// IPtyProcess::foreground_process() with a real shell running a chain of
// programs (shell -> child -> grandchild): the shell at its prompt, the
// program it runs while it runs (POSIX: the leader of the foreground process
// group, or its lowest live member once the leader has gone; Windows: the
// youngest console descendant), the shell again once that exits, and nothing
// after the shell exits.
//   test_pty_foreground <path-to-pty_child>
#include "pty_harness.h"
#include "test_common.h"

#if !defined(_WIN32)
#include <unistd.h>
#endif

using namespace bropty;
using namespace ph;

namespace {

// Pump the session until the foreground process satisfies `pred`; the last
// answer read is left in `last`.
template <class Pred>
bool wait_foreground(Run& r, Pred pred, std::optional<ProcessInfo>& last, std::chrono::milliseconds limit = 10000ms) {
    const auto end = Clock::now() + limit;
    for (;;) {
        r.session.update();
        last = r.pty->foreground_process();
        if (pred(last)) return true;
        if (Clock::now() >= end) return false;
        std::this_thread::sleep_for(20ms);
    }
}

void show(const char* what, const std::optional<ProcessInfo>& p) {
    if (!p) {
        std::printf("  %s: none\n", what);
        return;
    }
    std::printf("  %s: pid=%lld name=%s path=%s cmd=%s\n", what, static_cast<long long>(p->pid), p->name.c_str(),
                p->path.c_str(), p->command_line.c_str());
}

long long field(const std::string& screen, const std::string& key) {
    const size_t at = screen.find(key);
    return at == std::string::npos ? 0 : std::atoll(screen.c_str() + at + key.size());
}

bool ends_with(const std::string& s, const std::string& tail) {
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

} // namespace

int main(int argc, char** argv) {
    init_test();
    if (argc < 2) {
        std::printf("usage: test_pty_foreground <pty_child>\n");
        return 2;
    }
    g_child = argv[1];
    start_watchdog("test_pty_foreground");

    PtyConfig c;
#if defined(_WIN32)
    c.command = "cmd.exe";
    c.windows_command_line = "cmd.exe /d";
    std::string child = g_child;
    for (char& ch : child)
        if (ch == '/') ch = '\\';
    const std::string enter = "\r";
    const std::string shell_name = "cmd.exe";
    const std::string child_name = "pty_child.exe";
#else
    c.command = "/bin/sh";
    const std::string child = g_child;
    const std::string enter = "\n";
    const std::string shell_name = "sh";
    const std::string child_name = "pty_child";
#endif
    Run r(c, 100, 30);
    CHECK(r.ok);
    if (!r.ok) return check::finish("test_pty_foreground");
    const int64_t shell_pid = r.pty->pid();
    std::optional<ProcessInfo> fg;
    auto is_pid = [](int64_t pid) { return [pid](const std::optional<ProcessInfo>& p) { return p && p->pid == pid; }; };

    arm("the shell at its prompt", 30);
    CHECK(wait_foreground(r, is_pid(shell_pid), fg));
    show("prompt", fg);
    CHECK(fg && fg->name == shell_name);
    CHECK(fg && !fg->command_line.empty());
    CHECK(fg && !fg->path.empty());

    arm("shell -> child -> grandchild -> great-grandchild", 30);
    CHECK(r.pty->write("\"" + child + "\" nest 2" + enter) > 0);
    CHECK(r.wait_for_text("NEST-READY"));
    const std::string screen = r.screen();
    const long long leaf = field(screen, "NEST-READY pid=");
    CHECK(leaf > 0);
#if defined(_WIN32)
    // The deepest program of the chain, which the others wait for.
    CHECK(wait_foreground(r, is_pid(leaf), fg));
    show("running", fg);
    CHECK(fg && fg->name == child_name);
    CHECK(fg && ends_with(fg->command_line, "nest 0"));
#else
    // The foreground job: the leader of its process group, the program the
    // shell started, whose descendants share the group.
    const long long pgid = field(screen, "pgid=");
    CHECK(pgid > 0 && pgid != shell_pid && pgid != leaf);
    CHECK(wait_foreground(r, is_pid(pgid), fg));
    show("running", fg);
    CHECK(fg && fg->name == child_name);
    CHECK(fg && ends_with(fg->command_line, "nest 2"));
    CHECK(fg && ends_with(fg->path, child_name));
#endif

    arm("the job ends: the shell again", 30);
    CHECK(r.pty->write("q" + enter) > 0);  // cooked input: the line arrives with Enter
    CHECK(wait_foreground(r, is_pid(shell_pid), fg));
    show("after", fg);

#if !defined(_WIN32)
    arm("a pipeline whose leader has exited: the group's live member", 30);
    CHECK(r.pty->write("true | \"" + child + "\" nest 0" + enter) > 0);
    const auto end = Clock::now() + 10s;
    long long piped = 0;
    while (Clock::now() < end) {
        r.session.update();
        const std::string s = r.screen();
        const size_t first = s.find("NEST-READY");
        const size_t second = first == std::string::npos ? first : s.find("NEST-READY", first + 1);
        if (second != std::string::npos) {
            piped = std::atoll(s.c_str() + second + 15);
            break;
        }
        std::this_thread::sleep_for(10ms);
    }
    CHECK(piped > 0);
    CHECK(wait_foreground(r, is_pid(piped), fg));
    show("pipeline", fg);
    CHECK(r.pty->write("q" + enter) > 0);
    CHECK(wait_foreground(r, is_pid(shell_pid), fg));
#endif

    arm("the shell exits: nothing", 30);
    CHECK(r.pty->write("exit" + enter) > 0);
    CHECK(r.drain());
    CHECK(r.pty->wait_for(5s));
    CHECK(!r.pty->foreground_process());

    arm("describe_process and posix_command_line", 10);
    ProcessInfo self;
#if defined(_WIN32)
    CHECK(pty_detail::describe_process(int64_t(GetCurrentProcessId()), self));
    CHECK(self.name == "test_pty_foreground.exe");
#else
    CHECK(pty_detail::describe_process(int64_t(getpid()), self));
    CHECK(self.name == "test_pty_foreground");
#endif
    CHECK(!pty_detail::describe_process(0, self));
    CHECK(pty_detail::posix_command_line({"vim", "a b.txt", "it's", ""}) == "vim 'a b.txt' 'it'\\''s' ''");
    CHECK(pty_detail::posix_command_line({"ls", "-la", "/tmp/x=1"}) == "ls -la /tmp/x=1");

    g_deadline_ms = 0;
    return check::finish("test_pty_foreground");
}
