cmake_minimum_required(VERSION 3.24)

project(bropty VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
    add_compile_options(
        $<$<COMPILE_LANGUAGE:CXX>:/MP>
        $<$<COMPILE_LANGUAGE:CXX>:/permissive->
        $<$<COMPILE_LANGUAGE:CXX>:/utf-8>
    )
    if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endif()

set(BROPTY_SOURCES
    src/version.cpp
    src/color.cpp
    src/style.cpp
    src/cell.cpp
    src/unicode.cpp
    src/grid.cpp
    src/logical_line.cpp
    src/scrollback.cpp
    src/parser.cpp
    src/terminal.cpp
    src/terminal_print.cpp
    src/terminal_csi.cpp
    src/terminal_modes.cpp
    src/terminal_osc.cpp
    src/terminal_reflow.cpp
    src/buffer_lines.cpp
    src/selection.cpp
    src/selection_text.cpp
    src/search.cpp
    src/links.cpp
    src/view.cpp
    src/frame.cpp
    src/ring_buffer.cpp
    src/key_encoder.cpp
    src/key_encoder_kitty.cpp
    src/key_encoder_legacy.cpp
    src/mouse_encoder.cpp
    src/session.cpp
    src/pty_common.cpp
    src/pty_base.cpp
)

if(WIN32)
    list(APPEND BROPTY_SOURCES src/pty_win.cpp)
else()
    list(APPEND BROPTY_SOURCES src/pty_posix.cpp src/pty_posix_child.cpp)
endif()

add_library(bropty STATIC ${BROPTY_SOURCES})
add_library(bropty::bropty ALIAS bropty)

target_include_directories(bropty
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(MSVC)
    target_compile_options(bropty PRIVATE /W4)
else()
    target_compile_options(bropty PRIVATE -Wall -Wextra)
endif()

find_package(Threads REQUIRED)
target_link_libraries(bropty PUBLIC Threads::Threads)

# Regenerates src/unicode_tables.inc from a downloaded UCD (see tools/gen_unicode.cpp).
option(BROPTY_BUILD_TOOLS "Build the bropty maintenance tools" OFF)
if(BROPTY_BUILD_TOOLS)
    add_executable(bropty_gen_unicode tools/gen_unicode.cpp)
endif()

option(BROPTY_BUILD_TESTS "Build bropty test suite" ON)
if(BROPTY_BUILD_TESTS AND (PROJECT_IS_TOP_LEVEL OR BRO_BUILD_TESTS))
    include(CTest)
    add_subdirectory(tests)
endif()
