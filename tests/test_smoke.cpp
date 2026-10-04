#include "bropty/version.h"
#include "test_common.h"
#include <cassert>
#include <iostream>

int main() {
    init_test();
    std::cout << "[test_smoke] bropty version: " << bropty::version_string() << "\n";
    assert(!bropty::version_string().empty());
    assert(bropty::version_major() == BROPTY_VERSION_MAJOR);
    assert(bropty::version_minor() == BROPTY_VERSION_MINOR);
    assert(bropty::version_patch() == BROPTY_VERSION_PATCH);
    std::cout << "[test_smoke] PASSED\n";
    return 0;
}
