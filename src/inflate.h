#pragma once
// Internal: a zlib (RFC 1950) / DEFLATE (RFC 1951) decompressor for the
// kitty graphics protocol's `o=z` payloads.
//
// Why bropty carries its own instead of linking zlib or asking the host: the
// kitty protocol requires `o=z` for raw RGB / RGBA (kitten icat and most
// clients compress), so without it basic kitty support would depend on the
// embedder; the expected output size is known up front, which lets the
// decoder refuse a "zip bomb" the moment it would exceed it; and a single
// self-contained file (canonical Huffman decoding with a 9-bit lookup table,
// no preset dictionaries) keeps bropty free of third-party code and licences.
// It checks everything a malicious stream can get wrong: header, block
// types, over-subscribed or incomplete codes, distances before the start of
// the output, the Adler-32 trailer, and truncation.

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace bropty::detail {

enum class InflateStatus : uint8_t {
    Ok,
    BadHeader,   // not a zlib stream, or it needs a preset dictionary
    Corrupt,     // invalid block type, code, length or distance
    Truncated,   // the input ended inside the stream
    TooLarge,    // the output would exceed max_out
    BadChecksum, // Adler-32 mismatch
};

// Decompress a zlib stream into `out` (replacing its contents). At most
// `max_out` bytes are produced; more is TooLarge. Bytes after the trailer
// are ignored.
InflateStatus zlib_inflate(std::string_view in, std::vector<uint8_t>& out, size_t max_out);

// A raw DEFLATE stream (no zlib header or trailer). `consumed` receives the
// number of input bytes used.
InflateStatus raw_inflate(std::string_view in, std::vector<uint8_t>& out, size_t max_out, size_t* consumed = nullptr);

[[nodiscard]] const char* inflate_status_name(InflateStatus s) noexcept;

} // namespace bropty::detail
