#pragma once

#include "bropty/grid.h"
#include "bropty/scrollback.h"
#include "bropty/vt_parser.h"
#include <functional>
#include <string_view>

namespace bropty {

enum class MouseMode : uint8_t {
    None = 0,
    Click = 1,      // 1000: X10 / Normal tracking
    Button = 2,     // 1002: Motion while button pressed
    Any = 3         // 1003: All motion
};

struct TerminalModes {
    bool application_cursor_keys{false};
    bool auto_wrap{true};
    bool cursor_blinking{true};
    bool cursor_visible{true};
    bool bracketed_paste{false};
    bool alt_screen{false};
    bool sgr_mouse{false};
    bool focus_reporting{false};
    MouseMode mouse_mode{MouseMode::None};
};

class CsiHandler {
public:
    using ResponseCallback = std::function<void(std::string_view)>;
    using AltScreenCallback = std::function<void(bool enable)>;
    using ClearScrollbackCallback = std::function<void()>;

    explicit CsiHandler(Grid& primary_grid,
                        Grid& alt_grid,
                        ScrollbackBuffer& scrollback,
                        TerminalModes& modes);

    void set_response_callback(ResponseCallback cb) { response_cb_ = std::move(cb); }
    void set_alt_screen_callback(AltScreenCallback cb) { alt_screen_cb_ = std::move(cb); }
    void set_clear_scrollback_callback(ClearScrollbackCallback cb) { clear_scrollback_cb_ = std::move(cb); }

    void handle_csi(char final_char,
                    const std::vector<CsiParamValue>& params,
                    const std::vector<char>& intermediates,
                    char prefix);

    [[nodiscard]] Grid& active_grid() noexcept;
    [[nodiscard]] const Grid& active_grid() const noexcept;

private:
    Grid& primary_grid_;
    Grid& alt_grid_;
    ScrollbackBuffer& scrollback_;
    TerminalModes& modes_;

    ResponseCallback response_cb_;
    AltScreenCallback alt_screen_cb_;
    ClearScrollbackCallback clear_scrollback_cb_;

    void handle_sgr(const std::vector<CsiParamValue>& params);
    void handle_private_mode(int mode, bool enable);
    void handle_soft_reset();

    static int get_param(const std::vector<CsiParamValue>& params, size_t index, int default_val);
};

} // namespace bropty
