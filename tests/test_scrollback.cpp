#include "bropty/scrollback.h"
#include "test_common.h"
#include <cassert>
#include <iostream>

int main() {
    init_test();
    std::cout << "[test_scrollback] Starting...\n";

    // 1. Basic push and circular eviction
    bropty::ScrollbackBuffer sb(3);
    assert(sb.max_lines() == 3);
    assert(sb.size() == 0);

    auto make_line = [](const std::string& str) {
        std::vector<bropty::Cell> cells;
        for (char c : str) cells.emplace_back(static_cast<uint32_t>(c));
        return cells;
    };

    sb.push_line(make_line("Line 1"), false);
    sb.push_line(make_line("Line 2"), false);
    sb.push_line(make_line("Line 3"), false);
    assert(sb.size() == 3);
    assert(sb.get_cell(0, 5).codepoint == '1');
    assert(sb.get_cell(2, 5).codepoint == '3');

    // Push 4th line -> 1st line evicted
    sb.push_line(make_line("Line 4"), false);
    assert(sb.size() == 3);
    assert(sb.get_cell(0, 5).codepoint == '2');
    assert(sb.get_cell(1, 5).codepoint == '3');
    assert(sb.get_cell(2, 5).codepoint == '4');

    // 2. Reflow on resize
    // Create a 2-part wrapped line: "0123456789" (wrapped) + "ABCDE" (unwrapped)
    bropty::ScrollbackBuffer reflow_sb(100);
    reflow_sb.push_line(make_line("0123456789"), true);
    reflow_sb.push_line(make_line("ABCDE"), false);
    assert(reflow_sb.size() == 2);

    // Reflow to narrow width 5: should break into 3 lines of 5 chars each
    reflow_sb.reflow(5);
    assert(reflow_sb.size() == 3);

    // Line 0: "01234", wrapped = true
    assert(reflow_sb.get_line(0).wrapped == true);
    assert(reflow_sb.get_line(0).cells.size() == 5);
    assert(reflow_sb.get_cell(0, 0).codepoint == '0');
    assert(reflow_sb.get_cell(0, 4).codepoint == '4');

    // Line 1: "56789", wrapped = true
    assert(reflow_sb.get_line(1).wrapped == true);
    assert(reflow_sb.get_line(1).cells.size() == 5);
    assert(reflow_sb.get_cell(1, 0).codepoint == '5');
    assert(reflow_sb.get_cell(1, 4).codepoint == '9');

    // Line 2: "ABCDE", wrapped = false
    assert(reflow_sb.get_line(2).wrapped == false);
    assert(reflow_sb.get_line(2).cells.size() == 5);
    assert(reflow_sb.get_cell(2, 0).codepoint == 'A');
    assert(reflow_sb.get_cell(2, 4).codepoint == 'E');

    // Now reflow wider to 20: should collapse back to 1 line of 15 chars
    reflow_sb.reflow(20);
    assert(reflow_sb.size() == 1);
    assert(reflow_sb.get_line(0).wrapped == false);
    assert(reflow_sb.get_line(0).cells.size() == 15);
    assert(reflow_sb.get_cell(0, 0).codepoint == '0');
    assert(reflow_sb.get_cell(0, 9).codepoint == '9');
    assert(reflow_sb.get_cell(0, 10).codepoint == 'A');
    assert(reflow_sb.get_cell(0, 14).codepoint == 'E');

    std::cout << "[test_scrollback] PASSED\n";
    return 0;
}
