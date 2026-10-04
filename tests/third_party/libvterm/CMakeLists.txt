# libvterm (MIT, Paul Evans), vendored for the differential test only; it is
# never linked into bropty itself.
#
# Source: https://github.com/neovim/libvterm @ 934bc2fbf21800ac3458a499df8820ca5fb45fd3
# (0.3.3). Copied: include/, src/*.c except unicode.c, src/*.h,
# src/encoding/*.inc. unicode.c is replaced by bropty_width_shim.c so both
# emulators use the same per-code-point width table and the comparison
# exercises state handling, not width-table vintage.
#
# Local patches (grep "bropty patch"):
#  - state.c putglyph: a wide glyph that fills the last two columns leaves
#    the cursor on the last column (xterm), not on the glyph's lead.
#  - state.c on_csi: ICH / DCH / ECH / EL / ED clear the phantom column
#    (pending wrap) as in xterm; upstream clears it only when the cursor moves.
#  - state.c linefeed / RI: likewise for LF / VT / FF / IND / NEL / RI at
#    the margin, where the cursor does not move.
#  - state.c savecursor (+ vterm_internal.h, + DECRST 1049): DECSC / DECRC
#    and 1049 save and restore the pending wrap, as Ghostty's and bropty's
#    saved cursor do.
#  - state.c on_text: a wide glyph that does not fit blanks the rest of the
#    row before wrapping (Ghostty's spacer head; upstream leaves the cell).
#  - state.c on_text: a combining mark attaches to a glyph in the last
#    column while the cursor is pending wrap (upstream drops it).
# Every other libvterm/xterm difference is excluded by the stream generator
# (tests/vterm_diff_gen.cpp) or normalized in tests/test_vterm_diff.cpp.

enable_language(C)

add_library(bropty_test_vterm STATIC
    src/encoding.c
    src/keyboard.c
    src/mouse.c
    src/parser.c
    src/pen.c
    src/screen.c
    src/state.c
    src/vterm.c
    bropty_width_shim.c
)
set_target_properties(bropty_test_vterm PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_include_directories(bropty_test_vterm PUBLIC include PRIVATE src)
if(MSVC)
    target_compile_options(bropty_test_vterm PRIVATE /W0 /wd4996)
    target_compile_definitions(bropty_test_vterm PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(bropty_test_vterm PRIVATE -w)
endif()
