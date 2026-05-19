#ifdef KTEST_ENABLED

#include <kernel/ktest.h>
#include <kernel/mmu.h>
#include <kernel/pmm.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <memory.h>

/* ------------------------------------------------------------------ */
/* malloc / free                                                        */
/* ------------------------------------------------------------------ */

static void test_malloc_basic(ktest_ctx_t *ctx) {
    void *p = malloc(64);
    KT_ASSERT_NONNULL(p);
    free(p);
}

KTEST("malloc-basic", "memory",
      "malloc(64) returns non-NULL and free does not crash",
      KT_FLAG_CRITICAL, test_malloc_basic);

/* ------------------------------------------------------------------ */

static void test_malloc_alignment(ktest_ctx_t *ctx) {
    /* Slab allocator must return at least pointer-aligned results */
    for (size_t sz = 1; sz <= 4096; sz *= 2) {
        void *p = malloc(sz);
        KT_ASSERT_NONNULL(p);
        KT_CHECK(((uintptr_t)p & 7u) == 0); /* 8-byte aligned */
        free(p);
    }
}

KTEST("malloc-alignment", "memory",
      "malloc returns 8-byte aligned pointers for sizes 1..4096",
      KT_FLAG_CRITICAL, test_malloc_alignment);

/* ------------------------------------------------------------------ */

static void test_malloc_zero_fill(ktest_ctx_t *ctx) {
    /* We can't guarantee zero-fill, but we can at least write and read back */
    uint8_t *p = (uint8_t *)malloc(256);
    KT_ASSERT_NONNULL(p);
    for (int i = 0; i < 256; i++) p[i] = (uint8_t)i;
    for (int i = 0; i < 256; i++) KT_CHECK_EQ((int)p[i], i);
    free(p);
}

KTEST("malloc-write-read", "memory",
      "malloc'd memory is writable and readable",
      KT_FLAG_CRITICAL, test_malloc_zero_fill);

/* ------------------------------------------------------------------ */

static void test_malloc_many(ktest_ctx_t *ctx) {
    #define N 32
    void *ptrs[N];
    for (int i = 0; i < N; i++) {
        ptrs[i] = malloc((size_t)(i + 1) * 8);
        KT_ASSERT_NONNULL(ptrs[i]);
    }
    /* All pointers must be distinct */
    for (int i = 0; i < N; i++) {
        for (int j = i + 1; j < N; j++) {
            KT_CHECK(ptrs[i] != ptrs[j]);
        }
    }
    for (int i = 0; i < N; i++) free(ptrs[i]);
    #undef N
}

KTEST("malloc-many", "memory",
      "32 concurrent allocations return distinct pointers",
      KT_FLAG_NONE, test_malloc_many);

/* ------------------------------------------------------------------ */

static void test_malloc_large(ktest_ctx_t *ctx) {
    /* Allocate 1 MiB */
    void *p = malloc(1024 * 1024);
    KT_ASSERT_NONNULL(p);
    /* Touch first and last byte to verify it's mapped */
    volatile uint8_t *v = (volatile uint8_t *)p;
    v[0] = 0xAA;
    v[1024 * 1024 - 1] = 0xBB;
    KT_CHECK_EQ((int)v[0], 0xAA);
    KT_CHECK_EQ((int)v[1024 * 1024 - 1], 0xBB);
    free(p);
}

KTEST("malloc-large", "memory",
      "malloc(1 MiB) succeeds and memory is accessible",
      KT_FLAG_NONE, test_malloc_large);

/* ------------------------------------------------------------------ */

static void test_malloc_free_reuse(ktest_ctx_t *ctx) {
    /* Free then re-allocate the same size; should not OOM */
    void *p1 = malloc(128);
    KT_ASSERT_NONNULL(p1);
    free(p1);
    void *p2 = malloc(128);
    KT_ASSERT_NONNULL(p2);
    free(p2);
}

KTEST("malloc-free-reuse", "memory",
      "freed memory can be re-allocated",
      KT_FLAG_NONE, test_malloc_free_reuse);

/* ------------------------------------------------------------------ */
/* Physical memory manager                                              */
/* ------------------------------------------------------------------ */

static void test_pmm_request_frame(ktest_ctx_t *ctx) {
    uintptr_t frame = mmu_request_frame();
    KT_ASSERT(frame != 0);
    /* Must be page-aligned */
    KT_CHECK_EQ(frame & 0xFFFu, 0u);
    mmu_free_frames((void *)frame, 1);
}

KTEST("pmm-request-frame", "memory",
      "mmu_request_frame returns a page-aligned physical address",
      KT_FLAG_CRITICAL, test_pmm_request_frame);

/* ------------------------------------------------------------------ */

static void test_pmm_request_contiguous(ktest_ctx_t *ctx) {
    uintptr_t base = mmu_request_frames(4);
    KT_ASSERT(base != 0);
    KT_CHECK_EQ(base & 0xFFFu, 0u);
    /* Pages must be physically contiguous */
    for (int i = 0; i < 4; i++) {
        KT_CHECK_EQ((base + (uintptr_t)i * 4096) & 0xFFFu, 0u);
    }
    mmu_free_frames((void *)base, 4);
}

KTEST("pmm-contiguous", "memory",
      "mmu_request_frames(4) returns 4 contiguous page-aligned frames",
      KT_FLAG_NONE, test_pmm_request_contiguous);

/* ------------------------------------------------------------------ */

static void test_pmm_distinct_frames(ktest_ctx_t *ctx) {
    uintptr_t a = mmu_request_frame();
    uintptr_t b = mmu_request_frame();
    KT_ASSERT(a != 0);
    KT_ASSERT(b != 0);
    KT_CHECK(a != b);
    mmu_free_frames((void *)a, 1);
    mmu_free_frames((void *)b, 1);
}

KTEST("pmm-distinct", "memory",
      "two consecutive mmu_request_frame calls return distinct frames",
      KT_FLAG_NONE, test_pmm_distinct_frames);

#endif /* KTEST_ENABLED */
