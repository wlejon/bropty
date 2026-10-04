enable_testing()

set(TESTS
    test_smoke
    test_color
    test_cell
    test_ring_buffer
    test_grid
    test_scrollback
    test_vt_parser
    test_csi_handler
    test_osc_handler
    test_key_encoder
    test_terminal
    test_pty_win
    test_pty_posix
)

foreach(test_name IN LISTS TESTS)
    add_executable(${test_name} "${test_name}.cpp")
    target_link_libraries(${test_name} PRIVATE bropty)
    target_include_directories(${test_name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../include")
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
