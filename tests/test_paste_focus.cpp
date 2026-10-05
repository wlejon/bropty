// Paste (plain and bracketed) and focus reports.
#include "bropty/input.h"
#include "check.h"

#include <string>
#include <vector>

using namespace bropty;

namespace {

const std::string kStart = "\x1b[200~";
const std::string kEnd = "\x1b[201~";

// A bracketed paste is safe when the only ESC bytes (and the only C1 code
// points) in it are those of the start and end markers.
bool bracket_safe(const std::string& out) {
    if (out.size() < kStart.size() + kEnd.size()) return false;
    if (out.compare(0, kStart.size(), kStart) != 0) return false;
    if (out.compare(out.size() - kEnd.size(), kEnd.size(), kEnd) != 0) return false;
    std::string body = out.substr(kStart.size(), out.size() - kStart.size() - kEnd.size());
    for (size_t i = 0; i < body.size(); ++i) {
        auto c = static_cast<unsigned char>(body[i]);
        if (c == 0x1b || (c < 0x20 && c != '\t' && c != '\r') || c == 0x7f) return false;
        if (c == 0xc2 && i + 1 < body.size()) {
            auto d = static_cast<unsigned char>(body[i + 1]);
            if (d >= 0x80 && d <= 0x9f) return false;  // U+0080..U+009F
        }
        if (c >= 0x80 && c <= 0x9f) {
            // only legal as a UTF-8 continuation byte
            if (i == 0) return false;
        }
    }
    return true;
}

void plain_paste() {
    CHECK_EQ(encode_paste("hello", false), std::string("hello"));
    CHECK_EQ(encode_paste("a\r\nb\nc\rd", false), std::string("a\rb\rc\rd"));
    CHECK_EQ(encode_paste("\n\n", false), std::string("\r\r"));
    CHECK_EQ(encode_paste("\r\n\r\n", false), std::string("\r\r"));
    // Nothing else is touched without bracketed paste.
    CHECK_EQ(encode_paste("x\x1b[201~y\x03\t", false), std::string("x\x1b[201~y\x03\t"));
    CHECK_EQ(encode_paste("\xc3\xa9\x9b", false), std::string("\xc3\xa9\x9b"));
    CHECK_EQ(encode_paste("", false), std::string());
}

void bracketed_paste() {
    CHECK_EQ(encode_paste("hello", true), kStart + "hello" + kEnd);
    CHECK_EQ(encode_paste("", true), kStart + kEnd);
    CHECK_EQ(encode_paste("a\r\nb\nc\rd\te", true), kStart + "a\rb\rc\rd\te" + kEnd);
    CHECK_EQ(encode_paste("h\xc3\xa9llo \xf0\x9f\x98\x80", true), kStart + "h\xc3\xa9llo \xf0\x9f\x98\x80" + kEnd);
    // The end marker cannot be injected.
    CHECK_EQ(encode_paste("a\x1b[201~b", true), kStart + "a[201~b" + kEnd);
    CHECK_EQ(encode_paste("\x1b\x1b[201~[201~", true), kStart + "[201~[201~" + kEnd);
    CHECK_EQ(encode_paste("\x1b[200~nested\x1b[201~", true), kStart + "[200~nested[201~" + kEnd);
    // C0 (other than HT / CR / LF), DEL and C1 are removed.
    CHECK_EQ(encode_paste("a\x03\x07\x08\x7f" "b", true), kStart + "ab" + kEnd);
    CHECK_EQ(encode_paste("a\xc2\x9b" "201~b", true), kStart + "a201~b" + kEnd);  // U+009B CSI
    CHECK_EQ(encode_paste("a\xc2\x85" "b", true), kStart + "ab" + kEnd);          // U+0085 NEL
    // Bytes that are not UTF-8 (a raw 8-bit CSI) become U+FFFD.
    CHECK_EQ(encode_paste("a\x9b" "201~", true), kStart + "a\xef\xbf\xbd" "201~" + kEnd);
    CHECK_EQ(encode_paste("\xc3", true), kStart + "\xef\xbf\xbd" + kEnd);
    CHECK_EQ(encode_paste("\xe2\x82", true), kStart + "\xef\xbf\xbd" + kEnd);
    CHECK_EQ(encode_paste("\xed\xa0\x80", true), kStart + "\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd" + kEnd);

    // Exhaustively: ESC / 8-bit CSI inserted at every split position of a
    // payload made of end markers never yields an early end marker.
    const std::string payload = "x\x1b[201~y[201~\x1b\x1b[2\x1b" "01~z";
    std::vector<std::string> inserts = {"\x1b", "\x1b[", "\x9b", "\xc2\x9b", "\x1b\x1b", "\r\n"};
    for (const std::string& ins : inserts) {
        for (size_t i = 0; i <= payload.size(); ++i) {
            std::string text = payload.substr(0, i) + ins + payload.substr(i);
            std::string out = encode_paste(text, true);
            CHECK(bracket_safe(out));
            CHECK(out.find(kEnd) == out.size() - kEnd.size());
        }
    }
}

void focus() {
    CHECK_EQ(encode_focus(true, true), std::string("\x1b[I"));
    CHECK_EQ(encode_focus(false, true), std::string("\x1b[O"));
    CHECK_EQ(encode_focus(true, false), std::string());
    CHECK_EQ(encode_focus(false, false), std::string());
}

} // namespace

int main() {
    init_test();
    plain_paste();
    bracketed_paste();
    focus();
    return check::finish("test_paste_focus");
}
