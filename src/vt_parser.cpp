#include "bropty/vt_parser.h"
#include <charconv>

namespace bropty {

VtParser::VtParser(IVtHandler* handler)
    : handler_(handler) {}

void VtParser::feed(std::string_view data) {
    for (char c : data) {
        feed_byte(static_cast<uint8_t>(c));
    }
}

void VtParser::feed(const uint8_t* data, size_t length) {
    if (!data) return;
    for (size_t i = 0; i < length; ++i) {
        feed_byte(data[i]);
    }
}

void VtParser::reset() {
    state_ = VtState::Ground;
    csi_prefix_ = 0;
    csi_params_.clear();
    csi_intermediates_.clear();
    current_param_ = -1;
    in_subparam_ = false;
    osc_string_.clear();
    esc_intermediates_.clear();
    utf8_codepoint_ = 0;
    utf8_expected_bytes_ = 0;
    utf8_received_bytes_ = 0;
}

void VtParser::transition_to(VtState new_state) {
    state_ = new_state;
}

void VtParser::csi_flush_param() {
    if (in_subparam_) {
        // subparam is already in csi_params_.back().subparams
    } else {
        CsiParamValue val;
        val.value = current_param_;
        csi_params_.push_back(val);
    }
    current_param_ = -1;
    in_subparam_ = false;
}

void VtParser::handle_utf8(uint8_t byte) {
    if (utf8_expected_bytes_ == 0) {
        if ((byte & 0x80) == 0) {
            // ASCII printable
            if (handler_) handler_->on_print(byte);
        } else if ((byte & 0xE0) == 0xC0) {
            utf8_codepoint_ = byte & 0x1F;
            utf8_expected_bytes_ = 2;
            utf8_received_bytes_ = 1;
        } else if ((byte & 0xF0) == 0xE0) {
            utf8_codepoint_ = byte & 0x0F;
            utf8_expected_bytes_ = 3;
            utf8_received_bytes_ = 1;
        } else if ((byte & 0xF8) == 0xF0) {
            utf8_codepoint_ = byte & 0x07;
            utf8_expected_bytes_ = 4;
            utf8_received_bytes_ = 1;
        } else {
            // Invalid leading byte, fallback
            if (handler_) handler_->on_print(0xFFFD);
        }
    } else {
        if ((byte & 0xC0) == 0x80) {
            utf8_codepoint_ = (utf8_codepoint_ << 6) | (byte & 0x3F);
            utf8_received_bytes_++;
            if (utf8_received_bytes_ == utf8_expected_bytes_) {
                if (handler_) handler_->on_print(utf8_codepoint_);
                utf8_expected_bytes_ = 0;
                utf8_received_bytes_ = 0;
                utf8_codepoint_ = 0;
            }
        } else {
            // Invalid continuation
            if (handler_) handler_->on_print(0xFFFD);
            utf8_expected_bytes_ = 0;
            utf8_received_bytes_ = 0;
            utf8_codepoint_ = 0;
            // Reprocess current byte
            handle_utf8(byte);
        }
    }
}

void VtParser::feed_byte(uint8_t byte) {
    // Check for anywhere transitions (CAN, SUB, ESC, C1 controls)
    if (byte == 0x18 || byte == 0x1A) { // CAN, SUB
        if (handler_) handler_->on_execute(byte);
        reset();
        return;
    }

    if (byte == 0x1B) { // ESC
        // If in OSC and received ESC, wait for next char (usually '\' ST)
        if (state_ == VtState::OscString) {
            // We transition to Escape, but keep osc_string_ intact
            transition_to(VtState::Escape);
            return;
        }
        csi_prefix_ = 0;
        csi_params_.clear();
        csi_intermediates_.clear();
        current_param_ = -1;
        in_subparam_ = false;
        esc_intermediates_.clear();
        utf8_expected_bytes_ = 0;
        transition_to(VtState::Escape);
        return;
    }

    // 8-bit C1 controls
    if (byte == 0x9B) { // CSI
        csi_prefix_ = 0;
        csi_params_.clear();
        csi_intermediates_.clear();
        current_param_ = -1;
        in_subparam_ = false;
        transition_to(VtState::CsiEntry);
        return;
    }
    if (byte == 0x9D) { // OSC
        osc_string_.clear();
        transition_to(VtState::OscString);
        return;
    }
    if (byte == 0x9C) { // ST (String Terminator)
        if (state_ == VtState::OscString) {
            // Dispatch OSC
            size_t semicolon = osc_string_.find(';');
            int cmd = -1;
            std::string_view payload;
            if (semicolon != std::string::npos) {
                std::from_chars(osc_string_.data(), osc_string_.data() + semicolon, cmd);
                payload = std::string_view(osc_string_).substr(semicolon + 1);
            } else {
                std::from_chars(osc_string_.data(), osc_string_.data() + osc_string_.size(), cmd);
            }
            if (handler_) handler_->on_osc(cmd, payload);
            osc_string_.clear();
        }
        transition_to(VtState::Ground);
        return;
    }

    process_byte(byte);
}

void VtParser::process_byte(uint8_t byte) {
    switch (state_) {
        case VtState::Ground: {
            if (byte <= 0x1F) {
                if (handler_) handler_->on_execute(byte);
            } else if (byte == 0x7F) {
                // DEL: ignored in ground
            } else {
                handle_utf8(byte);
            }
            break;
        }

        case VtState::Escape: {
            if (byte <= 0x1F) {
                if (handler_) handler_->on_execute(byte);
                return;
            }
            if (byte >= 0x20 && byte <= 0x2F) {
                esc_intermediates_.push_back(static_cast<char>(byte));
                transition_to(VtState::EscapeIntermediate);
                return;
            }
            if (byte == '[') {
                csi_prefix_ = 0;
                csi_params_.clear();
                csi_intermediates_.clear();
                current_param_ = -1;
                in_subparam_ = false;
                transition_to(VtState::CsiEntry);
                return;
            }
            if (byte == ']') {
                osc_string_.clear();
                transition_to(VtState::OscString);
                return;
            }
            if (byte == 'P') {
                transition_to(VtState::DcsEntry);
                return;
            }
            if (byte == 'X' || byte == '^' || byte == '_') {
                transition_to(VtState::SosPmApcString);
                return;
            }
            if (byte == '\\') {
                // ESC \ terminates OSC if osc_string_ was pending
                if (!osc_string_.empty()) {
                    size_t semicolon = osc_string_.find(';');
                    int cmd = -1;
                    std::string_view payload;
                    if (semicolon != std::string::npos) {
                        std::from_chars(osc_string_.data(), osc_string_.data() + semicolon, cmd);
                        payload = std::string_view(osc_string_).substr(semicolon + 1);
                    } else {
                        std::from_chars(osc_string_.data(), osc_string_.data() + osc_string_.size(), cmd);
                    }
                    if (handler_) handler_->on_osc(cmd, payload);
                    osc_string_.clear();
                }
                transition_to(VtState::Ground);
                return;
            }

            // Normal ESC dispatch
            if (handler_) handler_->on_esc(static_cast<char>(byte), esc_intermediates_);
            transition_to(VtState::Ground);
            break;
        }

        case VtState::EscapeIntermediate: {
            if (byte <= 0x1F) {
                if (handler_) handler_->on_execute(byte);
                return;
            }
            if (byte >= 0x20 && byte <= 0x2F) {
                esc_intermediates_.push_back(static_cast<char>(byte));
                return;
            }
            if (byte >= 0x30 && byte <= 0x7E) {
                if (handler_) handler_->on_esc(static_cast<char>(byte), esc_intermediates_);
                transition_to(VtState::Ground);
                return;
            }
            transition_to(VtState::Ground);
            break;
        }

        case VtState::CsiEntry: {
            if (byte <= 0x1F) {
                if (handler_) handler_->on_execute(byte);
                return;
            }
            if (byte >= 0x3C && byte <= 0x3F) { // '<', '=', '>', '?'
                csi_prefix_ = static_cast<char>(byte);
                transition_to(VtState::CsiParam);
                return;
            }
            if (byte >= 0x30 && byte <= 0x39) { // '0'..'9'
                current_param_ = byte - '0';
                transition_to(VtState::CsiParam);
                return;
            }
            if (byte == ';') {
                csi_flush_param();
                transition_to(VtState::CsiParam);
                return;
            }
            if (byte == ':') {
                csi_flush_param();
                in_subparam_ = true;
                csi_params_.back().subparams.push_back(-1);
                transition_to(VtState::CsiParam);
                return;
            }
            if (byte >= 0x20 && byte <= 0x2F) {
                csi_intermediates_.push_back(static_cast<char>(byte));
                transition_to(VtState::CsiIntermediate);
                return;
            }
            if (byte >= 0x40 && byte <= 0x7E) {
                if (handler_) handler_->on_csi(static_cast<char>(byte), csi_params_, csi_intermediates_, csi_prefix_);
                transition_to(VtState::Ground);
                return;
            }
            transition_to(VtState::CsiIgnore);
            break;
        }

        case VtState::CsiParam: {
            if (byte <= 0x1F) {
                if (handler_) handler_->on_execute(byte);
                return;
            }
            if (byte >= 0x30 && byte <= 0x39) {
                int digit = byte - '0';
                if (in_subparam_) {
                    auto& sub = csi_params_.back().subparams.back();
                    sub = (sub == -1) ? digit : (sub * 10 + digit);
                } else {
                    current_param_ = (current_param_ == -1) ? digit : (current_param_ * 10 + digit);
                }
                return;
            }
            if (byte == ';') {
                csi_flush_param();
                return;
            }
            if (byte == ':') {
                if (!in_subparam_) {
                    csi_flush_param();
                    in_subparam_ = true;
                }
                csi_params_.back().subparams.push_back(-1);
                return;
            }
            if (byte >= 0x20 && byte <= 0x2F) {
                csi_flush_param();
                csi_intermediates_.push_back(static_cast<char>(byte));
                transition_to(VtState::CsiIntermediate);
                return;
            }
            if (byte >= 0x40 && byte <= 0x7E) {
                csi_flush_param();
                if (handler_) handler_->on_csi(static_cast<char>(byte), csi_params_, csi_intermediates_, csi_prefix_);
                transition_to(VtState::Ground);
                return;
            }
            transition_to(VtState::CsiIgnore);
            break;
        }

        case VtState::CsiIntermediate: {
            if (byte <= 0x1F) {
                if (handler_) handler_->on_execute(byte);
                return;
            }
            if (byte >= 0x20 && byte <= 0x2F) {
                csi_intermediates_.push_back(static_cast<char>(byte));
                return;
            }
            if (byte >= 0x40 && byte <= 0x7E) {
                if (handler_) handler_->on_csi(static_cast<char>(byte), csi_params_, csi_intermediates_, csi_prefix_);
                transition_to(VtState::Ground);
                return;
            }
            transition_to(VtState::CsiIgnore);
            break;
        }

        case VtState::CsiIgnore: {
            if (byte <= 0x1F) {
                if (handler_) handler_->on_execute(byte);
                return;
            }
            if (byte >= 0x40 && byte <= 0x7E) {
                transition_to(VtState::Ground);
            }
            break;
        }

        case VtState::OscString: {
            if (byte == 0x07) { // BEL terminates OSC
                size_t semicolon = osc_string_.find(';');
                int cmd = -1;
                std::string_view payload;
                if (semicolon != std::string::npos) {
                    std::from_chars(osc_string_.data(), osc_string_.data() + semicolon, cmd);
                    payload = std::string_view(osc_string_).substr(semicolon + 1);
                } else {
                    std::from_chars(osc_string_.data(), osc_string_.data() + osc_string_.size(), cmd);
                }
                if (handler_) handler_->on_osc(cmd, payload);
                osc_string_.clear();
                transition_to(VtState::Ground);
                return;
            }
            if (byte >= 0x20 || byte == 0x09) {
                osc_string_.push_back(static_cast<char>(byte));
            }
            break;
        }

        case VtState::DcsEntry:
        case VtState::DcsParam:
        case VtState::DcsIntermediate:
        case VtState::DcsPassthrough:
        case VtState::DcsIgnore:
        case VtState::SosPmApcString: {
            if (byte == 0x07) { // BEL
                transition_to(VtState::Ground);
            }
            break;
        }
    }
}

} // namespace bropty
