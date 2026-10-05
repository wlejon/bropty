enable_testing()

# Each test is a plain executable whose exit status is non-zero on any failed
# check (see check.h); none of them rely on assert().
#   bropty_test(<name> [extra sources...] [ARGS <test arguments...>])
function(bropty_test name)
    cmake_parse_arguments(PARSE_ARGV 1 BT "" "" "ARGS")
    add_executable(${name} ${name}.cpp ${BT_UNPARSED_ARGUMENTS})
    target_link_libraries(${name} PRIVATE bropty)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/src)
    add_test(NAME ${name} COMMAND ${name} ${BT_ARGS})
    set_tests_properties(${name} PROPERTIES TIMEOUT 300)
endfunction()

bropty_test(test_parser)
bropty_test(test_screen)
bropty_test(test_unicode_print)
bropty_test(test_replies)
bropty_test(test_reflow)
bropty_test(test_scrollback)
bropty_test(test_ring_buffer)

# Input encoding: kitty keyboard protocol, xterm legacy keys + modifyOtherKeys,
# mouse, paste / focus, and the Session wiring that picks them by mode.
bropty_test(test_keys_kitty)
bropty_test(test_keys_legacy)
bropty_test(test_mouse)
bropty_test(test_paste_focus)
bropty_test(test_input_session)

# PTY layer. The process tests run real shells plus pty_child, a helper whose
# behaviour (arguments, environment, raw keys, ignored signals, floods) does
# not depend on what a machine has installed.
bropty_test(test_pty_unit)
if(WIN32)
    target_link_libraries(test_pty_unit PRIVATE shell32)
endif()
add_executable(pty_child pty_child.cpp)
bropty_test(test_pty ARGS $<TARGET_FILE:pty_child>)
bropty_test(test_pty_teardown ARGS $<TARGET_FILE:pty_child>)
add_dependencies(test_pty pty_child)
add_dependencies(test_pty_teardown pty_child)

# Unicode tables + UAX #29 segmentation against the official conformance file.
bropty_test(test_unicode ARGS ${CMAKE_CURRENT_SOURCE_DIR}/data/GraphemeBreakTest.txt)

# Differential oracle: libvterm (MIT, vendored under third_party/, built only
# into this test) runs the same byte streams; screens must match cell for cell.
add_subdirectory(third_party/libvterm)
bropty_test(test_vterm_diff vterm_diff_gen.cpp)
target_link_libraries(test_vterm_diff PRIVATE bropty_test_vterm)

# Parse/emulate throughput (prints MB/s per corpus; checks only correctness).
bropty_test(bench_throughput)
