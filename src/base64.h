#pragma once
// Internal: base64 (RFC 4648) for graphics payloads. Decoding appends to the
// output, so chunked transmissions (kitty `m=1`, iTerm2 FilePart) decode
// each chunk as it arrives. Whitespace is skipped; '=' padding may end any
// chunk; missing padding is accepted. Any other byte is an error.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bropty::detail {

// Decoded size of `n` base64 characters (an upper bound when there is padding).
[[nodiscard]] constexpr size_t base64_decoded_bound(size_t n) noexcept { return (n + 3) / 4 * 3; }

bool base64_decode_append(std::string_view in, std::vector<uint8_t>& out);
bool base64_decode_append(std::string_view in, std::string& out);

std::string base64_encode(std::string_view in);

} // namespace bropty::detail
