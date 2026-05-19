#ifdef KTEST_ENABLED

#include <kernel/ktest.h>
#include <kernel/list.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ================================================================== */
/* Test payload struct with embedded list node                          */
/* ================================================================== */

typedef struct {
    int         value;
    list_node_t node;
} item_t;

/* ================================================================== */
/* Group 1: list_head_t initialization                                  */
/* ================================================================== */

static void test_list_head_init_empty(ktest_ctx_t *ctx) {
    list_head_t h;
    list_head_init(&h);
    KT_CHECK(list_empty(&h));
}

KTEST("list-head-init-empty", "list",
      "list_head_init produces an empty list",
      KT_FLAG_CRITICAL, test_list_head_init_empty);

/* ------------------------------------------------------------------ */

static void test_list_head_static_empty(ktest_ctx_t *ctx) {
    list_head_t h = LIST_HEAD_INIT(h);
    KT_CHECK(list_empty(&h));
}

KTEST("list-head-static-empty", "list",
      "LIST_HEAD_INIT macro produces an empty list",
      KT_FLAG_CRITICAL, test_list_head_static_empty);

/* ------------------------------------------------------------------ */

static void test_list_head_sentinel(ktest_ctx_t *ctx) {
    list_head_t h;
    list_head_init(&h);
    /* Circular sentinel: next and prev must both point to self */
    KT_CHECK_EQ(h.next, &h);
    KT_CHECK_EQ(h.prev, &h);
}

KTEST("list-head-sentinel", "list",
      "empty list has next == prev == self (circular sentinel)",
      KT_FLAG_CRITICAL, test_list_head_sentinel);

/* ================================================================== */
/* Group 2: list_node_t initialization                                  */
/* ================================================================== */

static void test_list_node_init_detached(ktest_ctx_t *ctx) {
    list_node_t n;
    list_node_init(&n);
    KT_CHECK_EQ(n.next, &n);
    KT_CHECK_EQ(n.prev, &n);
}

KTEST("list-node-init-detached", "list",
      "list_node_init leaves the node pointing to itself",
      KT_FLAG_CRITICAL, test_list_node_init_detached);

/* ================================================================== */
/* Group 3: list_empty                                                  */
/* ================================================================== */

static void test_list_empty_on_init(ktest_ctx_t *ctx) {
    list_head_t h;
    list_head_init(&h);
    KT_CHECK(list_empty(&h));
}

KTEST("list-empty-on-init", "list",
      "newly initialised list is empty",
      KT_FLAG_CRITICAL, test_list_empty_on_init);

/* ------------------------------------------------------------------ */

static void test_list_nonempty_after_push(ktest_ctx_t *ctx) {
    list_head_t h;
    list_head_init(&h);

    item_t a = { .value = 1 };
    list_node_init(&a.node);
    list_push_back(&h, &a.node);

    KT_CHECK(!list_empty(&h));
}

KTEST("list-nonempty-after-push", "list",
      "list_empty returns false after list_push_back",
      KT_FLAG_CRITICAL, test_list_nonempty_after_push);

/* ================================================================== */
/* Group 4: push_front / push_back                                      */
/* ================================================================== */

static void test_list_push_back_order(ktest_ctx_t *ctx) {
    list_head_t h;
    list_head_init(&h);

    item_t a = { .value = 10 }; list_node_init(&a.node);
    item_t b = { .value = 20 }; list_node_init(&b.node);
    item_t c = { .value = 30 }; list_node_init(&c.node);

    list_push_back(&h, &a.node);
    list_push_back(&h, &b.node);
    list_push_back(&h, &c.node);

    /* FIFO: pop_front should yield a, b, c */
    list_node_t *n1 = list_pop_front(&h);
    list_node_t *n2 = list_pop_front(&h);
    list_node_t *n3 = list_pop_front(&h);

    KT_ASSERT(n1 != NULL);
    KT_ASSERT(n2 != NULL);
    KT_ASSERT(n3 != NULL);

    KT_CHECK_EQ(list_entry(n1, item_t, node)->value, 10);
    KT_CHECK_EQ(list_entry(n2, item_t, node)->value, 20);
    KT_CHECK_EQ(list_entry(n3, item_t, node)->value, 30);
    KT_CHECK(list_empty(&h));
}

KTEST("list-push-back-order", "list",
      "list_push_back maintains FIFO order when popped front",
      KT_FLAG_CRITICAL, test_list_push_back_order);

/* ------------------------------------------------------------------ */

static void test_list_push_front_order(ktest_ctx_t *ctx) {
    list_head_t h;
    list_head_init(&h);

    item_t a = { .value = 10 }; list_node_init(&a.node);
    item_t b = { .value = 20 }; list_node_init(&b.node);
    item_t c = { .value = 30 }; list_node_init(&c.node);

    list_push_front(&h, &a.node);
    list_push_front(&h, &b.node);
    list_push_front(&h, &c.node);

    /* LIFO (push_front -> pop_front): c, b, a */
    list_node_t *n1 = list_pop_front(&h);
    list_node_t *n2 = list_pop_front(&h);
    list_node_t *n3 = list_pop_front(&h);

    KT_ASSERT(n1 != NULL);
    KT_ASSERT(n2 != NULL);
    KT_ASSERT(n3 != NULL);

    KT_CHECK_EQ(list_entry(n1, item_t, node)->value, 30);
    KT_CHECK_EQ(list_entry(n2, item_t, node)->value, 20);
    KT_CHECK_EQ(list_entry(n3, item_t, node)->value, 10);
    KT_CHECK(list_empty(&h));
}

KTEST("list-push-front-order", "list",
      "list_push_front reverses insertion order",
      KT_FLAG_NONE, test_list_push_front_order);

/* ================================================================== */
/* Group 5: list_pop_front                                              */
/* ================================================================== */

static void test_list_pop_front_empty(ktest_ctx_t *ctx) {
    list_head_t h;
    list_head_init(&h);
    list_node_t *n = list_pop_front(&h);
    KT_CHECK(n == NULL);
    KT_CHECK(list_empty(&h));
}

KTEST("list-pop-front-empty", "list",
      "list_pop_front on empty list returns NULL and list stays empty",
      KT_FLAG_CRITICAL, test_list_pop_front_empty);

/* ------------------------------------------------------------------ */

static void test_list_pop_front_detaches(ktest_ctx_t *ctx) {
    list_head_t h;
    list_head_init(&h);

    item_t a = { .value = 5 }; list_node_init(&a.node);
    list_push_back(&h, &a.node);

    list_node_t *popped = list_pop_front(&h);
    KT_ASSERT(popped != NULL);

    /* Popped node must be detached (self-pointing) */
    KT_CHECK_EQ(popped->next, popped);
    KT_CHECK_EQ(popped->prev, popped);
    KT_CHECK(list_empty(&h));
}

KTEST("list-pop-front-detaches", "list",
      "popped node is in detached (self-pointing) state afterwards",
      KT_FLAG_NONE, test_list_pop_front_detaches);

/* ================================================================== */
/* Group 6: list_remove                                                 */
/* ================================================================== */

static void test_list_remove_middle(ktest_ctx_t *ctx) {
    list_head_t h;
    list_head_init(&h);

    item_t a = { .value = 1 }; list_node_init(&a.node);
    item_t b = { .value = 2 }; list_node_init(&b.node);
    item_t c = { .value = 3 }; list_node_init(&c.node);

    list_push_back(&h, &a.node);
    list_push_back(&h, &b.node);
    list_push_back(&h, &c.node);

    /* Remove b from the middle */
    list_remove(&b.node);

    list_node_t *n1 = list_pop_front(&h);
    list_node_t *n2 = list_pop_front(&h);
    list_node_t *n3 = list_pop_front(&h);

    KT_ASSERT(n1 != NULL);
    KT_ASSERT(n2 != NULL);
    KT_CHECK(n3 == NULL);  /* only 2 items remain */

    KT_CHECK_EQ(list_entry(n1, item_t, node)->value, 1);
    KT_CHECK_EQ(list_entry(n2, item_t, node)->value, 3);
}

KTEST("list-remove-middle", "list",
      "list_remove from middle leaves the other nodes intact",
      KT_FLAG_CRITICAL, test_list_remove_middle);

/* ------------------------------------------------------------------ */

static void test_list_remove_only_element(ktest_ctx_t *ctx) {
    list_head_t h;
    list_head_init(&h);

    item_t a = { .value = 99 }; list_node_init(&a.node);
    list_push_back(&h, &a.node);
    list_remove(&a.node);

    KT_CHECK(list_empty(&h));
    /* node must be detached */
    KT_CHECK_EQ(a.node.next, &a.node);
    KT_CHECK_EQ(a.node.prev, &a.node);
}

KTEST("list-remove-only-element", "list",
      "list_remove of the only element yields an empty list",
      KT_FLAG_NONE, test_list_remove_only_element);

/* ================================================================== */
/* Group 7: list_entry                                                  */
/* ================================================================== */

static void test_list_entry_recovery(ktest_ctx_t *ctx) {
    item_t a = { .value = 42 };
    list_node_init(&a.node);

    item_t *recovered = list_entry(&a.node, item_t, node);
    KT_CHECK(recovered == &a);
    KT_CHECK_EQ(recovered->value, 42);
}

KTEST("list-entry-recovery", "list",
      "list_entry recovers the correct containing struct pointer",
      KT_FLAG_CRITICAL, test_list_entry_recovery);

/* ================================================================== */
/* Group 8: list_for_each                                               */
/* ================================================================== */

static void test_list_for_each_count(ktest_ctx_t *ctx) {
    list_head_t h;
    list_head_init(&h);

    item_t items[5];
    for (int i = 0; i < 5; i++) {
        items[i].value = i;
        list_node_init(&items[i].node);
        list_push_back(&h, &items[i].node);
    }

    int count = 0;
    list_node_t *pos;
    list_for_each(pos, &h) {
        count++;
    }
    KT_CHECK_EQ(count, 5);
}

KTEST("list-for-each-count", "list",
      "list_for_each visits exactly N nodes for an N-element list",
      KT_FLAG_CRITICAL, test_list_for_each_count);

/* ------------------------------------------------------------------ */

static void test_list_for_each_values(ktest_ctx_t *ctx) {
    list_head_t h;
    list_head_init(&h);

    item_t items[4];
    for (int i = 0; i < 4; i++) {
        items[i].value = i * 10;
        list_node_init(&items[i].node);
        list_push_back(&h, &items[i].node);
    }

    int idx = 0;
    list_node_t *pos;
    list_for_each(pos, &h) {
        item_t *it = list_entry(pos, item_t, node);
        KT_CHECK_EQ(it->value, idx * 10);
        idx++;
    }
    KT_CHECK_EQ(idx, 4);
}

KTEST("list-for-each-values", "list",
      "list_for_each iterates nodes in insertion order with correct values",
      KT_FLAG_NONE, test_list_for_each_values);

/* ================================================================== */
/* Group 9: list_for_each_safe                                          */
/* ================================================================== */

static void test_list_for_each_safe_remove(ktest_ctx_t *ctx) {
    list_head_t h;
    list_head_init(&h);

    item_t items[5];
    for (int i = 0; i < 5; i++) {
        items[i].value = i;
        list_node_init(&items[i].node);
        list_push_back(&h, &items[i].node);
    }

    /* Remove all even-valued items while iterating */
    list_node_t *pos, *tmp;
    list_for_each_safe(pos, tmp, &h) {
        item_t *it = list_entry(pos, item_t, node);
        if (it->value % 2 == 0)
            list_remove(pos);
    }

    /* Only odd-valued items should remain: 1, 3 */
    int count = 0;
    list_for_each(pos, &h) {
        item_t *it = list_entry(pos, item_t, node);
        KT_CHECK(it->value % 2 != 0);
        count++;
    }
    KT_CHECK_EQ(count, 2);
}

KTEST("list-for-each-safe-remove", "list",
      "list_for_each_safe survives removal of the current node",
      KT_FLAG_CRITICAL, test_list_for_each_safe_remove);

/* ================================================================== */
/* Group 10: round-trip push/pop                                        */
/* ================================================================== */

static void test_list_roundtrip(ktest_ctx_t *ctx) {
    list_head_t h;
    list_head_init(&h);

    #define N 16
    item_t items[N];
    for (int i = 0; i < N; i++) {
        items[i].value = i;
        list_node_init(&items[i].node);
        list_push_back(&h, &items[i].node);
    }

    for (int i = 0; i < N; i++) {
        list_node_t *n = list_pop_front(&h);
        KT_ASSERT(n != NULL);
        KT_CHECK_EQ(list_entry(n, item_t, node)->value, i);
    }

    KT_CHECK(list_empty(&h));
    KT_CHECK(list_pop_front(&h) == NULL);
    #undef N
}

KTEST("list-roundtrip", "list",
      "push/pop 16 items in FIFO order, list empty afterwards",
      KT_FLAG_NONE, test_list_roundtrip);

#endif /* KTEST_ENABLED */
