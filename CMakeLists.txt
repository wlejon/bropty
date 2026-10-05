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

# ---- brosearch -----------------------------------------------------------------
# Regex scrollback search (RegexMatcher, search_regex.h) runs on brosearch's
# linear-time engine. Resolved like the other siblings: an existing `brosearch`
# target (a superbuild already added it) wins; otherwise the standalone repo
# beside this one (../brosearch), overridable with -DBROSEARCH_DIR=<path>.
if(NOT TARGET brosearch)
    get_filename_component(_brosearch_default "${CMAKE_CURRENT_SOURCE_DIR}/../brosearch" ABSOLUTE)
    set(BROSEARCH_DIR "${_brosearch_default}" CACHE PATH "Path to the brosearch repo")
    if(NOT EXISTS "${BROSEARCH_DIR}/CMakeLists.txt")
        message(FATAL_ERROR "bropty needs brosearch: clone it beside bropty (../brosearch) or pass -DBROSEARCH_DIR=<path>")
    endif()
    message(STATUS "bropty: brosearch from ${BROSEARCH_DIR}")
    set(BROSEARCH_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(BROSEARCH_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    add_subdirectory("${BROSEARCH_DIR}" "${CMAKE_BINARY_DIR}/brosearch" EXCLUDE_FROM_ALL)
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
    src/buffer_lines_term.cpp
    src/selection.cpp
    src/selection_text.cpp
    src/search.cpp
    src/search_regex.cpp
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
    # inline images
    src/inflate.cpp
    src/base64.cpp
    src/sixel.cpp
    src/kitty_command.cpp
    src/placeholder.cpp
    src/graphics_state.cpp
    src/graphics_anchor.cpp
    src/graphics_load.cpp
    src/graphics_kitty.cpp
    src/graphics_kitty_anim.cpp
    src/graphics_sixel.cpp
    src/graphics_iterm.cpp
    src/graphics_cells.cpp
    src/frame_images.cpp
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
target_link_libraries(bropty PRIVATE brosearch)
# shm_open (kitty t=s) lives in librt on older glibc.
if(UNIX AND NOT APPLE)
    include(CheckLibraryExists)
    check_library_exists(rt shm_open "" BROPTY_HAVE_LIBRT)
    if(BROPTY_HAVE_LIBRT)
        target_link_libraries(bropty PUBLIC rt)
    endif()
endif()

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
