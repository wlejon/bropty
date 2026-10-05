// zlib / DEFLATE decompression (see inflate.h for why this is in-tree).
// Canonical Huffman decoding: codes up to 9 bits resolve through one table
// lookup, longer ones through the count/symbol walk of zlib's puff.c.
#include "inflate.h"

#include <cstring>

namespace bropty::detail {

namespace {

// LSB-first bit reader. Past the end of the input it feeds zero bytes and
// counts them, so a decoder can peek freely; consuming one of those padding
// bits means the stream was truncated (see truncated()).
struct Bits {
    const uint8_t* p;
    const uint8_t* end;
    const uint8_t* begin;
    uint64_t buf{0};
    int cnt{0};          // valid bits in buf
    uint64_t pad_bits{0};  // zero bits appended past the end (all still at the top of buf unless consumed)

    Bits(const uint8_t* b, size_t n) : p(b), end(b + n), begin(b) {}

    void refill() noexcept {
        while (cnt <= 56) {
            if (p < end) {
                buf |= uint64_t(*p++) << cnt;
            } else {
                pad_bits += 8;
            }
            cnt += 8;
        }
    }
    uint32_t take(int n) noexcept {  // n <= 32
        if (cnt < n) refill();
        const uint32_t v = uint32_t(buf & ((uint64_t(1) << n) - 1));
        buf >>= n;
        cnt -= n;
        return v;
    }
    [[nodiscard]] bool truncated() const noexcept { return uint64_t(cnt) < pad_bits; }
    void align() noexcept {
        const int r = cnt & 7;
        buf >>= r;
        cnt -= r;
    }
    // Input bytes consumed so far (whole bytes still buffered are given back).
    [[nodiscard]] size_t consumed() const noexcept {
        const uint64_t real_buffered = uint64_t(cnt) > pad_bits ? uint64_t(cnt) - pad_bits : 0;
        return size_t(p - begin) - size_t(real_buffered / 8);
    }
};

constexpr int kFastBits = 9;
constexpr int kMaxBits = 15;

struct Huffman {
    uint16_t fast[1 << kFastBits];  // (length << 9) | symbol, 0 = not in the table
    uint16_t count[kMaxBits + 1];
    uint16_t symbol[320];

    // False for an over-subscribed code. Incomplete codes are accepted; an
    // unused code met while decoding is an error.
    bool build(const uint8_t* lens, int n) noexcept {
        std::memset(count, 0, sizeof count);
        for (int i = 0; i < n; ++i) ++count[lens[i]];
        count[0] = 0;
        int left = 1;
        for (int len = 1; len <= kMaxBits; ++len) {
            left <<= 1;
            left -= count[len];
            if (left < 0) return false;
        }
        uint16_t offs[kMaxBits + 2];
        offs[1] = 0;
        for (int len = 1; len <= kMaxBits; ++len) offs[len + 1] = uint16_t(offs[len] + count[len]);
        for (int i = 0; i < n; ++i)
            if (lens[i]) symbol[offs[lens[i]]++] = uint16_t(i);
        std::memset(fast, 0, sizeof fast);
        uint32_t next[kMaxBits + 1];
        uint32_t code = 0;
        next[0] = 0;
        for (int len = 1; len <= kMaxBits; ++len) {
            code = (code + count[len - 1]) << 1;
            next[len] = code;
        }
        for (int i = 0; i < n; ++i) {
            const int len = lens[i];
            if (!len) continue;
            const uint32_t c = next[len]++;
            if (len > kFastBits) continue;
            uint32_t r = 0;
            for (int k = 0; k < len; ++k) r |= ((c >> k) & 1u) << (len - 1 - k);
            for (uint32_t k = r; k < (1u << kFastBits); k += 1u << len) fast[k] = uint16_t(len << 9 | i);
        }
        return true;
    }

    // The next symbol, or -1 for a code the table does not contain.
    int decode(Bits& b) const noexcept {
        if (b.cnt < kMaxBits) b.refill();
        const uint16_t e = fast[b.buf & ((1u << kFastBits) - 1)];
        if (e) {
            const int len = e >> 9;
            b.buf >>= len;
            b.cnt -= len;
            return e & 511;
        }
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= kMaxBits; ++len) {
            code |= int(b.buf & 1u);
            b.buf >>= 1;
            --b.cnt;
            const int c = count[len];
            if (code - c < first) return symbol[index + (code - first)];
            index += c;
            first += c;
            first <<= 1;
            code <<= 1;
        }
        return -1;
    }
};

constexpr uint16_t kLenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                   31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr uint16_t kDistBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,    65,    97,    129,
                                    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

struct Out {
    std::vector<uint8_t>& v;
    size_t pos{0};
    size_t max;

    bool ensure(size_t n) {
        if (n > max - pos) return false;
        if (pos + n > v.size()) {
            size_t cap = v.size() < 4096 ? 4096 : v.size() * 2;
            while (cap < pos + n) cap *= 2;
            if (cap > max) cap = max;
            v.resize(cap);
        }
        return true;
    }
};

InflateStatus codes(Bits& b, Out& o, const Huffman& lit, const Huffman& dist) {
    for (;;) {
        const int sym = lit.decode(b);
        if (sym < 0) return b.truncated() ? InflateStatus::Truncated : InflateStatus::Corrupt;
        if (b.truncated()) return InflateStatus::Truncated;
        if (sym < 256) {
            if (!o.ensure(1)) return InflateStatus::TooLarge;
            o.v[o.pos++] = uint8_t(sym);
            continue;
        }
        if (sym == 256) return InflateStatus::Ok;
        const int li = sym - 257;
        if (li >= 29) return InflateStatus::Corrupt;
        const size_t len = kLenBase[li] + b.take(kLenExtra[li]);
        const int ds = dist.decode(b);
        if (ds < 0 || ds >= 30) return b.truncated() ? InflateStatus::Truncated : InflateStatus::Corrupt;
        const size_t d = kDistBase[ds] + b.take(kDistExtra[ds]);
        if (b.truncated()) return InflateStatus::Truncated;
        if (d > o.pos) return InflateStatus::Corrupt;
        if (!o.ensure(len)) return InflateStatus::TooLarge;
        uint8_t* dst = o.v.data() + o.pos;
        const uint8_t* src = dst - d;
        if (d >= len) {
            std::memcpy(dst, src, len);
        } else {
            for (size_t i = 0; i < len; ++i) dst[i] = src[i];
        }
        o.pos += len;
    }
}

InflateStatus stored(Bits& b, Out& o) {
    b.align();
    const uint32_t len = b.take(16);
    const uint32_t nlen = b.take(16);
    if (b.truncated()) return InflateStatus::Truncated;
    if (len != (~nlen & 0xFFFFu)) return InflateStatus::Corrupt;
    if (!o.ensure(len)) return InflateStatus::TooLarge;
    uint32_t left = len;
    // Whole bytes still in the bit buffer first, then straight from the input.
    while (left && b.cnt >= 8) {
        o.v[o.pos++] = uint8_t(b.take(8));
        --left;
    }
    if (b.truncated()) return InflateStatus::Truncated;
    if (size_t(b.end - b.p) < left) return InflateStatus::Truncated;
    std::memcpy(o.v.data() + o.pos, b.p, left);
    o.pos += left;
    b.p += left;
    return InflateStatus::Ok;
}

InflateStatus fixed(Bits& b, Out& o) {
    static const struct Tables {
        Huffman lit, dist;
        Tables() {
            uint8_t lens[288];
            int i = 0;
            for (; i < 144; ++i) lens[i] = 8;
            for (; i < 256; ++i) lens[i] = 9;
            for (; i < 280; ++i) lens[i] = 7;
            for (; i < 288; ++i) lens[i] = 8;
            lit.build(lens, 288);
            for (i = 0; i < 30; ++i) lens[i] = 5;
            dist.build(lens, 30);
        }
    } t;
    return codes(b, o, t.lit, t.dist);
}

InflateStatus dynamic(Bits& b, Out& o) {
    static constexpr uint8_t kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    const int nlen = int(b.take(5)) + 257;
    const int ndist = int(b.take(5)) + 1;
    const int ncode = int(b.take(4)) + 4;
    if (b.truncated()) return InflateStatus::Truncated;
    if (nlen > 286 || ndist > 30) return InflateStatus::Corrupt;
    uint8_t lens[320] = {};
    for (int i = 0; i < ncode; ++i) lens[kOrder[i]] = uint8_t(b.take(3));
    Huffman lencode;
    if (!lencode.build(lens, 19)) return InflateStatus::Corrupt;
    std::memset(lens, 0, sizeof lens);
    int index = 0;
    while (index < nlen + ndist) {
        int sym = lencode.decode(b);
        if (sym < 0) return b.truncated() ? InflateStatus::Truncated : InflateStatus::Corrupt;
        if (sym < 16) {
            lens[index++] = uint8_t(sym);
            continue;
        }
        uint8_t len = 0;
        int rep;
        if (sym == 16) {
            if (index == 0) return InflateStatus::Corrupt;
            len = lens[index - 1];
            rep = 3 + int(b.take(2));
        } else if (sym == 17) {
            rep = 3 + int(b.take(3));
        } else {
            rep = 11 + int(b.take(7));
        }
        if (index + rep > nlen + ndist) return InflateStatus::Corrupt;
        while (rep--) lens[index++] = len;
    }
    if (b.truncated()) return InflateStatus::Truncated;
    if (lens[256] == 0) return InflateStatus::Corrupt;  // no end-of-block code
    Huffman lit, dist;
    if (!lit.build(lens, nlen) || !dist.build(lens + nlen, ndist)) return InflateStatus::Corrupt;
    return codes(b, o, lit, dist);
}

InflateStatus run(Bits& b, Out& o) {
    for (;;) {
        const uint32_t last = b.take(1);
        const uint32_t type = b.take(2);
        if (b.truncated()) return InflateStatus::Truncated;
        InflateStatus s;
        switch (type) {
        case 0: s = stored(b, o); break;
        case 1: s = fixed(b, o); break;
        case 2: s = dynamic(b, o); break;
        default: return InflateStatus::Corrupt;
        }
        if (s != InflateStatus::Ok) return s;
        if (last) return InflateStatus::Ok;
    }
}

uint32_t adler32(const uint8_t* p, size_t n) noexcept {
    uint32_t a = 1, b = 0;
    while (n) {
        const size_t k = n < 5552 ? n : 5552;
        for (size_t i = 0; i < k; ++i) {
            a += p[i];
            b += a;
        }
        a %= 65521u;
        b %= 65521u;
        p += k;
        n -= k;
    }
    return (b << 16) | a;
}

} // namespace

InflateStatus raw_inflate(std::string_view in, std::vector<uint8_t>& out, size_t max_out, size_t* consumed) {
    out.clear();
    Bits b(reinterpret_cast<const uint8_t*>(in.data()), in.size());
    Out o{out, 0, max_out};
    InflateStatus s = run(b, o);
    out.resize(o.pos);
    if (consumed) *consumed = b.consumed();
    return s;
}

InflateStatus zlib_inflate(std::string_view in, std::vector<uint8_t>& out, size_t max_out) {
    out.clear();
    if (in.size() < 2) return InflateStatus::Truncated;
    const uint8_t cmf = uint8_t(in[0]), flg = uint8_t(in[1]);
    if ((cmf & 0x0F) != 8 || (cmf >> 4) > 7 || ((uint32_t(cmf) << 8 | flg) % 31) != 0 || (flg & 0x20))
        return InflateStatus::BadHeader;
    Bits b(reinterpret_cast<const uint8_t*>(in.data()) + 2, in.size() - 2);
    Out o{out, 0, max_out};
    InflateStatus s = run(b, o);
    out.resize(o.pos);
    if (s != InflateStatus::Ok) return s;
    b.align();
    uint32_t want = 0;
    for (int i = 0; i < 4; ++i) want = (want << 8) | b.take(8);
    if (b.truncated()) return InflateStatus::Truncated;
    if (want != adler32(out.data(), out.size())) return InflateStatus::BadChecksum;
    return InflateStatus::Ok;
}

const char* inflate_status_name(InflateStatus s) noexcept {
    switch (s) {
    case InflateStatus::Ok: return "ok";
    case InflateStatus::BadHeader: return "bad zlib header";
    case InflateStatus::Corrupt: return "corrupt deflate data";
    case InflateStatus::Truncated: return "truncated deflate data";
    case InflateStatus::TooLarge: return "decompressed data too large";
    case InflateStatus::BadChecksum: return "zlib checksum mismatch";
    }
    return "?";
}

} // namespace bropty::detail
