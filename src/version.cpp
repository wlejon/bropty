#include "bropty/version.h"

namespace bropty {

std::string_view version_string() noexcept {
    return BROPTY_VERSION_STRING;
}

uint32_t version_major() noexcept {
    return BROPTY_VERSION_MAJOR;
}

uint32_t version_minor() noexcept {
    return BROPTY_VERSION_MINOR;
}

uint32_t version_patch() noexcept {
    return BROPTY_VERSION_PATCH;
}

} // namespace bropty
