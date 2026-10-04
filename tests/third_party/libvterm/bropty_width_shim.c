/* Replaces libvterm's unicode.c: widths come from bropty's Unicode tables
 * (provided by the test executable), so libvterm and bropty agree on every
 * code point's width and the differential test compares behaviour only. */
#include "vterm_internal.h"

extern int bropty_test_width(uint32_t codepoint);

INTERNAL int vterm_unicode_width(uint32_t codepoint)
{
  return bropty_test_width(codepoint);
}

INTERNAL int vterm_unicode_is_combining(uint32_t codepoint)
{
  return codepoint >= 0x20 && bropty_test_width(codepoint) == 0;
}
