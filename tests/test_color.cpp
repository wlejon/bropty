#include "bropty/color.h"
#include "test_common.h"
#include <cassert>
#include <iostream>

int main() {
    init_test();
    std::cout << "[test_color] Starting...\n";

    // Default color
    bropty::Color def;
    assert(def.is_default());
    assert(!def.is_indexed());
    assert(!def.is_rgb());
    assert(def.resolve(true) == bropty::Color::default_fg_rgb());
    assert(def.resolve(false) == bropty::Color::default_bg_rgb());

    // Indexed color (ANSI standard)
    bropty::Color c_red = bropty::Color::from_index(1);
    assert(c_red.is_indexed());
    assert(c_red.index() == 1);
    bropty::Rgb rgb_red = c_red.resolve();
    assert(rgb_red == (bropty::Rgb{205, 49, 49}));

    // Indexed color (256 color cube)
    // 16 is {0, 0, 0}, 21 is {0, 0, 255}
    bropty::Color c_21 = bropty::Color::from_index(21);
    assert(c_21.resolve() == (bropty::Rgb{0, 0, 255}));

    // RGB TrueColor
    bropty::Color truecolor = bropty::Color::from_rgb(123, 45, 67);
    assert(truecolor.is_rgb());
    assert(truecolor.rgb().r == 123);
    assert(truecolor.rgb().g == 45);
    assert(truecolor.rgb().b == 67);
    assert(truecolor.resolve() == (bropty::Rgb{123, 45, 67}));
    assert(truecolor.rgb().to_u32() == ((123 << 16) | (45 << 8) | 67));

    // Equality
    assert(def == bropty::Color::default_color());
    assert(c_red == bropty::Color::from_index(1));
    assert(c_red != c_21);
    assert(truecolor == bropty::Color::from_rgb(123, 45, 67));
    assert(truecolor != c_red);

    std::cout << "[test_color] PASSED\n";
    return 0;
}
