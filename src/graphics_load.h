#pragma once
// Internal: turning a graphics transmission into pixels. Reading the
// transmission medium (kitty t=f / t=t / t=s, each gated by GraphicsOptions),
// zlib decompression, raw RGB / RGBA, and compressed formats through the
// host's decoder. Errors are kitty-style "CODE:message" strings.

#include "bropty/graphics.h"
#include "kitty_command.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bropty {
class TerminalHost;
}

namespace bropty::detail {

// For t=f, t=t and t=s, `data` holds the (base64-decoded) path or shared
// memory name on entry and the bytes it names on success. t=d: unchanged.
bool read_medium(const KittyCommand& c, std::vector<uint8_t>& data, const GraphicsOptions& o, std::string& error);

// Decode kitty pixel data (o=z, then f=24 / 32 / 100) into one RGBA frame.
bool decode_kitty_pixels(const KittyCommand& c, std::vector<uint8_t>& data, const GraphicsOptions& o,
                         TerminalHost* host, DecodedImage& out, std::string& error);

// A compressed image through the host's decoder, checked against the limits.
bool decode_with_host(std::string_view data, const GraphicsOptions& o, TerminalHost* host, DecodedImage& out,
                      std::string& error);

// Whether `path` is somewhere kitty's temp-file medium may read and delete:
// inside a temporary directory, with "tty-graphics-protocol" in its path.
bool acceptable_temp_file(const std::string& path);

} // namespace bropty::detail
