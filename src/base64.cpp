#include "base64.h"

namespace bropty::detail {

namespace {

constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// 0..63 for alphabet bytes (both the standard and the URL-safe alphabet),
// 64 for whitespace, 65 for '=', 255 for anything else.
struct Table {
    uint8_t v[256];
    constexpr Table() : v{} {
        for (int i = 0; i < 256; ++i) v[i] = 255;
        for (int i = 0; i < 64; ++i) v[uint8_t(kAlphabet[i])] = uint8_t(i);
        v[uint8_t('-')] = 62;
        v[uint8_t('_')] = 63;
        v[uint8_t(' ')] = v[uint8_t('\n')] = v[uint8_t('\r')] = v[uint8_t('\t')] = 64;
        v[uint8_t('=')] = 65;
    }
};
constexpr Table kTable;

template <class Out>
bool decode(std::string_view in, Out& out) {
    const size_t base = out.size();
    out.resize(base + base64_decoded_bound(in.size()));
    auto* dst = reinterpret_cast<uint8_t*>(out.data()) + base;
    const auto* p = reinterpret_cast<const uint8_t*>(in.data());
    const auto* end = p + in.size();
    // Fast path: groups of four alphabet bytes.
    while (end - p >= 4) {
        const uint32_t a = kTable.v[p[0]], b = kTable.v[p[1]], c = kTable.v[p[2]], d = kTable.v[p[3]];
        if ((a | b | c | d) >= 64) break;
        const uint32_t v = a << 18 | b << 12 | c << 6 | d;
        dst[0] = uint8_t(v >> 16);
        dst[1] = uint8_t(v >> 8);
        dst[2] = uint8_t(v);
        dst += 3;
        p += 4;
    }
    uint32_t acc = 0;
    int bits = 0;
    bool padded = false;
    for (; p < end; ++p) {
        const uint8_t v = kTable.v[*p];
        if (v == 64) continue;
        if (v == 65) {  // padding: the group ends here; drop its partial bits
            padded = true;
            acc = 0;
            bits = 0;
            continue;
        }
        if (v == 255) {
            out.resize(base);
            return false;
        }
        if (padded) {  // data after padding: a new group (concatenated chunks)
            padded = false;
        }
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            *dst++ = uint8_t(acc >> bits);
            acc &= (1u << bits) - 1u;
        }
    }
    out.resize(size_t(dst - reinterpret_cast<uint8_t*>(out.data())));
    return true;
}

} // namespace

bool base64_decode_append(std::string_view in, std::vector<uint8_t>& out) { return decode(in, out); }
bool base64_decode_append(std::string_view in, std::string& out) { return decode(in, out); }

std::string base64_encode(std::string_view in) {
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    while (i + 2 < in.size()) {
        const uint32_t v = (uint32_t(uint8_t(in[i])) << 16) | (uint32_t(uint8_t(in[i + 1])) << 8) | uint8_t(in[i + 2]);
        out.push_back(kAlphabet[v >> 18]);
        out.push_back(kAlphabet[(v >> 12) & 63]);
        out.push_back(kAlphabet[(v >> 6) & 63]);
        out.push_back(kAlphabet[v & 63]);
        i += 3;
    }
    if (i < in.size()) {
        uint32_t v = uint32_t(uint8_t(in[i])) << 16;
        if (i + 1 < in.size()) v |= uint32_t(uint8_t(in[i + 1])) << 8;
        out.push_back(kAlphabet[v >> 18]);
        out.push_back(kAlphabet[(v >> 12) & 63]);
        out.push_back(i + 1 < in.size() ? kAlphabet[(v >> 6) & 63] : '=');
        out.push_back('=');
    }
    return out;
}

} // namespace bropty::detail
