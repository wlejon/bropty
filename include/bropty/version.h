#pragma once

#include <cstdint>
#include <string_view>

#define BROPTY_VERSION_MAJOR 0
#define BROPTY_VERSION_MINOR 1
#define BROPTY_VERSION_PATCH 0
#define BROPTY_VERSION_STRING "0.1.0"

namespace bropty {

[[nodiscard]] std::string_view version_string() noexcept;
[[nodiscard]] uint32_t version_major() noexcept;
[[nodiscard]] uint32_t version_minor() noexcept;
[[nodiscard]] uint32_t version_patch() noexcept;

} // namespace bropty
