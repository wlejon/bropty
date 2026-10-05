// The codecs inline images rest on: base64, the zlib / DEFLATE inflater
// (against stb_image's inflater as the reference, on gzip's and ImageMagick's
// real streams, stb_image_write's, and fuzzed ones), and kitty's placeholder
// diacritics table.
#include "image_helpers.h"

#include "base64.h"
#include "inflate.h"
#include "placeholder.h"

#include <random>

using namespace bropty;
using namespace bropty::detail;

namespace {

std::string_view sv(const std::vector<uint8_t>& v) {
    return std::string_view(reinterpret_cast<const char*>(v.data()), v.size());
}

void base64_cases() {
    std::string out;
    CHECK(base64_decode_append("aGVsbG8=", out));
    CHECK_EQ(out, std::string("hello"));
    out.clear();
    CHECK(base64_decode_append("aGVsbG8", out));  // missing padding
    CHECK_EQ(out, std::string("hello"));
    out.clear();
    CHECK(base64_decode_append("aGVs\r\nbG8=\n", out));  // whitespace
    CHECK_EQ(out, std::string("hello"));
    // Chunks appended one after another (kitty m=1).
    out.clear();
    CHECK(base64_decode_append("aGVs", out));
    CHECK(base64_decode_append("bG8=", out));
    CHECK_EQ(out, std::string("hello"));
    // URL-safe alphabet.
    out.clear();
    CHECK(base64_decode_append("-_8=", out));
    CHECK_EQ(out, std::string("\xfb\xff"));
    out = "keep";
    CHECK(!base64_decode_append("ab$d", out));
    CHECK_EQ(out, std::string("keep"));  // a failure appends nothing
    // Round trip over every byte length up to 300.
    std::mt19937 rng(5);
    for (size_t n = 0; n < 300; ++n) {
        std::string raw(n, '\0');
        for (char& c : raw) c = char(rng());
        std::string back;
        CHECK(base64_decode_append(base64_encode(raw), back));
        if (back != raw) CHECK_EQ(n, size_t(~0));
    }
}

void inflate_basic() {
    std::vector<uint8_t> out;
    // zlib.compress(b"hello") (fixed Huffman).
    const std::string hello("\x78\x9c\xcb\x48\xcd\xc9\xc9\x07\x00\x06\x2c\x02\x15", 13);
    CHECK_EQ(zlib_inflate(hello, out, 100), InflateStatus::Ok);
    CHECK_EQ(std::string(sv(out)), std::string("hello"));
    CHECK_EQ(zlib_inflate(hello, out, 4), InflateStatus::TooLarge);
    CHECK(out.size() <= 4);
    std::string bad = hello;
    bad[12] ^= 1;
    CHECK_EQ(zlib_inflate(bad, out, 100), InflateStatus::BadChecksum);
    CHECK_EQ(zlib_inflate(hello.substr(0, 8), out, 100), InflateStatus::Truncated);
    CHECK_EQ(zlib_inflate(std::string("\x78\x9d", 2) + hello.substr(2), out, 100), InflateStatus::BadHeader);
    CHECK_EQ(zlib_inflate(std::string("\x78\xbb\0\0\0\0", 6), out, 100), InflateStatus::BadHeader);  // FDICT
    // Stored block: BFINAL=1, BTYPE=00, LEN=3, NLEN=~3.
    const std::string stored("\x78\x01\x01\x03\x00\xfc\xff" "abc" "\x02\x4d\x01\x27", 14);
    CHECK_EQ(zlib_inflate(stored, out, 100), InflateStatus::Ok);
    CHECK_EQ(std::string(sv(out)), std::string("abc"));
    std::string badlen = stored;
    badlen[5] = 0;
    CHECK_EQ(zlib_inflate(badlen, out, 100), InflateStatus::Corrupt);
    // Reserved block type 3.
    CHECK_EQ(raw_inflate(std::string("\x07", 1), out, 100), InflateStatus::Corrupt);
    // A distance reaching before the start: fixed block, literal 'a', then
    // length 3 distance 2.
    // Bits (LSB first): BFINAL 1, BTYPE 01, 'a' = 0x91 (8 bits 10010001
    // reversed into the stream), len 257 = 0000001, dist code 1 = 00001.
    std::vector<uint8_t> bits;
    uint32_t acc = 0;
    int n = 0;
    auto put = [&](uint32_t v, int count, bool huff) {
        for (int i = 0; i < count; ++i) {
            const uint32_t bit = huff ? (v >> (count - 1 - i)) & 1 : (v >> i) & 1;
            acc |= bit << n;
            if (++n == 8) {
                bits.push_back(uint8_t(acc));
                acc = 0;
                n = 0;
            }
        }
    };
    put(1, 1, false);
    put(1, 2, false);
    put(0x30 + 'a', 8, true);
    put(1, 7, true);   // length code 257 = length 3
    put(1, 5, true);   // distance code 1 = distance 2
    put(0, 7, true);   // end of block
    if (n) bits.push_back(uint8_t(acc));
    CHECK_EQ(raw_inflate(std::string_view(reinterpret_cast<const char*>(bits.data()), bits.size()), out, 100),
             InflateStatus::Corrupt);
}

// gzip files: strip the 10-byte header (-n: no name, no time) and 8-byte trailer.
void inflate_gzip_fixture(const char* name) {
    const std::string gz = ih::read_file(ih::fixture(name));
    CHECK(gz.size() > 18);
    if (gz.size() <= 18) return;
    const std::string_view body(gz.data() + 10, gz.size() - 18);
    const uint8_t* t = reinterpret_cast<const uint8_t*>(gz.data() + gz.size() - 4);
    const size_t isize = size_t(t[0]) | size_t(t[1]) << 8 | size_t(t[2]) << 16 | size_t(t[3]) << 24;
    std::vector<uint8_t> out;
    size_t used = 0;
    CHECK_EQ(raw_inflate(body, out, isize, &used), InflateStatus::Ok);
    CHECK_EQ(out.size(), isize);
    CHECK_EQ(used, body.size());
    std::string ref;
    CHECK(ih::stb_inflate(body, ref, true));
    CHECK(std::string(sv(out)) == ref);
    CHECK_EQ(raw_inflate(body, out, isize - 1), InflateStatus::TooLarge);
}

// PNG IDAT streams (ImageMagick's zlib, dynamic Huffman).
void inflate_png_idat(const char* name) {
    const std::string png = ih::read_file(ih::fixture(name));
    std::string z;
    for (size_t p = 8; p + 12 <= png.size();) {
        const uint8_t* q = reinterpret_cast<const uint8_t*>(png.data() + p);
        const size_t len = size_t(q[0]) << 24 | size_t(q[1]) << 16 | size_t(q[2]) << 8 | q[3];
        if (png.compare(p + 4, 4, "IDAT") == 0) z.append(png, p + 8, len);
        p += 12 + len;
    }
    CHECK(!z.empty());
    std::vector<uint8_t> out;
    CHECK_EQ(zlib_inflate(z, out, 1u << 24), InflateStatus::Ok);
    std::string ref;
    CHECK(ih::stb_inflate(z, ref, false));
    CHECK(std::string(sv(out)) == ref);
}

void inflate_differential() {
    std::mt19937 rng(42);
    for (int iter = 0; iter < 300; ++iter) {
        // Data with structure: runs, repeats and noise, so every block kind
        // and long matches appear.
        std::string data;
        const size_t target = rng() % 20000;
        while (data.size() < target) {
            switch (rng() % 3) {
            case 0: data.append(rng() % 300, char('a' + rng() % 26)); break;
            case 1:
                if (!data.empty()) {
                    const size_t from = rng() % data.size();
                    data += data.substr(from, rng() % 258);
                }
                break;
            default:
                for (int i = int(rng() % 64); i > 0; --i) data.push_back(char(rng()));
            }
        }
        const std::string z = ih::zlib_compress(data, 1 + int(rng() % 9));
        std::vector<uint8_t> out;
        CHECK_EQ(zlib_inflate(z, out, data.size()), InflateStatus::Ok);
        if (std::string(sv(out)) != data) CHECK_EQ(iter, -1);
    }
}

// Mutated and truncated streams: never crash, never exceed max_out, and
// agree with stb whenever both accept the stream.
void inflate_fuzz() {
    std::mt19937 rng(7);
    const std::string seeds[] = {
        ih::zlib_compress(std::string(5000, 'x') + "hello world hello world", 8),
        ih::zlib_compress("The quick brown fox jumps over the lazy dog", 5),
        ih::read_file(ih::fixture("grad.ppm.gz")).substr(10),
    };
    int agreed = 0;
    for (int iter = 0; iter < 20000; ++iter) {
        std::string s = seeds[iter % 3];
        const int muts = 1 + int(rng() % 4);
        for (int m = 0; m < muts && !s.empty(); ++m) {
            switch (rng() % 4) {
            case 0: s[rng() % s.size()] ^= char(1 << (rng() % 8)); break;
            case 1: s[rng() % s.size()] = char(rng()); break;
            case 2: s.resize(rng() % s.size()); break;
            default: s.insert(rng() % (s.size() + 1), 1, char(rng()));
            }
        }
        const size_t cap = 1 + rng() % 70000;
        std::vector<uint8_t> out;
        const bool raw = iter % 3 == 2;
        const InflateStatus st = raw ? raw_inflate(s, out, cap) : zlib_inflate(s, out, cap);
        if (out.size() > cap) CHECK_EQ(out.size(), cap);
        std::string ref;
        if (st == InflateStatus::Ok && ih::stb_inflate(s, ref, raw) && ref.size() <= cap) {
            if (std::string(sv(out)) != ref) CHECK_EQ(iter, -1);
            ++agreed;
        }
    }
    CHECK(agreed > 0);
}

void diacritics() {
    CHECK_EQ(kitty_diacritic(1), char32_t(0x0305));
    CHECK_EQ(kitty_diacritic(2), char32_t(0x030D));
    CHECK_EQ(kitty_diacritic(297), char32_t(0x1D244));
    CHECK_EQ(kitty_diacritic(0), char32_t(0));
    CHECK_EQ(kitty_diacritic(298), char32_t(0));
    for (uint32_t n = 1; n <= uint32_t(kKittyDiacritics); ++n) {
        CHECK_EQ(kitty_diacritic_number(kitty_diacritic(n)), n);
        if (n > 1) CHECK(kitty_diacritic(n) > kitty_diacritic(n - 1));
    }
    CHECK_EQ(kitty_diacritic_number(U'a'), uint32_t(0));
    CHECK_EQ(kitty_diacritic_number(0x0306), uint32_t(0));
}

} // namespace

int main() {
    init_test();
    base64_cases();
    inflate_basic();
    inflate_gzip_fixture("grad.ppm.gz");
    inflate_gzip_fixture("grad1.ppm.gz");
    inflate_png_idat("src_a.png");
    inflate_png_idat("src_b.png");
    inflate_differential();
    inflate_fuzz();
    diacritics();
    return check::finish("test_image_codecs");
}
