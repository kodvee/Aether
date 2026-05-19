#ifdef KTEST_ENABLED

#include <kernel/ktest.h>
#include <kernel/mmu.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ================================================================== */
/* Group 1: basic malloc / free                                         */
/* ================================================================== */

static void test_slab_malloc_small(ktest_ctx_t *ctx) {
    void *p = malloc(8);
    KT_ASSERT_NONNULL(p);
    free(p);
}

KTEST("slab-malloc-small", "slab",
      "malloc(8) returns non-NULL",
      KT_FLAG_CRITICAL, test_slab_malloc_small);

/* ------------------------------------------------------------------ */

static void test_slab_malloc_alignment(ktest_ctx_t *ctx) {
    /* All allocations must be at least 8-byte aligned */
    static const size_t sizes[] = { 1, 2, 4, 7, 8, 15, 16, 31, 32, 63, 64,
                                    127, 128, 255, 256, 511, 512, 1023, 1024,
                                    2047, 2048 };
    for (size_t i = 0; i < sizeof(sizes)/sizeof(sizes[0]); i++) {
        void *p = malloc(sizes[i]);
        KT_ASSERT_NONNULL(p);
        KT_CHECK(((uintptr_t)p & 7u) == 0u);
        free(p);
    }
}

KTEST("slab-malloc-alignment", "slab",
      "malloc returns 8-byte aligned pointers for sizes 1..2048",
      KT_FLAG_CRITICAL, test_slab_malloc_alignment);

/* ------------------------------------------------------------------ */

static void test_slab_malloc_distinct_ptrs(ktest_ctx_t *ctx) {
    void *a = malloc(64);
    void *b = malloc(64);
    KT_ASSERT_NONNULL(a);
    KT_ASSERT_NONNULL(b);
    KT_CHECK(a != b);
    free(a);
    free(b);
}

KTEST("slab-malloc-distinct-ptrs", "slab",
      "two malloc(64) calls return different pointers",
      KT_FLAG_CRITICAL, test_slab_malloc_distinct_ptrs);

/* ------------------------------------------------------------------ */

static void test_slab_malloc_writable(ktest_ctx_t *ctx) {
    uint8_t *p = (uint8_t *)malloc(256);
    KT_ASSERT_NONNULL(p);

    for (int i = 0; i < 256; i++)
        p[i] = (uint8_t)(i ^ 0xAA);
    for (int i = 0; i < 256; i++)
        KT_CHECK_EQ((int)p[i], (int)((uint8_t)(i ^ 0xAA)));

    free(p);
}

KTEST("slab-malloc-writable", "slab",
      "malloc'd region is fully writable and retains written values",
      KT_FLAG_CRITICAL, test_slab_malloc_writable);

/* ================================================================== */
/* Group 2: free(NULL)                                                  */
/* ================================================================== */

static void test_slab_free_null(ktest_ctx_t *ctx) {
    free(NULL);  /* must be a safe no-op */
    KT_CHECK(true);
}

KTEST("slab-free-null", "slab",
      "free(NULL) is a safe no-op",
      KT_FLAG_CRITICAL, test_slab_free_null);

/* ================================================================== */
/* Group 3: size classes                                                */
/*                                                                      */
/* Typical slab allocators have size classes at powers of two.         */
/* Verify that every size class boundary round-trips correctly.         */
/* ================================================================== */

static void test_slab_size_classes(ktest_ctx_t *ctx) {
    static const size_t classes[] = { 8, 16, 32, 64, 128, 256, 512, 1024, 2048 };

    for (size_t i = 0; i < sizeof(classes)/sizeof(classes[0]); i++) {
        uint8_t *p = (uint8_t *)malloc(classes[i]);
        KT_ASSERT_NONNULL(p);

        /* Write the full allocation */
        for (size_t j = 0; j < classes[i]; j++)
            p[j] = (uint8_t)(j & 0xFF);

        /* Verify */
        bool ok = true;
        for (size_t j = 0; j < classes[i]; j++) {
            if (p[j] != (uint8_t)(j & 0xFF)) { ok = false; break; }
        }
        KT_CHECK(ok);

        free(p);
    }
}

KTEST("slab-size-classes", "slab",
      "all slab size classes (8..2048) are writable and correct",
      KT_FLAG_CRITICAL, test_slab_size_classes);

/* ================================================================== */
/* Group 4: realloc                                                     */
/* ================================================================== */

static void test_slab_realloc_grow(ktest_ctx_t *ctx) {
    uint8_t *p = (uint8_t *)malloc(32);
    KT_ASSERT_NONNULL(p);

    for (int i = 0; i < 32; i++) p[i] = (uint8_t)i;

    uint8_t *q = (uint8_t *)realloc(p, 128);
    KT_ASSERT_NONNULL(q);

    /* Original data must be preserved in the grown allocation */
    for (int i = 0; i < 32; i++)
        KT_CHECK_EQ((int)q[i], i);

    free(q);
}

KTEST("slab-realloc-grow", "slab",
      "realloc to a larger size preserves original data",
      KT_FLAG_CRITICAL, test_slab_realloc_grow);

/* ------------------------------------------------------------------ */

static void test_slab_realloc_shrink(ktest_ctx_t *ctx) {
    uint8_t *p = (uint8_t *)malloc(256);
    KT_ASSERT_NONNULL(p);

    for (int i = 0; i < 64; i++) p[i] = (uint8_t)(i + 1);

    uint8_t *q = (uint8_t *)realloc(p, 64);
    KT_ASSERT_NONNULL(q);

    /* First 64 bytes must be preserved */
    for (int i = 0; i < 64; i++)
        KT_CHECK_EQ((int)q[i], i + 1);

    free(q);
}

KTEST("slab-realloc-shrink", "slab",
      "realloc to a smaller size preserves data up to new size",
      KT_FLAG_NONE, test_slab_realloc_shrink);

/* ------------------------------------------------------------------ */

static void test_slab_realloc_null(ktest_ctx_t *ctx) {
    /* realloc(NULL, n) must behave like malloc(n) */
    void *p = realloc(NULL, 64);
    KT_ASSERT_NONNULL(p);
    free(p);
}

KTEST("slab-realloc-null", "slab",
      "realloc(NULL, n) behaves like malloc(n)",
      KT_FLAG_CRITICAL, test_slab_realloc_null);

/* ================================================================== */
/* Group 5: stress                                                      */
/* ================================================================== */

static void test_slab_stress_alloc_free(ktest_ctx_t *ctx) {
    #define NPTRS 64
    void *ptrs[NPTRS];

    /* Allocate all */
    for (int i = 0; i < NPTRS; i++) {
        ptrs[i] = malloc((size_t)(8 + i * 4));
        KT_ASSERT_NONNULL(ptrs[i]);
    }

    /* All pointers must be distinct */
    for (int i = 0; i < NPTRS; i++) {
        for (int j = i + 1; j < NPTRS; j++) {
            KT_CHECK(ptrs[i] != ptrs[j]);
        }
    }

    /* Free all */
    for (int i = 0; i < NPTRS; i++)
        free(ptrs[i]);

    /* Allocate again to verify the allocator recovered */
    for (int i = 0; i < NPTRS; i++) {
        ptrs[i] = malloc(64);
        KT_ASSERT_NONNULL(ptrs[i]);
    }
    for (int i = 0; i < NPTRS; i++)
        free(ptrs[i]);

    #undef NPTRS
}

KTEST("slab-stress-alloc-free", "slab",
      "64 allocs of varying size produce distinct pointers; allocator recovers after free",
      KT_FLAG_STRESS, test_slab_stress_alloc_free);

/* ================================================================== */
/* Group 6: large allocation (page-granule path)                        */
/* ================================================================== */

static void test_slab_large_alloc(ktest_ctx_t *ctx) {
    /* 8 KiB: forces a large (page-granule) allocation path in most slabs */
    size_t sz = 8u * 1024u;
    uint8_t *p = (uint8_t *)malloc(sz);
    KT_ASSERT_NONNULL(p);

    /* Touch every byte */
    for (size_t i = 0; i < sz; i++) p[i] = (uint8_t)(i & 0xFF);

    bool ok = true;
    for (size_t i = 0; i < sz; i++) {
        if (p[i] != (uint8_t)(i & 0xFF)) { ok = false; break; }
    }
    KT_CHECK(ok);

    free(p);
}

KTEST("slab-large-alloc", "slab",
      "malloc(8 KiB) returns writable memory covering the full range",
      KT_FLAG_NONE, test_slab_large_alloc);

#endif /* KTEST_ENABLED */
