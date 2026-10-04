#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace bropty {

enum class ShellMarkerType : uint8_t {
    PromptStart = 0,    // 'A'
    CommandStart = 1,   // 'B'
    CommandExecuted = 2,// 'C'
    CommandFinished = 3 // 'D'
};

struct ShellMarker {
    ShellMarkerType type;
    int exit_code{0}; // For CommandFinished
};

class OscHandler {
public:
    using TitleCallback = std::function<void(std::string_view title)>;
    using CwdCallback = std::function<void(std::string_view cwd)>;
    using ShellMarkerCallback = std::function<void(const ShellMarker& marker)>;
    using ClipboardWriteCallback = std::function<void(std::string_view text)>;
    using ClipboardReadCallback = std::function<std::string()>;
    using ResponseCallback = std::function<void(std::string_view)>;

    OscHandler();

    void set_title_callback(TitleCallback cb) { title_cb_ = std::move(cb); }
    void set_cwd_callback(CwdCallback cb) { cwd_cb_ = std::move(cb); }
    void set_shell_marker_callback(ShellMarkerCallback cb) { shell_marker_cb_ = std::move(cb); }
    void set_clipboard_write_callback(ClipboardWriteCallback cb) { clip_write_cb_ = std::move(cb); }
    void set_clipboard_read_callback(ClipboardReadCallback cb) { clip_read_cb_ = std::move(cb); }
    void set_response_callback(ResponseCallback cb) { response_cb_ = std::move(cb); }

    void handle_osc(int command, std::string_view payload);

    [[nodiscard]] std::string_view title() const noexcept { return title_; }
    [[nodiscard]] std::string_view cwd() const noexcept { return cwd_; }
    [[nodiscard]] uint16_t current_hyperlink_id() const noexcept { return current_hyperlink_id_; }

    [[nodiscard]] std::string_view get_hyperlink_url(uint16_t id) const noexcept;
    uint16_t register_hyperlink(std::string_view url);

private:
    std::string title_;
    std::string cwd_;
    std::vector<std::string> hyperlink_pool_;
    uint16_t current_hyperlink_id_{0};

    TitleCallback title_cb_;
    CwdCallback cwd_cb_;
    ShellMarkerCallback shell_marker_cb_;
    ClipboardWriteCallback clip_write_cb_;
    ClipboardReadCallback clip_read_cb_;
    ResponseCallback response_cb_;

    void handle_clipboard(std::string_view payload);
    void handle_shell_integration(std::string_view payload);
    void handle_hyperlink(std::string_view payload);
};

// Base64 helper utilities for OSC 52
std::string base64_encode(std::string_view input);
std::string base64_decode(std::string_view input);

} // namespace bropty
