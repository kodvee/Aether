#ifdef KTEST_ENABLED

#include <kernel/ktest.h>
#include <kernel/spinlock.h>
#include <stdbool.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Basic acquire / release                                              */
/* ------------------------------------------------------------------ */

static void test_spinlock_basic(ktest_ctx_t *ctx) {
    spinlock_t lock = SPINLOCK_ZERO;

    /* acquire should succeed and return previous interrupt state */
    bool prev = spinlock_acquire(&lock);
    /* Lock is now held; release it */
    spinlock_release(&lock, prev);
    /* Should not crash or deadlock */
    KT_CHECK(true);
}

KTEST("spinlock-basic", "sync",
      "spinlock acquire/release completes without deadlock",
      KT_FLAG_CRITICAL, test_spinlock_basic);

/* ------------------------------------------------------------------ */

static void test_spinlock_reacquire(ktest_ctx_t *ctx) {
    spinlock_t lock = SPINLOCK_ZERO;

    /* Acquire, release, then re-acquire - should be idempotent */
    bool s1 = spinlock_acquire(&lock);
    spinlock_release(&lock, s1);

    bool s2 = spinlock_acquire(&lock);
    spinlock_release(&lock, s2);

    KT_CHECK(true);
}

KTEST("spinlock-reacquire", "sync",
      "spinlock can be acquired and released multiple times in sequence",
      KT_FLAG_NONE, test_spinlock_reacquire);

/* ------------------------------------------------------------------ */

static void test_spinlock_ticket_advance(ktest_ctx_t *ctx) {
    spinlock_t lock = SPINLOCK_ZERO;

    /* Acquire - the next_ticket should advance */
    uint16_t before_next = lock.next_ticket;
    bool s = spinlock_acquire(&lock);
    uint16_t after_next = lock.next_ticket;
    spinlock_release(&lock, s);

    KT_CHECK_EQ((int)(after_next - before_next), 1);
}

KTEST("spinlock-ticket-advance", "sync",
      "next_ticket increments by 1 on each acquire",
      KT_FLAG_NONE, test_spinlock_ticket_advance);

/* ------------------------------------------------------------------ */

static void test_spinlock_many(ktest_ctx_t *ctx) {
    spinlock_t lock = SPINLOCK_ZERO;
    volatile uint32_t counter = 0;

    /* Single-core: serialised increments, no real contention */
    for (int i = 0; i < 1000; i++) {
        bool s = spinlock_acquire(&lock);
        counter++;
        spinlock_release(&lock, s);
    }

    KT_CHECK_EQ((int)counter, 1000);
}

KTEST("spinlock-counter", "sync",
      "1000 acquire/increment/release cycles produce correct counter",
      KT_FLAG_NONE, test_spinlock_many);

/* ------------------------------------------------------------------ */

static void test_spinlock_independent(ktest_ctx_t *ctx) {
    /* Two independent locks should not interfere */
    spinlock_t a = SPINLOCK_ZERO;
    spinlock_t b = SPINLOCK_ZERO;

    bool sa = spinlock_acquire(&a);
    bool sb = spinlock_acquire(&b);
    spinlock_release(&b, sb);
    spinlock_release(&a, sa);

    KT_CHECK(true);
}

KTEST("spinlock-independent", "sync",
      "two independent spinlocks can be held simultaneously without conflict",
      KT_FLAG_NONE, test_spinlock_independent);

#endif /* KTEST_ENABLED */
