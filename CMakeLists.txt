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
    src/cell.cpp
    src/ring_buffer.cpp
    src/scrollback.cpp
    src/grid.cpp
    src/vt_parser.cpp
    src/csi_handler.cpp
    src/osc_handler.cpp
    src/key_encoder.cpp
    src/terminal.cpp
    src/pty_common.cpp
)

if(WIN32)
    list(APPEND BROPTY_SOURCES src/pty_win.cpp)
else()
    list(APPEND BROPTY_SOURCES src/pty_posix.cpp)
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

if(NOT WIN32)
    find_library(UTIL_LIB util)
    if(UTIL_LIB)
        target_link_libraries(bropty PRIVATE ${UTIL_LIB})
    endif()
endif()

option(BROPTY_BUILD_TESTS "Build bropty test suite" ON)
if(BROPTY_BUILD_TESTS AND (PROJECT_IS_TOP_LEVEL OR BRO_BUILD_TESTS))
    include(CTest)
    add_subdirectory(tests)
endif()
