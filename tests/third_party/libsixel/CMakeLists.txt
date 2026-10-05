# libsixel's sixel decoder (MIT, Hayaki Saito and libsixel developers),
# vendored as the reference for bropty's sixel decoder in tests only; it is
# never linked into bropty itself.
#
# Source: https://github.com/libsixel/libsixel @ 37026b01a0bd38634ae0a8c5017bd4671101fe08
# Copied: src/fromsixel.c (unmodified), LICENSE. shim/ stands in for the rest
# of libsixel (its header, allocator and message helper).

enable_language(C)

add_library(bropty_test_libsixel STATIC fromsixel.c shim/shim.c)
set_target_properties(bropty_test_libsixel PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
# PRIVATE: shim/sixel.h would shadow bropty's src/sixel.h in the tests;
# they declare sixel_decode_raw themselves.
target_include_directories(bropty_test_libsixel PRIVATE shim)
if(MSVC)
    target_compile_options(bropty_test_libsixel PRIVATE /W0 /wd4996)
    target_compile_definitions(bropty_test_libsixel PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(bropty_test_libsixel PRIVATE -w)
endif()
