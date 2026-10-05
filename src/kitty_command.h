#pragma once
// Internal: the kitty graphics protocol's control data
// (`ESC _ G <key=value,...> [; <payload>] ESC \`).

#include <cstdint>
#include <string>
#include <string_view>

namespace bropty::detail {

struct KittyCommand {
    char action{'t'};        // a: t T q p d f a c
    uint32_t quiet{0};       // q
    uint32_t format{32};     // f: 24 32 100
    char medium{'d'};        // t: d f t s
    uint32_t width{0};       // s (a=a: animation state)
    uint32_t height{0};      // v (a=a: loops)
    uint32_t size{0};        // S
    uint32_t offset{0};      // O
    uint32_t id{0};          // i
    uint32_t number{0};      // I
    uint32_t placement{0};   // p
    char compression{0};     // o: z
    uint32_t more{0};        // m
    uint32_t x{0};           // x
    uint32_t y{0};           // y
    uint32_t w{0};           // w
    uint32_t h{0};           // h
    uint32_t cell_x{0};      // X (a=f: composition mode, a=c: source x)
    uint32_t cell_y{0};      // Y (a=f: background RGBA, a=c: source y)
    uint32_t cols{0};        // c (a=f: base frame, a=a: current frame, a=c: destination frame)
    uint32_t rows{0};        // r (a=f / a=a: frame number, a=c: source frame)
    uint32_t cursor{0};      // C (a=c: composition mode)
    uint32_t unicode{0};     // U
    int32_t z{0};            // z (a=f / a=a: gap)
    uint32_t parent_id{0};   // P
    uint32_t parent_placement{0};  // Q
    int32_t parent_dx{0};    // H
    int32_t parent_dy{0};    // V
    char del{'a'};           // d
    uint64_t present{0};     // bit (key - 'A') per key given
    std::string_view payload;

    [[nodiscard]] bool has(char key) const noexcept {
        return key >= 'A' && key <= 'z' && (present >> (key - 'A') & 1u);
    }
};

// Parse the APC payload after the leading 'G'. False (with a message in
// `error`) for malformed control data: an unknown or repeated key is
// ignored, a malformed value is an error (kitty answers EINVAL).
bool parse_kitty_command(std::string_view data, KittyCommand& out, std::string& error);

} // namespace bropty::detail
