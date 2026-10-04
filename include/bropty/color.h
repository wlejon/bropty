#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace bropty {

enum class ColorType : uint8_t {
    Default = 0,
    Indexed = 1,
    Rgb = 2
};

struct Rgb {
    uint8_t r{0};
    uint8_t g{0};
    uint8_t b{0};

    constexpr bool operator==(const Rgb& other) const noexcept {
        return r == other.r && g == other.g && b == other.b;
    }
    constexpr bool operator!=(const Rgb& other) const noexcept {
        return !(*this == other);
    }
    [[nodiscard]] constexpr uint32_t to_u32() const noexcept {
        return (static_cast<uint32_t>(r) << 16) |
               (static_cast<uint32_t>(g) << 8) |
               static_cast<uint32_t>(b);
    }
};

class Color {
public:
    constexpr Color() noexcept : type_(ColorType::Default), index_(0), rgb_{0, 0, 0} {}

    static constexpr Color default_color() noexcept {
        return Color();
    }

    static constexpr Color from_index(uint8_t idx) noexcept {
        Color c;
        c.type_ = ColorType::Indexed;
        c.index_ = idx;
        return c;
    }

    static constexpr Color from_rgb(uint8_t r, uint8_t g, uint8_t b) noexcept {
        Color c;
        c.type_ = ColorType::Rgb;
        c.rgb_ = {r, g, b};
        return c;
    }

    [[nodiscard]] constexpr ColorType type() const noexcept { return type_; }
    [[nodiscard]] constexpr bool is_default() const noexcept { return type_ == ColorType::Default; }
    [[nodiscard]] constexpr bool is_indexed() const noexcept { return type_ == ColorType::Indexed; }
    [[nodiscard]] constexpr bool is_rgb() const noexcept { return type_ == ColorType::Rgb; }

    [[nodiscard]] constexpr uint8_t index() const noexcept { return index_; }
    [[nodiscard]] constexpr Rgb rgb() const noexcept { return rgb_; }

    constexpr bool operator==(const Color& other) const noexcept {
        if (type_ != other.type_) return false;
        if (type_ == ColorType::Indexed) return index_ == other.index_;
        if (type_ == ColorType::Rgb) return rgb_ == other.rgb_;
        return true;
    }
    constexpr bool operator!=(const Color& other) const noexcept {
        return !(*this == other);
    }

    // Resolves this color to concrete RGB against standard or supplied palette
    [[nodiscard]] Rgb resolve(bool is_fg = true, const Rgb* custom_palette_256 = nullptr) const noexcept;

    // Standard 256-color palette lookup
    static Rgb get_indexed_rgb(uint8_t index) noexcept;
    static Rgb default_fg_rgb() noexcept;
    static Rgb default_bg_rgb() noexcept;

private:
    ColorType type_{ColorType::Default};
    uint8_t index_{0};
    Rgb rgb_{0, 0, 0};
};

} // namespace bropty
