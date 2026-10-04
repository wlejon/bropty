#pragma once
// Minimal test harness. Checks never abort and are never compiled out: every
// failure is printed with its location and counted, and main() returns the
// count, so a Release build fails exactly like a Debug one.

#include "test_common.h"

#include <cstdio>
#include <string>
#include <string_view>

namespace check {

inline int g_failures = 0;
inline int g_checks = 0;

inline std::string escape(std::string_view s) {
    std::string out;
    for (unsigned char c : s) {
        if (c == 0x1b) out += "\\e";
        else if (c == '\r') out += "\\r";
        else if (c == '\n') out += "\\n";
        else if (c == '\a') out += "\\a";
        else if (c < 0x20 || c == 0x7f) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\x%02x", c);
            out += buf;
        } else {
            out.push_back(char(c));
        }
    }
    return out;
}

inline std::string show(const std::string& s) { return "\"" + escape(s) + "\""; }
inline std::string show(std::string_view s) { return show(std::string(s)); }
inline std::string show(const char* s) { return show(std::string(s)); }
inline std::string show(bool b) { return b ? "true" : "false"; }
inline std::string show(char32_t c) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "U+%04X", unsigned(c));
    return buf;
}
template <class T>
std::string show(const T& v) {
    if constexpr (std::is_enum_v<T>) return std::to_string(static_cast<long long>(v));
    else return std::to_string(v);
}

inline void fail(const char* file, int line, const std::string& what) {
    ++g_failures;
    std::printf("FAIL %s:%d: %s\n", file, line, what.c_str());
    std::fflush(stdout);
}

template <class A, class B>
void eq(const A& a, const B& b, const char* ea, const char* eb, const char* file, int line) {
    ++g_checks;
    if (!(a == b)) fail(file, line, std::string(ea) + " == " + eb + "\n     got  " + show(a) + "\n     want " + show(b));
}

inline int finish(const char* name) {
    std::printf("[%s] %d checks, %d failed\n", name, g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

} // namespace check

#define CHECK(cond)                                                       \
    do {                                                                  \
        ++::check::g_checks;                                              \
        if (!(cond)) ::check::fail(__FILE__, __LINE__, "CHECK(" #cond ")"); \
    } while (0)

#define CHECK_EQ(a, b) ::check::eq((a), (b), #a, #b, __FILE__, __LINE__)
