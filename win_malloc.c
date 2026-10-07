/*
 * How to use: put these prototypes in your own header (or you can simply paste them into your code):
 *
 *     #include <stddef.h>
 *     #include <stdio.h>
 *     void  *wm_malloc(size_t size);
 *     void  *wm_calloc(size_t count, size_t size);
 *     void  *wm_realloc(void *ptr, size_t size);
 *     void   wm_free(void *ptr);
 *     size_t wm_usable_size(void *ptr);
 *     typedef struct {
 *         size_t alloc_count, alloc_bytes, free_count, free_bytes, reserved_bytes;
 *     } wm_stats;
 *     void   wm_get_stats(wm_stats *out);
 *     void   wm_dump(FILE *out);
 *     void   wm_shutdown(void);
 *     size_t wm_collect(void);
 *     int    wm_gc_add_root(void *start, size_t bytes);
 */

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00 /*(needed for GetCurrentThreadStackLimits) */
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#if defined(WM_BUILD_DLL)
#define WM_API __declspec(dllexport)
#else
#define WM_API
#endif

#if defined(_MSC_VER)
#define WM_NOINLINE __declspec(noinline)
#else
#define WM_NOINLINE __attribute__((noinline))
#endif

#define WM_ALIGN 16u                             /* alignment of every returned pointer   */
#define WM_PAGE 4096u                            /* page size                              */
#define WM_REGION_MIN ((size_t)1 << 20)          /* heap grows in steps of at least 1 MiB  */
#define WM_REGION_GRAN ((size_t)64 * 1024)       /* region sizes rounded to 64 KiB         */
#define WM_DIRECT_THRESHOLD ((size_t)256 * 1024) /* >= this goes straight to VirtualAlloc */
#define WM_MAX_REQUEST (SIZE_MAX / 4)            /* sanity limit, avoids overflow          */
#define WM_LIST_INITIAL_CAP 256u

#define WM_NOT_FOUND ((size_t)-1)

#define WM_F_DIRECT 1u /* chunk owns a whole VirtualAlloc region */
#define WM_F_MARK 2u   /* used by the garbage collector */

typedef struct
{
    uintptr_t start;
    size_t size; /* bytes */
    unsigned flags;
} wm_chunk;

typedef struct
{
    wm_chunk *items;
    size_t count;
    size_t cap;
} wm_list;

static wm_list g_alloced = {0}; /* live allocations              */
static wm_list g_freed = {0};   /* free chunks inside regions    */
static wm_list g_regions = {0}; /* OS regions backing the heap   */
static wm_list g_roots = {0};   /* extra GC roots (user-added)   */
static SRWLOCK g_lock = SRWLOCK_INIT;

static size_t align_up(size_t v, size_t a)
{
    return (v + a - 1) & ~(a - 1);
}

static int list_reserve(wm_list *l, size_t need)
{
    if (need <= l->cap)
        return 1;

    size_t newcap = l->cap ? l->cap : WM_LIST_INITIAL_CAP;
    while (newcap < need)
    {
        if (newcap > SIZE_MAX / 2 / sizeof(wm_chunk))
            return 0;
        newcap *= 2;
    }

    wm_chunk *p = (wm_chunk *)VirtualAlloc(NULL, newcap * sizeof(wm_chunk),
                                           MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p)
        return 0;

    if (l->count)
        memcpy(p, l->items, l->count * sizeof(wm_chunk));
    if (l->items)
        VirtualFree(l->items, 0, MEM_RELEASE);
    l->items = p;
    l->cap = newcap;
    return 1;
}

static size_t list_lower_bound(const wm_list *l, uintptr_t addr)
{
    size_t lo = 0, hi = l->count;
    while (lo < hi)
    {
        size_t mid = lo + (hi - lo) / 2;
        if (l->items[mid].start < addr)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

static size_t list_find_exact(const wm_list *l, uintptr_t addr)
{
    size_t i = list_lower_bound(l, addr);
    if (i < l->count && l->items[i].start == addr)
        return i;
    return WM_NOT_FOUND;
}

static size_t list_find_containing(const wm_list *l, uintptr_t addr)
{
    if (l->count == 0 || addr == UINTPTR_MAX)
        return WM_NOT_FOUND;
    size_t i = list_lower_bound(l, addr + 1); /* first start > addr */
    if (i == 0)
        return WM_NOT_FOUND;
    const wm_chunk *c = &l->items[i - 1];
    if (addr - c->start < c->size)
        return i - 1;
    return WM_NOT_FOUND;
}

static int list_insert_at(wm_list *l, size_t index, wm_chunk c)
{
    if (!list_reserve(l, l->count + 1))
        return 0;
    memmove(&l->items[index + 1], &l->items[index], (l->count - index) * sizeof(wm_chunk));
    l->items[index] = c;
    l->count += 1;
    return 1;
}

static void list_remove_at(wm_list *l, size_t index)
{
    memmove(&l->items[index], &l->items[index + 1], (l->count - index - 1) * sizeof(wm_chunk));
    l->count -= 1;
}

/* Insert [start, start+size) into the free list, merging with neighbours.
   Returns 0 only if the metadata could not grow (the chunk is then leaked). */
static int freed_insert(uintptr_t start, size_t size)
{
    size_t i = list_lower_bound(&g_freed, start);
    int merge_prev = (i > 0 && g_freed.items[i - 1].start + g_freed.items[i - 1].size == start);
    int merge_next = (i < g_freed.count && start + size == g_freed.items[i].start);

    if (merge_prev && merge_next)
    {
        g_freed.items[i - 1].size += size + g_freed.items[i].size;
        list_remove_at(&g_freed, i);
    }
    else if (merge_prev)
    {
        g_freed.items[i - 1].size += size;
    }
    else if (merge_next)
    {
        g_freed.items[i].start = start;
        g_freed.items[i].size += size;
    }
    else
    {
        wm_chunk c = {start, size, 0};
        return list_insert_at(&g_freed, i, c);
    }
    return 1;
}

/* Ask the OS for a new region able to satisfy `need` bytes and add it to the free list. */
static int add_region(size_t need)
{
    size_t size = align_up(need > WM_REGION_MIN ? need : WM_REGION_MIN, WM_REGION_GRAN);
    void *p = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p)
        return 0;

    wm_chunk region = {(uintptr_t)p, size, 0};
    size_t ri = list_lower_bound(&g_regions, region.start);
    if (!list_insert_at(&g_regions, ri, region))
    {
        VirtualFree(p, 0, MEM_RELEASE);
        return 0;
    }
    if (!freed_insert(region.start, size))
    {
        list_remove_at(&g_regions, ri);
        VirtualFree(p, 0, MEM_RELEASE);
        return 0;
    }
    return 1;
}

static void *alloc_locked(size_t size) /*for holding the lock*/
{
    if (size > WM_MAX_REQUEST)
    {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }

    const size_t n = align_up(size ? size : 1, WM_ALIGN);

    if (!list_reserve(&g_alloced, g_alloced.count + 1))
    {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }

    if (n >= WM_DIRECT_THRESHOLD)
    {
        const size_t bytes = align_up(n, WM_PAGE);
        void *p = VirtualAlloc(NULL, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!p)
            return NULL;
        wm_chunk c = {(uintptr_t)p, bytes, WM_F_DIRECT};
        list_insert_at(&g_alloced, list_lower_bound(&g_alloced, c.start), c);
        return p;
    }

    for (int attempt = 0; attempt < 2; ++attempt)
    {
        for (size_t i = 0; i < g_freed.count; ++i)
        {
            wm_chunk *f = &g_freed.items[i];
            if (f->size >= n)
            {
                const uintptr_t start = f->start;
                if (f->size == n)
                {
                    list_remove_at(&g_freed, i);
                }
                else
                {
                    f->start += n;
                    f->size -= n;
                }
                wm_chunk c = {start, n, 0};
                list_insert_at(&g_alloced, list_lower_bound(&g_alloced, start), c);
                return (void *)start;
            }
        }
        if (attempt == 0 && !add_region(n))
        {
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return NULL;
        }
    }
    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return NULL;
}

static void free_locked(void *ptr)
{
    size_t i = list_find_exact(&g_alloced, (uintptr_t)ptr);
    if (i == WM_NOT_FOUND)
        return; /* unknown pointer or double free gets ignored */

    const wm_chunk c = g_alloced.items[i];
    list_remove_at(&g_alloced, i);

    if (c.flags & WM_F_DIRECT)
        VirtualFree((void *)c.start, 0, MEM_RELEASE);
    else
        freed_insert(c.start, c.size);
}

WM_API void *wm_malloc(size_t size) /* The api used to actually call */
{
    AcquireSRWLockExclusive(&g_lock);
    void *p = alloc_locked(size);
    ReleaseSRWLockExclusive(&g_lock);
    return p;
}

WM_API void wm_free(void *ptr)
{
    if (!ptr)
        return;
    AcquireSRWLockExclusive(&g_lock);
    free_locked(ptr);
    ReleaseSRWLockExclusive(&g_lock);
}

WM_API void *wm_calloc(size_t count, size_t size)
{
    if (size != 0 && count > WM_MAX_REQUEST / size)
    {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    const size_t total = count * size;
    void *p = wm_malloc(total);
    if (p)
        memset(p, 0, total); /* recycle the chunks that are not zero */
    return p;
}

WM_API void *wm_realloc(void *ptr, size_t size)
{
    if (!ptr)
        return wm_malloc(size);
    if (size == 0)
    {
        wm_free(ptr);
        return NULL;
    }

    AcquireSRWLockExclusive(&g_lock);

    size_t i = list_find_exact(&g_alloced, (uintptr_t)ptr);
    if (i == WM_NOT_FOUND || size > WM_MAX_REQUEST)
    {
        ReleaseSRWLockExclusive(&g_lock);
        SetLastError(i == WM_NOT_FOUND ? ERROR_INVALID_PARAMETER : ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }

    const wm_chunk c = g_alloced.items[i];
    const size_t n = align_up(size, WM_ALIGN);
    void *result = NULL;

    if (c.flags & WM_F_DIRECT)
    {
        /* keep the region if the new size still fits and is not  wastefully small */
        if (n >= WM_DIRECT_THRESHOLD && n <= c.size && n >= c.size / 2)
            result = ptr;
    }
    else if (n <= c.size)
    {
        const size_t tail = c.size - n;
        if (tail)
        {
            g_alloced.items[i].size = n;
            freed_insert(c.start + n, tail);
        }
        result = ptr;
    }
    else if (n < WM_DIRECT_THRESHOLD)
    {
        const uintptr_t end = c.start + c.size;
        size_t j = list_lower_bound(&g_freed, end);
        const size_t extra = n - c.size;
        if (j < g_freed.count && g_freed.items[j].start == end && g_freed.items[j].size >= extra)
        {
            wm_chunk *f = &g_freed.items[j];
            if (f->size == extra)
            {
                list_remove_at(&g_freed, j);
            }
            else
            {
                f->start += extra;
                f->size -= extra;
            }
            g_alloced.items[i].size = n;
            result = ptr;
        }
    }

    if (!result)
    {
        void *np = alloc_locked(size);
        if (np)
        {
            memcpy(np, ptr, c.size < size ? c.size : size);
            free_locked(ptr);
        }
        result = np; /* NULL leaves the original block untouched */
    }

    ReleaseSRWLockExclusive(&g_lock);
    return result;
}

/* Number of bytes that you may actually use at ptr (>= the size requested) it will be 0 if unknown. */
WM_API size_t wm_usable_size(void *ptr)
{
    size_t r = 0;
    if (!ptr)
        return 0;
    AcquireSRWLockExclusive(&g_lock);
    size_t i = list_find_exact(&g_alloced, (uintptr_t)ptr);
    if (i != WM_NOT_FOUND)
        r = g_alloced.items[i].size;
    ReleaseSRWLockExclusive(&g_lock);
    return r;
}

typedef struct
{
    size_t alloc_count;
    size_t alloc_bytes;
    size_t free_count;
    size_t free_bytes;
    size_t reserved_bytes;
} wm_stats;

WM_API void wm_get_stats(wm_stats *out)
{
    memset(out, 0, sizeof(*out));
    AcquireSRWLockExclusive(&g_lock);
    out->alloc_count = g_alloced.count;
    for (size_t i = 0; i < g_alloced.count; ++i)
    {
        out->alloc_bytes += g_alloced.items[i].size;
        if (g_alloced.items[i].flags & WM_F_DIRECT)
            out->reserved_bytes += g_alloced.items[i].size;
    }
    out->free_count = g_freed.count;
    for (size_t i = 0; i < g_freed.count; ++i)
        out->free_bytes += g_freed.items[i].size;
    for (size_t i = 0; i < g_regions.count; ++i)
        out->reserved_bytes += g_regions.items[i].size;
    ReleaseSRWLockExclusive(&g_lock);
}

WM_API void wm_dump(FILE *out)
{
    AcquireSRWLockExclusive(&g_lock);
    fprintf(out, "Alloced Chunks (%zu):\n", g_alloced.count);
    for (size_t i = 0; i < g_alloced.count; ++i)
        fprintf(out, "  start: %p, size: %zu%s\n", (void *)g_alloced.items[i].start,
                g_alloced.items[i].size, (g_alloced.items[i].flags & WM_F_DIRECT) ? " (direct)" : "");
    fprintf(out, "Freed Chunks (%zu):\n", g_freed.count);
    for (size_t i = 0; i < g_freed.count; ++i)
        fprintf(out, "  start: %p, size: %zu\n", (void *)g_freed.items[i].start, g_freed.items[i].size);
    ReleaseSRWLockExclusive(&g_lock);
}

/* Release EVERYTHING back to the OS. All pointers handed out become invalid.
   The allocator can be used again afterwards. */
WM_API void wm_shutdown(void)
{
    AcquireSRWLockExclusive(&g_lock);
    for (size_t i = 0; i < g_alloced.count; ++i)
        if (g_alloced.items[i].flags & WM_F_DIRECT)
            VirtualFree((void *)g_alloced.items[i].start, 0, MEM_RELEASE);
    for (size_t i = 0; i < g_regions.count; ++i)
        VirtualFree((void *)g_regions.items[i].start, 0, MEM_RELEASE);

    wm_list *all[] = {&g_alloced, &g_freed, &g_regions, &g_roots};
    for (size_t k = 0; k < sizeof(all) / sizeof(all[0]); ++k)
    {
        if (all[k]->items)
            VirtualFree(all[k]->items, 0, MEM_RELEASE);
        all[k]->items = NULL;
        all[k]->count = all[k]->cap = 0;
    }
    ReleaseSRWLockExclusive(&g_lock);
}

static size_t *g_mark_stack = NULL;
static size_t g_mark_count = 0, g_mark_cap = 0;
static int g_mark_overflow = 0;

static void mark_push(size_t index)
{
    if (g_mark_count == g_mark_cap)
    {
        size_t newcap = g_mark_cap ? g_mark_cap * 2 : 1024;
        size_t *p = (size_t *)VirtualAlloc(NULL, newcap * sizeof(size_t),
                                           MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!p)
        {
            g_mark_overflow = 1;
            return;
        }
        if (g_mark_count)
            memcpy(p, g_mark_stack, g_mark_count * sizeof(size_t));
        if (g_mark_stack)
            VirtualFree(g_mark_stack, 0, MEM_RELEASE);
        g_mark_stack = p;
        g_mark_cap = newcap;
    }
    g_mark_stack[g_mark_count++] = index;
}

static void mark_word(uintptr_t w)
{
    size_t i = list_find_containing(&g_alloced, w);
    if (i != WM_NOT_FOUND && !(g_alloced.items[i].flags & WM_F_MARK))
    {
        g_alloced.items[i].flags |= WM_F_MARK;
        mark_push(i);
    }
}

static void scan_range(uintptr_t lo, uintptr_t hi)
{
    lo = (lo + sizeof(uintptr_t) - 1) & ~(uintptr_t)(sizeof(uintptr_t) - 1);
    for (uintptr_t p = lo; p + sizeof(uintptr_t) <= hi && !g_mark_overflow; p += sizeof(uintptr_t))
        mark_word(*(const uintptr_t *)p);
}

static void mark_drain(void)
{
    while (g_mark_count > 0 && !g_mark_overflow)
    {
        const wm_chunk c = g_alloced.items[g_mark_stack[--g_mark_count]];
        scan_range(c.start, c.start + c.size);
    }
}

static WM_NOINLINE void gc_scan_stack(void)
{
    volatile uintptr_t marker = 0;
    ULONG_PTR low = 0, high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    scan_range((uintptr_t)&marker, (uintptr_t)high);
}

WM_API int wm_gc_add_root(void *start, size_t bytes)
{
    int ok;
    AcquireSRWLockExclusive(&g_lock);
    wm_chunk c = {(uintptr_t)start, bytes, 0};
    ok = list_insert_at(&g_roots, list_lower_bound(&g_roots, c.start), c);
    ReleaseSRWLockExclusive(&g_lock);
    return ok;
}

WM_API size_t wm_collect(void) /* Returns the number of allocations that were freed. */
{
    CONTEXT ctx;
    RtlCaptureContext(&ctx);

    AcquireSRWLockExclusive(&g_lock);

    g_mark_count = 0;
    g_mark_overflow = 0;

    scan_range((uintptr_t)&ctx, (uintptr_t)&ctx + sizeof(ctx));
    gc_scan_stack();
    for (size_t i = 0; i < g_roots.count; ++i)
        scan_range(g_roots.items[i].start, g_roots.items[i].start + g_roots.items[i].size);
    mark_drain();

    size_t freed = 0;
    size_t keep = 0;
    for (size_t i = 0; i < g_alloced.count; ++i)
    {
        wm_chunk c = g_alloced.items[i];
        if (g_mark_overflow || (c.flags & WM_F_MARK))
        {
            c.flags &= ~(unsigned)WM_F_MARK;
            g_alloced.items[keep++] = c;
        }
        else
        {
            if (c.flags & WM_F_DIRECT)
                VirtualFree((void *)c.start, 0, MEM_RELEASE);
            else
                freed_insert(c.start, c.size);
            freed += 1;
        }
    }
    g_alloced.count = keep;

    ReleaseSRWLockExclusive(&g_lock);
    return freed;
}
