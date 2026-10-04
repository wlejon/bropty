#include "bropty/osc_handler.h"
#include "test_common.h"
#include <cassert>
#include <iostream>
#include <string>

int main() {
    init_test();
    std::cout << "[test_osc_handler] Starting...\n";

    bropty::OscHandler handler;

    std::string received_title;
    handler.set_title_callback([&](std::string_view title) {
        received_title = std::string(title);
    });

    std::string received_cwd;
    handler.set_cwd_callback([&](std::string_view cwd) {
        received_cwd = std::string(cwd);
    });

    bropty::ShellMarker last_marker{};
    handler.set_shell_marker_callback([&](const bropty::ShellMarker& marker) {
        last_marker = marker;
    });

    std::string clip_written;
    handler.set_clipboard_write_callback([&](std::string_view text) {
        clip_written = std::string(text);
    });

    handler.set_clipboard_read_callback([]() {
        return "copied text";
    });

    std::string response_sent;
    handler.set_response_callback([&](std::string_view resp) {
        response_sent = std::string(resp);
    });

    // 1. Title test: OSC 0 / OSC 2
    handler.handle_osc(0, "Terminal Title 1");
    assert(handler.title() == "Terminal Title 1");
    assert(received_title == "Terminal Title 1");

    handler.handle_osc(2, "Terminal Title 2");
    assert(handler.title() == "Terminal Title 2");
    assert(received_title == "Terminal Title 2");

    // 2. CWD test: OSC 7
    handler.handle_osc(7, "file://localhost/home/user/project");
    assert(handler.cwd() == "file://localhost/home/user/project");
    assert(received_cwd == "file://localhost/home/user/project");

    // 3. Hyperlink test: OSC 8
    handler.handle_osc(8, "id=foo;https://example.com");
    uint16_t link_id = handler.current_hyperlink_id();
    assert(link_id > 0);
    assert(handler.get_hyperlink_url(link_id) == "https://example.com");

    // Close hyperlink
    handler.handle_osc(8, ";");
    assert(handler.current_hyperlink_id() == 0);

    // 4. Clipboard test: OSC 52
    // Base64 encode "Hello World!" -> "SGVsbG8gV29ybGQh"
    assert(bropty::base64_encode("Hello World!") == "SGVsbG8gV29ybGQh");
    assert(bropty::base64_decode("SGVsbG8gV29ybGQh") == "Hello World!");

    // Write to clipboard
    handler.handle_osc(52, "c;SGVsbG8gV29ybGQh");
    assert(clip_written == "Hello World!");

    // Read from clipboard: OSC 52 c;?
    handler.handle_osc(52, "c;?");
    // "copied text" encoded is "Y29waWVkIHRleHQ="
    assert(response_sent == "\x1b]52;c;Y29waWVkIHRleHQ=\x1b\\");

    // 5. Shell integration: OSC 133
    handler.handle_osc(133, "A");
    assert(last_marker.type == bropty::ShellMarkerType::PromptStart);

    handler.handle_osc(133, "B");
    assert(last_marker.type == bropty::ShellMarkerType::CommandStart);

    handler.handle_osc(133, "C");
    assert(last_marker.type == bropty::ShellMarkerType::CommandExecuted);

    handler.handle_osc(133, "D;42");
    assert(last_marker.type == bropty::ShellMarkerType::CommandFinished);
    assert(last_marker.exit_code == 42);

    std::cout << "[test_osc_handler] PASSED\n";
    return 0;
}
