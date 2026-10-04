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
bropty_test(test_key_encoder)
bropty_test(test_pty)

# Unicode tables + UAX #29 segmentation against the official conformance file.
bropty_test(test_unicode ARGS ${CMAKE_CURRENT_SOURCE_DIR}/data/GraphemeBreakTest.txt)

# Differential oracle: libvterm (MIT, vendored under third_party/, built only
# into this test) runs the same byte streams; screens must match cell for cell.
add_subdirectory(third_party/libvterm)
bropty_test(test_vterm_diff vterm_diff_gen.cpp)
target_link_libraries(test_vterm_diff PRIVATE bropty_test_vterm)

# Real-program probes kept for the PTY / keyboard work (labelled, not part of the core suite).
foreach(probe_name IN ITEMS probe_keys probe_conpty)
    add_executable(${probe_name} "${probe_name}.cpp")
    target_link_libraries(${probe_name} PRIVATE bropty)
    target_include_directories(${probe_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME ${probe_name} COMMAND ${probe_name} "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(${probe_name} PROPERTIES LABELS probe TIMEOUT 180)
endforeach()
