#include "bropty/terminal.h"
#include "test_common.h"
#include <cassert>
#include <iostream>
#include <string>

int main() {
    init_test();
    std::cout << "[test_terminal] Starting...\n";

    bropty::Terminal term(80, 24, 100);

    bool bell_called = false;
    term.set_bell_callback([&]() {
        bell_called = true;
    });

    std::string term_title;
    term.set_title_callback([&](std::string_view t) {
        term_title = std::string(t);
    });

    // 1. Text processing & cursor movement
    term.process_input("Hello, Terminal!\r\nSecond Line\a");
    assert(term.active_grid().get_cell(0, 0).codepoint == 'H');
    assert(term.active_grid().get_cell(0, 15).codepoint == '!');
    assert(term.active_grid().get_cell(1, 0).codepoint == 'S');
    assert(term.cursor().row == 1);
    assert(term.cursor().col == 11);
    assert(bell_called);

    // 2. OSC Title
    term.process_input("\x1b]0;Bro PTY Test\x07");
    assert(term.title() == "Bro PTY Test");
    assert(term_title == "Bro PTY Test");

    // 3. SGR Styling
    term.process_input("\x1b[1;31mRedBold\x1b[0m");
    const auto& c_red = term.active_grid().get_cell(1, 11);
    assert(c_red.codepoint == 'R');
    assert(c_red.has_flag(bropty::CellFlag_Bold));
    assert(c_red.fg.is_indexed());
    assert(c_red.fg.index() == 1); // 1 = Red

    // 4. OSC 8 Hyperlinks
    term.process_input("\x1b]8;;https://bro.dev\x1b\\Link\x1b]8;;\x1b\\");
    const auto& c_link = term.active_grid().get_cell(1, 18);
    assert(c_link.codepoint == 'L');
    assert(c_link.hyperlink_id > 0);
    assert(term.get_hyperlink(c_link.hyperlink_id) == "https://bro.dev");

    // 5. Alternate screen buffer switching (CSI ? 1049 h / l)
    term.process_input("\x1b[?1049hAlt Screen Active\r\n");
    assert(term.modes().alt_screen);
    assert(term.active_grid().get_cell(0, 0).codepoint == 'A');
    // Primary grid should be unaffected
    assert(term.primary_grid().get_cell(0, 0).codepoint == 'H');

    // Switch back to primary
    term.process_input("\x1b[?1049l");
    assert(!term.modes().alt_screen);
    assert(term.active_grid().get_cell(0, 0).codepoint == 'H');

    // 6. Scrollback eviction
    bropty::Terminal small_term(10, 3, 50);
    small_term.process_input("Line 0\r\nLine 1\r\nLine 2\r\nLine 3");
    assert(small_term.scrollback().size() == 1);
    assert(small_term.scrollback().get_cell(0, 5).codepoint == '0');
    assert(small_term.active_grid().get_cell(0, 5).codepoint == '1');
    assert(small_term.active_grid().get_cell(1, 5).codepoint == '2');
    assert(small_term.active_grid().get_cell(2, 5).codepoint == '3');

    std::cout << "[test_terminal] PASSED\n";
    return 0;
}
