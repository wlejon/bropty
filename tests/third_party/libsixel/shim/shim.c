/* The pieces of libsixel the decoder calls: a plain malloc allocator and a
 * message sink. bropty test-only. */
#include "sixel.h"

#include <stdlib.h>

struct sixel_allocator {
    int refs;
};

SIXELSTATUS sixel_allocator_new(sixel_allocator_t **ppallocator, void *(*fn_malloc)(size_t),
                                void *(*fn_calloc)(size_t, size_t), void *(*fn_realloc)(void *, size_t),
                                void (*fn_free)(void *)) {
    (void)fn_malloc;
    (void)fn_calloc;
    (void)fn_realloc;
    (void)fn_free;
    *ppallocator = (sixel_allocator_t *)malloc(sizeof(sixel_allocator_t));
    if (!*ppallocator) return SIXEL_BAD_ALLOCATION;
    (*ppallocator)->refs = 1;
    return SIXEL_OK;
}

void sixel_allocator_ref(sixel_allocator_t *a) { ++a->refs; }

void sixel_allocator_unref(sixel_allocator_t *a) {
    if (a && --a->refs == 0) free(a);
}

void *sixel_allocator_malloc(sixel_allocator_t *a, size_t n) {
    (void)a;
    return malloc(n ? n : 1);
}

void sixel_allocator_free(sixel_allocator_t *a, void *p) {
    (void)a;
    free(p);
}

void sixel_helper_set_additional_message(const char *message) { (void)message; }
