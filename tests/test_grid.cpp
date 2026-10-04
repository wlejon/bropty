#include "bropty/grid.h"
#include "test_common.h"
#include <cassert>
#include <iostream>

int main() {
    init_test();
    std::cout << "[test_grid] Starting...\n";

    // 1. Creation & initial state
    bropty::Grid grid(80, 24);
    assert(grid.cols() == 80);
    assert(grid.rows() == 24);
    assert(grid.cursor().row == 0);
    assert(grid.cursor().col == 0);
    assert(grid.has_damage());

    grid.clear_damage();
    assert(!grid.has_damage());

    // 2. Writing characters and damage tracking
    grid.write_char('H', 1);
    grid.write_char('i', 1);
    assert(grid.cursor().row == 0);
    assert(grid.cursor().col == 2);
    assert(grid.get_cell(0, 0).codepoint == 'H');
    assert(grid.get_cell(0, 1).codepoint == 'i');
    assert(grid.is_line_dirty(0));
    assert(!grid.is_line_dirty(1));

    // 3. Auto-wrap
    bropty::Grid narrow(5, 3);
    narrow.write_char('A', 1);
    narrow.write_char('B', 1);
    narrow.write_char('C', 1);
    narrow.write_char('D', 1);
    narrow.write_char('E', 1); // At right margin
    assert(narrow.cursor().row == 0);
    assert(narrow.cursor().col == 4);

    narrow.write_char('F', 1); // Should wrap to row 1, col 0
    assert(narrow.is_line_wrapped(0));
    assert(narrow.cursor().row == 1);
    assert(narrow.cursor().col == 1);
    assert(narrow.get_cell(1, 0).codepoint == 'F');

    // 4. Wide character handling
    bropty::Grid wide_grid(10, 3);
    wide_grid.set_cursor_pos(0, 8); // col 8 of 10
    wide_grid.write_char(0x4E2D, 2); // '中' occupies col 8 and 9
    assert(wide_grid.get_cell(0, 8).codepoint == 0x4E2D);
    assert(wide_grid.get_cell(0, 8).has_flag(bropty::CellFlag_WideLead));
    assert(wide_grid.get_cell(0, 9).has_flag(bropty::CellFlag_WideTrail));

    // Wide char at edge wraps to next line if it doesn't fit
    wide_grid.set_cursor_pos(1, 9); // only 1 column left
    wide_grid.write_char(0x6587, 2); // '文' doesn't fit in 1 col -> wraps to row 2 col 0
    assert(wide_grid.is_line_wrapped(1));
    assert(wide_grid.cursor().row == 2);
    assert(wide_grid.get_cell(2, 0).codepoint == 0x6587);

    // 5. Erase in line
    bropty::Grid erase_grid(10, 3);
    for (int i = 0; i < 10; ++i) erase_grid.write_char('X', 1);
    erase_grid.set_cursor_pos(0, 5);
    erase_grid.erase_in_line(0); // cursor to end (cols 5..9 cleared)
    assert(erase_grid.get_cell(0, 4).codepoint == 'X');
    assert(erase_grid.get_cell(0, 5).codepoint == ' ');
    assert(erase_grid.get_cell(0, 9).codepoint == ' ');

    // 6. Scrolling
    bropty::Grid scroll_grid(10, 3);
    scroll_grid.set_cursor_pos(0, 0);
    scroll_grid.write_char('1', 1);
    scroll_grid.set_cursor_pos(1, 0);
    scroll_grid.write_char('2', 1);
    scroll_grid.set_cursor_pos(2, 0);
    scroll_grid.write_char('3', 1);

    int evict_count = 0;
    scroll_grid.scroll_up(1, [&](std::vector<bropty::Cell> cells, bool wrapped) {
        evict_count++;
        assert(cells[0].codepoint == '1');
        assert(!wrapped);
    });
    assert(evict_count == 1);
    assert(scroll_grid.get_cell(0, 0).codepoint == '2');
    assert(scroll_grid.get_cell(1, 0).codepoint == '3');
    assert(scroll_grid.get_cell(2, 0).codepoint == ' ');

    // 7. Resize
    grid.resize(40, 12);
    assert(grid.cols() == 40);
    assert(grid.rows() == 12);
    assert(grid.get_cell(0, 0).codepoint == 'H');

    std::cout << "[test_grid] PASSED\n";
    return 0;
}
