#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <windows.h>

#include "win_malloc.h"

#define ITERATIONS 100000
#define ALLOCATION_SIZE 64

static double elapsed_ms(LARGE_INTEGER start, LARGE_INTEGER end,
                         LARGE_INTEGER frequency)
{
    return ((double)(end.QuadPart - start.QuadPart) * 1000.0)
           / (double)frequency.QuadPart;
}

/*
 * Benchmark your custom allocator.
 */
static double benchmark_wm(size_t count, size_t size)
{
    void **ptrs = malloc(count * sizeof(void *));

    if (!ptrs)
    {
        printf("Failed to allocate benchmark pointer array.\n");
        return -1.0;
    }

    LARGE_INTEGER frequency;
    LARGE_INTEGER start;
    LARGE_INTEGER end;

    QueryPerformanceFrequency(&frequency);

    QueryPerformanceCounter(&start);

    for (size_t i = 0; i < count; i++)
    {
        ptrs[i] = wm_malloc(size);

        if (!ptrs[i])
        {
            printf("wm_malloc failed at allocation %zu\n", i);

            for (size_t j = 0; j < i; j++)
                wm_free(ptrs[j]);

            free(ptrs);
            return -1.0;
        }

        /*
         * Touch the memory so the benchmark actually uses it.
         */
        memset(ptrs[i], 0xAB, size);
    }

    for (size_t i = 0; i < count; i++)
    {
        wm_free(ptrs[i]);
    }

    QueryPerformanceCounter(&end);

    double result = elapsed_ms(start, end, frequency);

    free(ptrs);

    return result;
}

/*
 * Benchmark normal malloc/free.
 */
static double benchmark_malloc(size_t count, size_t size)
{
    void **ptrs = malloc(count * sizeof(void *));

    if (!ptrs)
    {
        printf("Failed to allocate benchmark pointer array.\n");
        return -1.0;
    }

    LARGE_INTEGER frequency;
    LARGE_INTEGER start;
    LARGE_INTEGER end;

    QueryPerformanceFrequency(&frequency);

    QueryPerformanceCounter(&start);

    for (size_t i = 0; i < count; i++)
    {
        ptrs[i] = malloc(size);

        if (!ptrs[i])
        {
            printf("malloc failed at allocation %zu\n", i);

            for (size_t j = 0; j < i; j++)
                free(ptrs[j]);

            free(ptrs);
            return -1.0;
        }

        memset(ptrs[i], 0xAB, size);
    }

    for (size_t i = 0; i < count; i++)
    {
        free(ptrs[i]);
    }

    QueryPerformanceCounter(&end);

    double result = elapsed_ms(start, end, frequency);

    free(ptrs);

    return result;
}

static void run_test(size_t count, size_t size)
{
    printf("\n");
    printf("========================================\n");
    printf("Allocations : %zu\n", count);
    printf("Size        : %zu bytes\n", size);
    printf("========================================\n");

    double wm_time = benchmark_wm(count, size);
    double malloc_time = benchmark_malloc(count, size);

    if (wm_time < 0 || malloc_time < 0)
        return;

    printf("wm_malloc : %.3f ms\n", wm_time);
    printf("malloc    : %.3f ms\n", malloc_time);

    printf("Ratio     : %.2fx\n", wm_time / malloc_time);

    if (wm_time < malloc_time)
        printf("Result    : wm_malloc is faster\n");
    else
        printf("Result    : malloc is faster\n");
}

int main(void)
{
    printf("========================================\n");
    printf(" Custom Allocator Benchmark\n");
    printf("========================================\n");



  
    run_test(1000, 1048576);
    run_test(70, 1048576);
    run_test(30, 1048576);
    run_test(20, 1048576);



    wm_shutdown();

    return 0;
}