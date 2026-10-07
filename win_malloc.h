#ifndef WIN_MALLOC_H
#define WIN_MALLOC_H

#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    size_t alloc_count;
    size_t alloc_bytes;
    size_t free_count;
    size_t free_bytes;
    size_t reserved_bytes;
} wm_stats;

void  *wm_malloc(size_t size);
void  *wm_calloc(size_t count, size_t size);
void  *wm_realloc(void *ptr, size_t size);
void   wm_free(void *ptr);
size_t wm_usable_size(void *ptr);
void   wm_get_stats(wm_stats *out);
void   wm_dump(FILE *out);
void   wm_shutdown(void);
size_t wm_collect(void);
int    wm_gc_add_root(void *start, size_t bytes);

#ifdef __cplusplus
}
#endif

#endif /* WIN_MALLOC_H */