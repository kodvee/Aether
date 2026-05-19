#ifdef KTEST_ENABLED

#include <kernel/ktest.h>
#include <kernel/dlist.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ================================================================== */
/* Group 1: create / destroy                                            */
/* ================================================================== */

static void test_dlist_create_nonnull(ktest_ctx_t *ctx) {
    dlist_t *dl = dlist_create();
    KT_ASSERT_NONNULL(dl);
    dlist_destroy(dl);
}

KTEST("dlist-create-nonnull", "dlist",
      "dlist_create returns a non-NULL pointer",
      KT_FLAG_CRITICAL, test_dlist_create_nonnull);

/* ------------------------------------------------------------------ */

static void test_dlist_create_empty(ktest_ctx_t *ctx) {
    dlist_t *dl = dlist_create();
    KT_ASSERT_NONNULL(dl);
    KT_CHECK_EQ(dlist_get_length(dl), (size_t)0);
    dlist_destroy(dl);
}

KTEST("dlist-create-empty", "dlist",
      "freshly created dlist has length 0",
      KT_FLAG_CRITICAL, test_dlist_create_empty);

/* ------------------------------------------------------------------ */

static void test_dlist_destroy_empty(ktest_ctx_t *ctx) {
    dlist_t *dl = dlist_create();
    KT_ASSERT_NONNULL(dl);
    dlist_destroy(dl);  /* must not crash */
    KT_CHECK(true);
}

KTEST("dlist-destroy-empty", "dlist",
      "dlist_destroy on an empty list does not crash",
      KT_FLAG_CRITICAL, test_dlist_destroy_empty);

/* ================================================================== */
/* Group 2: push_back / push_front                                      */
/* ================================================================== */

static void test_dlist_push_back_length(ktest_ctx_t *ctx) {
    dlist_t *dl = dlist_create();
    KT_ASSERT_NONNULL(dl);

    dlist_push_back(dl, (void *)1);
    KT_CHECK_EQ(dlist_get_length(dl), (size_t)1);
    dlist_push_back(dl, (void *)2);
    KT_CHECK_EQ(dlist_get_length(dl), (size_t)2);
    dlist_push_back(dl, (void *)3);
    KT_CHECK_EQ(dlist_get_length(dl), (size_t)3);

    dlist_destroy(dl);
}

KTEST("dlist-push-back-length", "dlist",
      "dlist_push_back increments length on each call",
      KT_FLAG_CRITICAL, test_dlist_push_back_length);

/* ------------------------------------------------------------------ */

static void test_dlist_push_front_length(ktest_ctx_t *ctx) {
    dlist_t *dl = dlist_create();
    KT_ASSERT_NONNULL(dl);

    dlist_push_front(dl, (void *)10);
    dlist_push_front(dl, (void *)20);
    KT_CHECK_EQ(dlist_get_length(dl), (size_t)2);

    dlist_destroy(dl);
}

KTEST("dlist-push-front-length", "dlist",
      "dlist_push_front increments length on each call",
      KT_FLAG_NONE, test_dlist_push_front_length);

/* ================================================================== */
/* Group 3: pop_front / pop_back                                        */
/* ================================================================== */

static void test_dlist_pop_front_fifo(ktest_ctx_t *ctx) {
    dlist_t *dl = dlist_create();
    KT_ASSERT_NONNULL(dl);

    dlist_push_back(dl, (void *)100);
    dlist_push_back(dl, (void *)200);
    dlist_push_back(dl, (void *)300);

    KT_CHECK_EQ(dlist_pop_front(dl), (void *)100);
    KT_CHECK_EQ(dlist_pop_front(dl), (void *)200);
    KT_CHECK_EQ(dlist_pop_front(dl), (void *)300);
    KT_CHECK_EQ(dlist_get_length(dl), (size_t)0);

    dlist_destroy(dl);
}

KTEST("dlist-pop-front-fifo", "dlist",
      "push_back + pop_front produces FIFO order",
      KT_FLAG_CRITICAL, test_dlist_pop_front_fifo);

/* ------------------------------------------------------------------ */

static void test_dlist_pop_back_lifo(ktest_ctx_t *ctx) {
    dlist_t *dl = dlist_create();
    KT_ASSERT_NONNULL(dl);

    dlist_push_back(dl, (void *)1);
    dlist_push_back(dl, (void *)2);
    dlist_push_back(dl, (void *)3);

    KT_CHECK_EQ(dlist_pop_back(dl), (void *)3);
    KT_CHECK_EQ(dlist_pop_back(dl), (void *)2);
    KT_CHECK_EQ(dlist_pop_back(dl), (void *)1);
    KT_CHECK_EQ(dlist_get_length(dl), (size_t)0);

    dlist_destroy(dl);
}

KTEST("dlist-pop-back-lifo", "dlist",
      "push_back + pop_back produces LIFO order",
      KT_FLAG_NONE, test_dlist_pop_back_lifo);

/* ------------------------------------------------------------------ */

static void test_dlist_push_front_pop_front(ktest_ctx_t *ctx) {
    dlist_t *dl = dlist_create();
    KT_ASSERT_NONNULL(dl);

    dlist_push_front(dl, (void *)0xA);
    dlist_push_front(dl, (void *)0xB);
    dlist_push_front(dl, (void *)0xC);

    /* push_front + pop_front = LIFO */
    KT_CHECK_EQ(dlist_pop_front(dl), (void *)0xC);
    KT_CHECK_EQ(dlist_pop_front(dl), (void *)0xB);
    KT_CHECK_EQ(dlist_pop_front(dl), (void *)0xA);

    dlist_destroy(dl);
}

KTEST("dlist-push-front-pop-front", "dlist",
      "push_front + pop_front produces LIFO (stack) order",
      KT_FLAG_NONE, test_dlist_push_front_pop_front);

/* ================================================================== */
/* Group 4: dlist_get by index                                          */
/* ================================================================== */

static void test_dlist_get_index(ktest_ctx_t *ctx) {
    dlist_t *dl = dlist_create();
    KT_ASSERT_NONNULL(dl);

    for (int i = 0; i < 8; i++)
        dlist_push_back(dl, (void *)(uintptr_t)(i * 11));

    for (int i = 0; i < 8; i++) {
        void *v = dlist_get(dl, (size_t)i);
        KT_CHECK_EQ(v, (void *)(uintptr_t)(i * 11));
    }

    dlist_destroy(dl);
}

KTEST("dlist-get-index", "dlist",
      "dlist_get returns the correct value for each index 0..7",
      KT_FLAG_CRITICAL, test_dlist_get_index);

/* ================================================================== */
/* Group 5: dlist_get_length                                            */
/* ================================================================== */

static void test_dlist_length_after_ops(ktest_ctx_t *ctx) {
    dlist_t *dl = dlist_create();
    KT_ASSERT_NONNULL(dl);

    KT_CHECK_EQ(dlist_get_length(dl), (size_t)0);

    dlist_push_back(dl, (void *)1);
    KT_CHECK_EQ(dlist_get_length(dl), (size_t)1);

    dlist_push_back(dl, (void *)2);
    dlist_push_front(dl, (void *)0);
    KT_CHECK_EQ(dlist_get_length(dl), (size_t)3);

    dlist_pop_front(dl);
    KT_CHECK_EQ(dlist_get_length(dl), (size_t)2);

    dlist_pop_back(dl);
    KT_CHECK_EQ(dlist_get_length(dl), (size_t)1);

    dlist_pop_front(dl);
    KT_CHECK_EQ(dlist_get_length(dl), (size_t)0);

    dlist_destroy(dl);
}

KTEST("dlist-length-after-ops", "dlist",
      "dlist_get_length tracks length correctly across push/pop ops",
      KT_FLAG_CRITICAL, test_dlist_length_after_ops);

/* ================================================================== */
/* Group 6: dlist_remove                                                */
/* ================================================================== */

static void test_dlist_remove_middle(ktest_ctx_t *ctx) {
    dlist_t *dl = dlist_create();
    KT_ASSERT_NONNULL(dl);

    dlist_push_back(dl, (void *)1);
    dlist_push_back(dl, (void *)2);
    dlist_push_back(dl, (void *)3);

    dlist_remove(dl, (void *)2);

    KT_CHECK_EQ(dlist_get_length(dl), (size_t)2);
    KT_CHECK_EQ(dlist_get(dl, 0), (void *)1);
    KT_CHECK_EQ(dlist_get(dl, 1), (void *)3);

    dlist_destroy(dl);
}

KTEST("dlist-remove-middle", "dlist",
      "dlist_remove eliminates the target element and preserves order",
      KT_FLAG_CRITICAL, test_dlist_remove_middle);

/* ------------------------------------------------------------------ */

static void test_dlist_remove_head(ktest_ctx_t *ctx) {
    dlist_t *dl = dlist_create();
    KT_ASSERT_NONNULL(dl);

    dlist_push_back(dl, (void *)10);
    dlist_push_back(dl, (void *)20);
    dlist_push_back(dl, (void *)30);

    dlist_remove(dl, (void *)10);

    KT_CHECK_EQ(dlist_get_length(dl), (size_t)2);
    KT_CHECK_EQ(dlist_get(dl, 0), (void *)20);
    KT_CHECK_EQ(dlist_get(dl, 1), (void *)30);

    dlist_destroy(dl);
}

KTEST("dlist-remove-head", "dlist",
      "dlist_remove of the head element leaves remaining nodes intact",
      KT_FLAG_NONE, test_dlist_remove_head);

/* ------------------------------------------------------------------ */

static void test_dlist_remove_tail(ktest_ctx_t *ctx) {
    dlist_t *dl = dlist_create();
    KT_ASSERT_NONNULL(dl);

    dlist_push_back(dl, (void *)100);
    dlist_push_back(dl, (void *)200);
    dlist_push_back(dl, (void *)300);

    dlist_remove(dl, (void *)300);

    KT_CHECK_EQ(dlist_get_length(dl), (size_t)2);
    KT_CHECK_EQ(dlist_get(dl, 0), (void *)100);
    KT_CHECK_EQ(dlist_get(dl, 1), (void *)200);

    dlist_destroy(dl);
}

KTEST("dlist-remove-tail", "dlist",
      "dlist_remove of the tail element leaves remaining nodes intact",
      KT_FLAG_NONE, test_dlist_remove_tail);

/* ================================================================== */
/* Group 7: destroy with contents                                       */
/* ================================================================== */

static void test_dlist_destroy_with_elements(ktest_ctx_t *ctx) {
    dlist_t *dl = dlist_create();
    KT_ASSERT_NONNULL(dl);

    for (int i = 0; i < 10; i++)
        dlist_push_back(dl, (void *)(uintptr_t)i);

    dlist_destroy(dl);  /* must free wrapper nodes, not crash */
    KT_CHECK(true);
}

KTEST("dlist-destroy-with-elements", "dlist",
      "dlist_destroy with 10 elements does not crash",
      KT_FLAG_CRITICAL, test_dlist_destroy_with_elements);

/* ================================================================== */
/* Group 8: dlist_push alias                                            */
/* ================================================================== */

static void test_dlist_push_alias(ktest_ctx_t *ctx) {
    dlist_t *dl = dlist_create();
    KT_ASSERT_NONNULL(dl);

    dlist_push(dl, (void *)42);
    KT_CHECK_EQ(dlist_get_length(dl), (size_t)1);
    KT_CHECK_EQ(dlist_pop(dl), (void *)42);
    KT_CHECK_EQ(dlist_get_length(dl), (size_t)0);

    dlist_destroy(dl);
}

KTEST("dlist-push-alias", "dlist",
      "dlist_push / dlist_pop aliases behave identically to push_back / pop_back",
      KT_FLAG_NONE, test_dlist_push_alias);

#endif /* KTEST_ENABLED */
