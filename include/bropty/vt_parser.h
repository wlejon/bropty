#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bropty {

struct CsiParamValue {
    int value{-1}; // -1 means omitted / default
    std::vector<int> subparams;
};

class IVtHandler {
public:
    virtual ~IVtHandler() = default;

    virtual void on_print(uint32_t codepoint) = 0;
    virtual void on_execute(uint8_t byte) = 0;
    virtual void on_csi(char final_char,
                        const std::vector<CsiParamValue>& params,
                        const std::vector<char>& intermediates,
                        char prefix) = 0;
    virtual void on_osc(int command, std::string_view payload) = 0;
    virtual void on_esc(char final_char, const std::vector<char>& intermediates) = 0;
};

enum class VtState : uint8_t {
    Ground = 0,
    Escape,
    EscapeIntermediate,
    CsiEntry,
    CsiParam,
    CsiIntermediate,
    CsiIgnore,
    DcsEntry,
    DcsParam,
    DcsIntermediate,
    DcsPassthrough,
    DcsIgnore,
    OscString,
    SosPmApcString
};

class VtParser {
public:
    explicit VtParser(IVtHandler* handler = nullptr);

    void set_handler(IVtHandler* handler) noexcept { handler_ = handler; }
    [[nodiscard]] IVtHandler* handler() const noexcept { return handler_; }

    // Feeds incoming bytes into the parser
    void feed(std::string_view data);
    void feed(const uint8_t* data, size_t length);
    void feed_byte(uint8_t byte);

    // Reset parser to ground state
    void reset();

    [[nodiscard]] VtState state() const noexcept { return state_; }

private:
    IVtHandler* handler_{nullptr};
    VtState state_{VtState::Ground};

    // CSI parsing state
    char csi_prefix_{0}; // '?', '>', '<', '='
    std::vector<CsiParamValue> csi_params_;
    std::vector<char> csi_intermediates_;
    int current_param_{-1};
    bool in_subparam_{false};

    // OSC parsing state
    std::string osc_string_;

    // ESC parsing state
    std::vector<char> esc_intermediates_;

    // UTF-8 accumulator state
    uint32_t utf8_codepoint_{0};
    uint8_t utf8_expected_bytes_{0};
    uint8_t utf8_received_bytes_{0};

    void process_byte(uint8_t byte);
    void handle_utf8(uint8_t byte);
    void transition_to(VtState new_state);

    void csi_flush_param();
};

} // namespace bropty
