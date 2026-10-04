#pragma once

#include "bropty/csi_handler.h"
#include "bropty/grid.h"
#include "bropty/key_encoder.h"
#include "bropty/osc_handler.h"
#include "bropty/pty.h"
#include "bropty/scrollback.h"
#include "bropty/vt_parser.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace bropty {

// Fast, zero-dependency East Asian Width / emoji width detector
int codepoint_width(uint32_t codepoint) noexcept;

class Terminal : public IVtHandler {
public:
    using BellCallback = std::function<void()>;
    using TitleCallback = std::function<void(std::string_view)>;
    using CwdCallback = std::function<void(std::string_view)>;
    using ShellMarkerCallback = std::function<void(const ShellMarker&)>;
    using OutputCallback = std::function<void(std::string_view)>;

    explicit Terminal(int cols = 80, int rows = 24, size_t max_scrollback_lines = 10000);
    ~Terminal() override = default;

    // Attach an external or spawned PTY process
    void attach_pty(std::shared_ptr<IPtyProcess> pty);
    [[nodiscard]] std::shared_ptr<IPtyProcess> pty() const noexcept { return pty_; }

    // Feeds bytes directly into the terminal parser
    void process_input(std::string_view data);
    void process_input(const uint8_t* data, size_t length);

    // Reads pending bytes from the attached PTY and processes them
    // Returns number of bytes processed
    size_t update();

    // Sends input from keyboard/mouse to the terminal (and attached PTY)
    void send_key(Key key,
                  uint32_t codepoint = 0,
                  uint8_t modifiers = Mod_None,
                  KeyEventType event_type = KeyEventType::Press);

    void send_text(std::string_view text);

    void send_mouse(MouseButton button,
                    MouseAction action,
                    uint8_t modifiers,
                    int col,
                    int row);

    // Resizes terminal grid, reflows scrollback, and resizes attached PTY
    void resize(int cols, int rows);

    // Reset terminal state
    void reset();

    // Accessors
    [[nodiscard]] Grid& active_grid() noexcept;
    [[nodiscard]] const Grid& active_grid() const noexcept;
    [[nodiscard]] Grid& primary_grid() noexcept { return primary_grid_; }
    [[nodiscard]] const Grid& primary_grid() const noexcept { return primary_grid_; }
    [[nodiscard]] Grid& alt_grid() noexcept { return alt_grid_; }
    [[nodiscard]] const Grid& alt_grid() const noexcept { return alt_grid_; }
    [[nodiscard]] ScrollbackBuffer& scrollback() noexcept { return scrollback_; }
    [[nodiscard]] const ScrollbackBuffer& scrollback() const noexcept { return scrollback_; }
    [[nodiscard]] const Cursor& cursor() const noexcept { return active_grid().cursor(); }
    [[nodiscard]] const TerminalModes& modes() const noexcept { return modes_; }
    [[nodiscard]] TerminalModes& modes() noexcept { return modes_; }

    [[nodiscard]] std::string_view title() const noexcept { return osc_handler_.title(); }
    [[nodiscard]] std::string_view cwd() const noexcept { return osc_handler_.cwd(); }
    [[nodiscard]] std::string_view get_hyperlink(uint16_t id) const noexcept { return osc_handler_.get_hyperlink_url(id); }

    // Callbacks
    void set_bell_callback(BellCallback cb) { bell_cb_ = std::move(cb); }
    void set_title_callback(TitleCallback cb);
    void set_cwd_callback(CwdCallback cb);
    void set_shell_marker_callback(ShellMarkerCallback cb);
    void set_output_callback(OutputCallback cb) { output_cb_ = std::move(cb); }

    // IVtHandler implementation
    void on_print(uint32_t codepoint) override;
    void on_execute(uint8_t byte) override;
    void on_csi(char final_char,
                const std::vector<CsiParamValue>& params,
                const std::vector<char>& intermediates,
                char prefix) override;
    void on_osc(int command, std::string_view payload) override;
    void on_esc(char final_char, const std::vector<char>& intermediates) override;

private:
    int cols_{80};
    int rows_{24};

    Grid primary_grid_;
    Grid alt_grid_;
    ScrollbackBuffer scrollback_;
    TerminalModes modes_{};

    VtParser parser_;
    CsiHandler csi_handler_;
    OscHandler osc_handler_;

    std::shared_ptr<IPtyProcess> pty_;

    BellCallback bell_cb_;
    OutputCallback output_cb_;

    void send_to_pty(std::string_view data);
};

} // namespace bropty
