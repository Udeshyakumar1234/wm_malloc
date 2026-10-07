# wm_malloc

`wm_malloc` is a simple malloc-like custom memory allocator made especially for Windows.

I built it to learn more about memory management, heap allocation and fragmentation. Instead of `mmap`/`brk` (used by Unix-like systems), it is built directly on the low-level Windows API `VirtualAlloc`.

## Features

- `malloc`, `calloc`, `realloc` and `free` equivalents
- 16-byte aligned allocations
- Free-chunk coalescing and in-place `realloc` (shrink and grow)
- Large allocations (>= 256 KiB) go straight to `VirtualAlloc`
- Out-of-band metadata, so buffer overflows can't corrupt allocator state
- Allocation statistics and heap dump helpers
- Optional conservative garbage collector (`wm_collect`)

## API

| Function | Description |
|---|---|
| `void *wm_malloc(size_t size)` | Allocate `size` bytes |
| `void *wm_calloc(size_t count, size_t size)` | Allocate zero-initialised memory |
| `void *wm_realloc(void *ptr, size_t size)` | Resize an allocation |
| `void wm_free(void *ptr)` | Free an allocation |
| `size_t wm_usable_size(void *ptr)` | Usable bytes at `ptr` (0 if unknown) |
| `void wm_get_stats(wm_stats *out)` | Fill in allocation statistics |
| `void wm_dump(FILE *out)` | Print all allocated and free chunks |
| `void wm_shutdown(void)` | Release everything back to the OS |
| `size_t wm_collect(void)` | Run the garbage collector, returns number of allocations freed |
| `int wm_gc_add_root(void *start, size_t bytes)` | Register an extra memory range for the GC to scan |

## How to use

**Requirement:** MSYS2 with GCC.

1. The only source file you need is `win_malloc.c`. To use it as a library, you need a header file that declares the functions. You can write your own, or use the `win_malloc.h` included in this repo. You can also simply clone the repo.

2. **Important:** include the header in your code with `#include "win_malloc.h"`, then compile your code **together with** `win_malloc.c`.

   For example, `test.c` contains the benchmarks and uses `win_malloc.c` as its library, so compile them together with:

   ```
   gcc -O2 -Wall -Wextra win_malloc.c test.c -o test.exe
   ```

   Then run it with:

   ```
   .\test.exe
   ```

### Minimal example

```c
#include <stdio.h>
#include "win_malloc.h"

int main(void)
{
    int *a = wm_malloc(100 * sizeof(int));
    for (int i = 0; i < 100; ++i)
        a[i] = i;

    a = wm_realloc(a, 200 * sizeof(int));
    printf("usable: %zu bytes\n", wm_usable_size(a));

    wm_free(a);
    wm_shutdown();
    return 0;
}
```

```
gcc -O2 -Wall -Wextra win_malloc.c example.c -o example.exe
```

## Performance comparison

`wm_malloc` is competitive with, and sometimes faster than, the standard `malloc` for a moderate number of allocations or larger block sizes (up to about 2x faster in the best case). However, it performs significantly worse as the number of small allocations grows large.

The benchmark code is in `test.c`. It is set up to run 100 times and append each result to an output CSV. All raw results are in the `benchmark_results` file, with summaries below.

### Where wm_malloc is faster

| Test | Result |
|---|---|
| 4 KB x 1,000 | `wm_malloc` ~2.06x faster |
| 1 MB x 20 | ~15.5% faster |
| 1 MB x 30 | ~19.3% faster |
| 1 MB x 70 | ~10.2% faster |
| 16 KB x 5,000 | ~13.4% faster |

### Where wm_malloc degrades

As allocation size gets smaller and the allocation count gets larger, `wm_malloc` degrades dramatically:

| Test | Result |
|---|---|
| 4 KB x 7,000 | `malloc` ~2.81x faster |
| 1 KB x 8,000 | `malloc` ~9.04x faster |
| 256 B x 9,000 | `malloc` ~22.2x faster |
| 128 B x 10,000 | `malloc` ~22.8x faster |

<img width="989" height="490" alt="Per-workload wins" src="https://github.com/user-attachments/assets/094cd2e0-b4b1-4763-a042-df6a90df5427" />

<img width="989" height="490" alt="Relative performance by workload (lower is better)" src="https://github.com/user-attachments/assets/54beb579-d15c-4f7f-8384-d5251a65c465" />



## Known limitations and future improvements

1. **Not built for multithreaded performance.** All operations are protected by a single global lock (an SRW lock), so the allocator is thread-safe, but threads contend on that lock and it does not scale. Per-thread caches or sharding could improve this.

2. **O(n) cost per operation.** Allocation metadata is kept in a sorted array, so every allocation and free may `memmove` up to the whole array (`memmove` safely copies a block of memory from a source to a destination, even if they overlap). With very large numbers of allocations the total time therefore grows quadratically. A hash table or tree for lookups and size-segregated free lists would fix most of this.

3. **Metadata overhead.** A 16-byte allocation costs 24 bytes of metadata (stored out of band). Workloads with many small objects will use roughly 2.5x the memory.

4. **The garbage collector is conservative and single-threaded.** `wm_collect` only scans the calling thread's stack and registers, plus any ranges added with `wm_gc_add_root`. Objects referenced only from other threads' stacks, global variables, or memory from other allocators may be freed incorrectly unless you register them as roots.
