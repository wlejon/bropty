#include "bropty/osc_handler.h"
#include <charconv>

namespace bropty {

namespace {

constexpr char kBase64Chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789+/";

} // namespace

std::string base64_encode(std::string_view input) {
    std::string out;
    out.reserve(((input.size() + 2) / 3) * 4);

    size_t i = 0;
    while (i < input.size()) {
        size_t rem = input.size() - i;
        uint32_t octet_a = static_cast<uint8_t>(input[i]);
        uint32_t octet_b = (rem > 1) ? static_cast<uint8_t>(input[i + 1]) : 0;
        uint32_t octet_c = (rem > 2) ? static_cast<uint8_t>(input[i + 2]) : 0;

        uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;

        out.push_back(kBase64Chars[(triple >> 18) & 0x3F]);
        out.push_back(kBase64Chars[(triple >> 12) & 0x3F]);
        out.push_back(rem > 1 ? kBase64Chars[(triple >> 6) & 0x3F] : '=');
        out.push_back(rem > 2 ? kBase64Chars[triple & 0x3F] : '=');

        i += 3;
    }
    return out;
}

std::string base64_decode(std::string_view input) {
    std::string out;
    std::vector<int> table(256, -1);
    for (int i = 0; i < 64; ++i) {
        table[static_cast<uint8_t>(kBase64Chars[i])] = i;
    }

    uint32_t val = 0;
    int valb = -8;
    for (char c : input) {
        uint8_t uc = static_cast<uint8_t>(c);
        if (table[uc] == -1) continue; // Skip non-base64 chars (e.g. padding/newlines)
        val = (val << 6) | table[uc];
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<char>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

OscHandler::OscHandler() {
    hyperlink_pool_.emplace_back(""); // Index 0 = empty/none
}

std::string_view OscHandler::get_hyperlink_url(uint16_t id) const noexcept {
    if (id == 0 || id >= hyperlink_pool_.size()) {
        return {};
    }
    return hyperlink_pool_[id];
}

uint16_t OscHandler::register_hyperlink(std::string_view url) {
    if (url.empty()) return 0;
    for (size_t i = 1; i < hyperlink_pool_.size(); ++i) {
        if (hyperlink_pool_[i] == url) {
            return static_cast<uint16_t>(i);
        }
    }
    if (hyperlink_pool_.size() >= 65535) {
        return 0; // Overflow guard
    }
    hyperlink_pool_.emplace_back(url);
    return static_cast<uint16_t>(hyperlink_pool_.size() - 1);
}

void OscHandler::handle_osc(int command, std::string_view payload) {
    switch (command) {
        case 0: // Set window title and icon
        case 2: // Set window title
            title_ = std::string(payload);
            if (title_cb_) title_cb_(title_);
            break;
        case 7: // Set current working directory
            cwd_ = std::string(payload);
            if (cwd_cb_) cwd_cb_(cwd_);
            break;
        case 8: // Hyperlink
            handle_hyperlink(payload);
            break;
        case 52: // Clipboard access
            handle_clipboard(payload);
            break;
        case 133: // Shell integration
            handle_shell_integration(payload);
            break;
        default:
            break;
    }
}

void OscHandler::handle_hyperlink(std::string_view payload) {
    // Format: params;url
    size_t semicolon = payload.find(';');
    if (semicolon == std::string_view::npos) {
        current_hyperlink_id_ = 0;
        return;
    }

    std::string_view url = payload.substr(semicolon + 1);
    if (url.empty()) {
        current_hyperlink_id_ = 0;
    } else {
        current_hyperlink_id_ = register_hyperlink(url);
    }
}

void OscHandler::handle_clipboard(std::string_view payload) {
    // Format: targets;data
    size_t semicolon = payload.find(';');
    if (semicolon == std::string_view::npos) return;

    std::string_view data = payload.substr(semicolon + 1);
    if (data == "?") {
        if (clip_read_cb_ && response_cb_) {
            std::string content = clip_read_cb_();
            std::string encoded = base64_encode(content);
            std::string resp = "\x1b]52;c;" + encoded + "\x1b\\";
            response_cb_(resp);
        }
    } else {
        if (clip_write_cb_) {
            std::string decoded = base64_decode(data);
            clip_write_cb_(decoded);
        }
    }
}

void OscHandler::handle_shell_integration(std::string_view payload) {
    if (payload.empty()) return;

    ShellMarker marker{};
    char code = payload[0];
    switch (code) {
        case 'A': marker.type = ShellMarkerType::PromptStart; break;
        case 'B': marker.type = ShellMarkerType::CommandStart; break;
        case 'C': marker.type = ShellMarkerType::CommandExecuted; break;
        case 'D': {
            marker.type = ShellMarkerType::CommandFinished;
            if (payload.size() > 2 && payload[1] == ';') {
                std::from_chars(payload.data() + 2, payload.data() + payload.size(), marker.exit_code);
            }
            break;
        }
        default:
            return;
    }

    if (shell_marker_cb_) {
        shell_marker_cb_(marker);
    }
}

} // namespace bropty
