#include "bropty/cell.h"
#include "test_common.h"
#include <cassert>
#include <iostream>

int main() {
    init_test();
    std::cout << "[test_cell] Starting...\n";

    bropty::Cell c1;
    assert(c1.codepoint == ' ');
    assert(c1.fg.is_default());
    assert(c1.bg.is_default());
    assert(c1.flags == bropty::CellFlag_None);
    assert(c1.hyperlink_id == 0);
    assert(c1.width == 1);
    assert(c1.is_empty());

    // Attribute flags
    c1.set_flag(bropty::CellFlag_Bold, true);
    c1.set_flag(bropty::CellFlag_Italic, true);
    assert(c1.has_flag(bropty::CellFlag_Bold));
    assert(c1.has_flag(bropty::CellFlag_Italic));
    assert(!c1.has_flag(bropty::CellFlag_Dim));
    assert(!c1.is_empty());

    // Underline styles
    assert(!c1.is_underlined());
    c1.set_flag(bropty::CellFlag_UnderlineCurly, true);
    assert(c1.is_underlined());
    assert(c1.has_flag(bropty::CellFlag_UnderlineCurly));

    c1.set_flag(bropty::CellFlag_UnderlineCurly, false);
    assert(!c1.is_underlined());

    // Reset
    c1.reset();
    assert(c1.is_empty());

    // Wide characters
    bropty::Cell wide_lead(0x4E2D); // Chinese character '中'
    wide_lead.width = 2;
    wide_lead.set_flag(bropty::CellFlag_WideLead, true);
    assert(wide_lead.has_flag(bropty::CellFlag_WideLead));
    assert(wide_lead.width == 2);

    bropty::Cell wide_trail(' ');
    wide_trail.width = 0;
    wide_trail.set_flag(bropty::CellFlag_WideTrail, true);
    assert(wide_trail.has_flag(bropty::CellFlag_WideTrail));
    assert(wide_trail.width == 0);

    // Equality
    bropty::Cell c2 = wide_lead;
    assert(c2 == wide_lead);
    c2.codepoint = 'A';
    assert(c2 != wide_lead);

    std::cout << "[test_cell] PASSED\n";
    return 0;
}
