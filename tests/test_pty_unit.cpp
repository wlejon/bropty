// The pure pieces of the PTY layer: Windows argument quoting (checked against
// CommandLineToArgvW itself on Windows) and environment construction.
#include "bropty/pty.h"
#include "check.h"

#include <string>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#endif

using namespace bropty;
using pty_detail::build_environment;
using pty_detail::quote_windows_arg;
using pty_detail::windows_command_line;

namespace {

bool has(const std::vector<std::string>& env, const std::string& entry) {
    for (const std::string& e : env)
        if (e == entry) return true;
    return false;
}

bool has_name(const std::vector<std::string>& env, const std::string& name) {
    for (const std::string& e : env)
        if (e.compare(0, name.size() + 1, name + "=") == 0) return true;
    return false;
}

#if defined(_WIN32)
std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}
std::string narrow(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n > 0 ? n - 1 : 0), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}
std::vector<std::string> parse(const std::string& line) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(widen(line).c_str(), &argc);
    std::vector<std::string> out;
    for (int i = 0; i < argc; ++i) out.push_back(narrow(argv[i]));
    LocalFree(argv);
    return out;
}
#endif

} // namespace

int main() {
    init_test();

    // ---- quoting
    CHECK_EQ(quote_windows_arg("plain"), std::string("plain"));
    CHECK_EQ(quote_windows_arg(""), std::string("\"\""));
    CHECK_EQ(quote_windows_arg("a b"), std::string("\"a b\""));
    CHECK_EQ(quote_windows_arg("a\"b"), std::string("\"a\\\"b\""));
    CHECK_EQ(quote_windows_arg("C:\\dir\\"), std::string("C:\\dir\\"));  // no quoting needed: backslashes literal
    CHECK_EQ(quote_windows_arg("C:\\my dir\\"), std::string("\"C:\\my dir\\\\\""));
    CHECK_EQ(quote_windows_arg("a\\\"b"), std::string("\"a\\\\\\\"b\""));
    CHECK_EQ(quote_windows_arg("tab\there"), std::string("\"tab\there\""));
    CHECK_EQ(windows_command_line("C:\\Program Files\\x.exe", {"-a", "b c"}),
             std::string("\"C:\\Program Files\\x.exe\" -a \"b c\""));
    CHECK_EQ(windows_command_line("cmd.exe", {}), std::string("cmd.exe"));

    const std::vector<std::string> tricky = {
        "plain", "with space", "quote\"inside", "trailing\\", "trail space\\", "back\\\\slash\\\"q",
        "", "tab\there", "\"", "\\\"", "\\\\", "a\\\\\"b c", "%PATH%", "^&|<>", "ünïcødé ✓", "  ",
    };
#if defined(_WIN32)
    {
        std::vector<std::string> got = parse(windows_command_line("C:\\Program Files\\x.exe", tricky));
        CHECK_EQ(got.size(), tricky.size() + 1);
        if (got.size() == tricky.size() + 1) {
            CHECK_EQ(got[0], std::string("C:\\Program Files\\x.exe"));
            for (size_t i = 0; i < tricky.size(); ++i) CHECK_EQ(got[i + 1], tricky[i]);
        }
    }
#endif

    // ---- environment
    {
        PtyConfig c;
        std::vector<std::string> base = {"PATH=/bin", "HOME=/home/u", "TERM=screen", "COLUMNS=80", "LINES=24",
                                         "TERM_PROGRAM=OtherTerm", "KEEP=1", "DROP=1"};
        c.env = {{"FOO", "bar"}, {"HOME", "/tmp"}, {"FOO", "baz"}};
        c.env_unset = {"DROP"};
        std::vector<std::string> e = build_environment(base, c, false);
        CHECK(has(e, "PATH=/bin"));
        CHECK(has(e, "HOME=/tmp"));
        CHECK(has(e, "FOO=baz"));
        CHECK(!has(e, "FOO=bar"));
        CHECK(has(e, "KEEP=1"));
        CHECK(!has_name(e, "DROP"));
        CHECK(has(e, "TERM=xterm-256color"));
        CHECK(has(e, "COLORTERM=truecolor"));
        CHECK(!has_name(e, "COLUMNS"));
        CHECK(!has_name(e, "LINES"));
        CHECK(!has_name(e, "TERM_PROGRAM"));
        for (size_t i = 1; i < e.size(); ++i) CHECK(e[i - 1] <= e[i]);
    }
    {
        PtyConfig c;
        c.env = {{"TERM", "dumb"}};
        std::vector<std::string> e = build_environment({"TERM=screen"}, c, false);
        CHECK(has(e, "TERM=dumb"));  // explicit config wins over the default
    }
    {
        PtyConfig c;
        c.inherit_env = false;
        std::vector<std::string> e = build_environment({"PATH=/bin", "SECRET=1"}, c, false);
        CHECK(!has_name(e, "PATH"));
        CHECK(!has_name(e, "SECRET"));
        CHECK(has(e, "TERM=xterm-256color"));
    }
    {
        // Windows: names are case-insensitive; "=C:" drive entries survive and sort first.
        PtyConfig c;
        c.env = {{"path", "C:\\x"}};
        c.env_unset = {"temp"};
        std::vector<std::string> e =
            build_environment({"Path=C:\\Windows", "=C:=C:\\work", "TEMP=C:\\t", "windir=C:\\Windows"}, c, true);
        CHECK(has(e, "path=C:\\x"));
        CHECK(!has_name(e, "Path"));
        CHECK(!has_name(e, "TEMP"));
        CHECK(!e.empty() && e.front() == "=C:=C:\\work");
        CHECK(has(e, "windir=C:\\Windows"));
    }

    return check::finish("test_pty_unit");
}
