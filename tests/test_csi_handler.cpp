#include "bropty/csi_handler.h"
#include "test_common.h"
#include <cassert>
#include <iostream>
#include <string>

int main() {
    init_test();
    std::cout << "[test_csi_handler] Starting...\n";

    bropty::Grid primary(80, 24);
    bropty::Grid alt(80, 24);
    bropty::ScrollbackBuffer sb(100);
    bropty::TerminalModes modes;

    bropty::CsiHandler handler(primary, alt, sb, modes);

    std::string response_received;
    handler.set_response_callback([&](std::string_view resp) {
        response_received = std::string(resp);
    });

    // 1. Cursor positioning: CUP 10;20 -> row 9, col 19
    std::vector<bropty::CsiParamValue> params;
    params.push_back({10, {}});
    params.push_back({20, {}});
    handler.handle_csi('H', params, {}, 0);
    assert(primary.cursor().row == 9);
    assert(primary.cursor().col == 19);

    // 2. Cursor relative movement: CUD 3, CUB 5
    params.clear();
    params.push_back({3, {}});
    handler.handle_csi('B', params, {}, 0); // Down 3 -> row 12
    assert(primary.cursor().row == 12);

    params.clear();
    params.push_back({5, {}});
    handler.handle_csi('D', params, {}, 0); // Back 5 -> col 14
    assert(primary.cursor().col == 14);

    // 3. SGR Styles: Bold, Italic, Curly Underline
    params.clear();
    params.push_back({1, {}}); // Bold
    params.push_back({3, {}}); // Italic
    bropty::CsiParamValue ul_curly{4, {3}}; // 4:3 curly underline
    params.push_back(ul_curly);
    handler.handle_csi('m', params, {}, 0);

    const auto& pen = primary.cursor().pen;
    assert(pen.has_flag(bropty::CellFlag_Bold));
    assert(pen.has_flag(bropty::CellFlag_Italic));
    assert(pen.has_flag(bropty::CellFlag_UnderlineCurly));

    // 4. SGR Colors: Semicolon TrueColor (38;2;250;150;50)
    params.clear();
    params.push_back({38, {}});
    params.push_back({2, {}});
    params.push_back({250, {}});
    params.push_back({150, {}});
    params.push_back({50, {}});
    handler.handle_csi('m', params, {}, 0);

    assert(primary.cursor().pen.fg.is_rgb());
    assert(primary.cursor().pen.fg.rgb().r == 250);
    assert(primary.cursor().pen.fg.rgb().g == 150);
    assert(primary.cursor().pen.fg.rgb().b == 50);

    // 5. SGR Colors: Colon TrueColor (48:2::10:20:30)
    params.clear();
    bropty::CsiParamValue bg_colon{48, {2, -1, 10, 20, 30}};
    params.push_back(bg_colon);
    handler.handle_csi('m', params, {}, 0);

    assert(primary.cursor().pen.bg.is_rgb());
    assert(primary.cursor().pen.bg.rgb().r == 10);
    assert(primary.cursor().pen.bg.rgb().g == 20);
    assert(primary.cursor().pen.bg.rgb().b == 30);

    // 6. Underline color: (58;2;100;200;255)
    params.clear();
    params.push_back({58, {}});
    params.push_back({2, {}});
    params.push_back({100, {}});
    params.push_back({200, {}});
    params.push_back({255, {}});
    handler.handle_csi('m', params, {}, 0);

    assert(primary.cursor().pen.underline_color.is_rgb());
    assert(primary.cursor().pen.underline_color.rgb().r == 100);
    assert(primary.cursor().pen.underline_color.rgb().g == 200);
    assert(primary.cursor().pen.underline_color.rgb().b == 255);

    // 7. SGR Reset
    params.clear();
    params.push_back({0, {}});
    handler.handle_csi('m', params, {}, 0);
    assert(primary.cursor().pen.is_empty());

    // 8. Private modes: 1049 alternate screen buffer
    assert(!modes.alt_screen);
    params.clear();
    params.push_back({1049, {}});
    handler.handle_csi('h', params, {}, '?');
    assert(modes.alt_screen);
    assert(&handler.active_grid() == &alt);

    handler.handle_csi('l', params, {}, '?');
    assert(!modes.alt_screen);
    assert(&handler.active_grid() == &primary);

    // 9. CPR cursor position report: 6n
    params.clear();
    params.push_back({6, {}});
    primary.set_cursor_pos(5, 7); // row 5 -> 6, col 7 -> 8
    handler.handle_csi('n', params, {}, 0);
    assert(response_received == "\x1b[6;8R");

    std::cout << "[test_csi_handler] PASSED\n";
    return 0;
}
