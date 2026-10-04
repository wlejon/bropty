#include "bropty/vt_parser.h"
#include "test_common.h"
#include <cassert>
#include <iostream>
#include <vector>

class MockVtHandler : public bropty::IVtHandler {
public:
    std::vector<uint32_t> prints;
    std::vector<uint8_t> executes;
    struct CsiEvent {
        char final_char;
        std::vector<bropty::CsiParamValue> params;
        std::vector<char> intermediates;
        char prefix;
    };
    std::vector<CsiEvent> csi_events;
    struct OscEvent {
        int command;
        std::string payload;
    };
    std::vector<OscEvent> osc_events;
    struct EscEvent {
        char final_char;
        std::vector<char> intermediates;
    };
    std::vector<EscEvent> esc_events;

    void on_print(uint32_t cp) override { prints.push_back(cp); }
    void on_execute(uint8_t b) override { executes.push_back(b); }
    void on_csi(char fc,
                const std::vector<bropty::CsiParamValue>& p,
                const std::vector<char>& im,
                char pr) override {
        csi_events.push_back({fc, p, im, pr});
    }
    void on_osc(int cmd, std::string_view payload) override {
        osc_events.push_back({cmd, std::string(payload)});
    }
    void on_esc(char fc, const std::vector<char>& im) override {
        esc_events.push_back({fc, im});
    }
};

int main() {
    init_test();
    std::cout << "[test_vt_parser] Starting...\n";

    MockVtHandler handler;
    bropty::VtParser parser(&handler);

    // 1. Ground state & control characters
    parser.feed("Hello\r\nWorld");
    assert(handler.prints.size() == 10);
    assert(handler.prints[0] == 'H');
    assert(handler.prints[4] == 'o');
    assert(handler.prints[5] == 'W');
    assert(handler.executes.size() == 2);
    assert(handler.executes[0] == '\r');
    assert(handler.executes[1] == '\n');

    // 2. UTF-8 multi-byte decoding
    handler.prints.clear();
    // UTF-8 for "中" (E4 B8 AD) and emoji "🚀" (F0 9F 9A 80)
    parser.feed("\xE4\xB8\xAD\xF0\x9F\x9A\x80");
    assert(handler.prints.size() == 2);
    assert(handler.prints[0] == 0x4E2D);   // '中'
    assert(handler.prints[1] == 0x1F680);  // '🚀'

    // 3. Simple ESC sequences
    parser.feed("\x1b" "7\x1b" "8\x1b" "c");
    assert(handler.esc_events.size() == 3);
    assert(handler.esc_events[0].final_char == '7');
    assert(handler.esc_events[1].final_char == '8');
    assert(handler.esc_events[2].final_char == 'c');

    // 4. CSI sequences
    // CSI 1;24r
    parser.feed("\x1b[1;24r");
    assert(handler.csi_events.size() == 1);
    assert(handler.csi_events[0].final_char == 'r');
    assert(handler.csi_events[0].params.size() == 2);
    assert(handler.csi_events[0].params[0].value == 1);
    assert(handler.csi_events[0].params[1].value == 24);
    assert(handler.csi_events[0].prefix == 0);

    // CSI with private prefix: CSI ? 1049 h
    parser.feed("\x1b[?1049h");
    assert(handler.csi_events.size() == 2);
    assert(handler.csi_events[1].final_char == 'h');
    assert(handler.csi_events[1].prefix == '?');
    assert(handler.csi_events[1].params[0].value == 1049);

    // CSI with subparameters: SGR 38:2::255:128:0 m
    parser.feed("\x1b[38:2::255:128:0m");
    assert(handler.csi_events.size() == 3);
    assert(handler.csi_events[2].final_char == 'm');
    assert(handler.csi_events[2].params.size() == 1);
    assert(handler.csi_events[2].params[0].value == 38);
    const auto& subs = handler.csi_events[2].params[0].subparams;
    assert(subs.size() >= 4);
    assert(subs[0] == 2);

    // 5. OSC with BEL and ST
    parser.feed("\x1b]0;My Window Title\x07");
    assert(handler.osc_events.size() == 1);
    assert(handler.osc_events[0].command == 0);
    assert(handler.osc_events[0].payload == "My Window Title");

    parser.feed("\x1b]7;file://localhost/home/user\x1b\\");
    assert(handler.osc_events.size() == 2);
    assert(handler.osc_events[1].command == 7);
    assert(handler.osc_events[1].payload == "file://localhost/home/user");

    std::cout << "[test_vt_parser] PASSED\n";
    return 0;
}
