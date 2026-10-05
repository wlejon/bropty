/* Minimal stand-in for libsixel's public header: just what fromsixel.c
 * (the decoder) needs. bropty test-only. */
#ifndef BROPTY_LIBSIXEL_SHIM_H
#define BROPTY_LIBSIXEL_SHIM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SIXELAPI
typedef int SIXELSTATUS;

#define SIXEL_PALETTE_MAX 256
#define SIXEL_WIDTH_LIMIT 1000000
#define SIXEL_HEIGHT_LIMIT 1000000

#define SIXEL_OK 0x0000
#define SIXEL_FALSE 0x1000
#define SIXEL_RUNTIME_ERROR (SIXEL_FALSE | 0x0100)
#define SIXEL_BAD_ALLOCATION (SIXEL_RUNTIME_ERROR | 0x0001)
#define SIXEL_BAD_INPUT (SIXEL_RUNTIME_ERROR | 0x0003)
#define SIXEL_BAD_INTEGER_OVERFLOW (SIXEL_RUNTIME_ERROR | 0x0004)
#define SIXEL_FAILED(status) (((status) & 0x1000) != 0)

typedef struct sixel_allocator sixel_allocator_t;
typedef void *(*sixel_allocator_function)(size_t);

SIXELSTATUS sixel_allocator_new(sixel_allocator_t **ppallocator, void *(*fn_malloc)(size_t),
                                void *(*fn_calloc)(size_t, size_t), void *(*fn_realloc)(void *, size_t),
                                void (*fn_free)(void *));
void sixel_allocator_ref(sixel_allocator_t *allocator);
void sixel_allocator_unref(sixel_allocator_t *allocator);
void *sixel_allocator_malloc(sixel_allocator_t *allocator, size_t n);
void sixel_allocator_free(sixel_allocator_t *allocator, void *p);

SIXELSTATUS sixel_decode_raw(unsigned char *p, int len, unsigned char **pixels, int *pwidth, int *pheight,
                             unsigned char **palette, int *ncolors, sixel_allocator_t *allocator);

#ifdef __cplusplus
}
#endif

#endif
