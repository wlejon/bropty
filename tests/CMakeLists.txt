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
bropty_test(test_xterm_modes)
bropty_test(test_osc_ext)
bropty_test(test_commands)

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
bropty_test(test_pty_lifecycle ARGS $<TARGET_FILE:pty_child>)
add_dependencies(test_pty pty_child)
add_dependencies(test_pty_teardown pty_child)
add_dependencies(test_pty_lifecycle pty_child)
if(WIN32)
    # Every ConPTY is a conhost (and a console on the desktop heap). The three
    # process tests open dozens between them, so under ctest -j they take
    # turns rather than run at once; the rest of the suite still runs alongside.
    set_tests_properties(test_pty test_pty_teardown test_pty_lifecycle PROPERTIES RESOURCE_LOCK conpty)
endif()

# Unicode tables + UAX #29 segmentation against the official conformance file.
bropty_test(test_unicode ARGS ${CMAKE_CURRENT_SOURCE_DIR}/data/GraphemeBreakTest.txt)

# Differential oracle: libvterm (MIT, vendored under third_party/, built only
# into this test) runs the same byte streams; screens must match cell for cell.
add_subdirectory(third_party/libvterm)
bropty_test(test_vterm_diff vterm_diff_gen.cpp)
target_link_libraries(test_vterm_diff PRIVATE bropty_test_vterm)

# Renderer-facing state: selection, search, links, frame snapshots, and the
# randomized / concurrency oracles for them.
bropty_test(test_selection)
bropty_test(test_search)
bropty_test(test_links)
bropty_test(test_row_source)
bropty_test(test_row_numbers)
bropty_test(test_clipboard)
bropty_test(test_frame)
bropty_test(test_anchor_props)
bropty_test(test_frame_stress)
find_package(Threads REQUIRED)
target_link_libraries(test_frame_stress PRIVATE Threads::Threads)

# Inline images (kitty graphics, sixel, iTerm2). Test-only references:
# stb_image / stb_image_write (public domain; PNG / GIF decoding for the
# host decoder, PNG / zlib encoding, and stb's inflater as the reference for
# bropty's) and libsixel's decoder (MIT) as the reference for sixel pixels.
add_subdirectory(third_party/libsixel)
add_library(bropty_image_helpers STATIC image_helpers.cpp)
target_link_libraries(bropty_image_helpers PUBLIC bropty)
target_include_directories(bropty_image_helpers PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(bropty_image_helpers PUBLIC BROPTY_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data")
function(bropty_image_test name)
    bropty_test(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE bropty_image_helpers)
endfunction()
bropty_image_test(test_image_codecs)
bropty_image_test(test_kitty_graphics)
bropty_image_test(test_kitty_anim)
bropty_image_test(test_sixel)
target_link_libraries(test_sixel PRIVATE bropty_test_libsixel)
bropty_image_test(test_image_anchor)
bropty_image_test(test_iterm_images)
# Captures of kitten icat / img2sixel / chafa / imgcat-style output, replayed
# everywhere; POSIX also runs whichever of those programs are installed.
bropty_image_test(test_image_programs)

# Parse/emulate throughput (prints MB/s per corpus; checks only correctness).
bropty_test(bench_throughput)
target_link_libraries(bench_throughput PRIVATE Threads::Threads)

# PTY transport throughput (bropty's PTY, and a bare ConPTY on Windows),
# for several write patterns of the child. Built, not run by ctest:
#   bench_pty <pty_child> [megabytes] [line|4k|64k...]
add_executable(bench_pty bench_pty.cpp)
target_link_libraries(bench_pty PRIVATE bropty Threads::Threads)
target_include_directories(bench_pty PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_dependencies(bench_pty pty_child)
