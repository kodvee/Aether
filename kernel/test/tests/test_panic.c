#ifdef KTEST_ENABLED

#include <kernel/ktest.h>
#include <kernel/panic.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ------------------------------------------------------------------ */
/* Expected-panic tests                                                 */
/*                                                                      */
/* These tests intentionally trigger kernel panics and verify that the  */
/* panic hook in panic.c correctly intercepts them.                     */
/* ------------------------------------------------------------------ */

static void test_panic_generic(ktest_ctx_t *ctx) {
    KT_EXPECT_PANIC(ctx, "test-expected") {
        SUBSYS_PANIC("ktest", "test-expected panic trigger");
    }
    KT_EXPECT_PANIC_VERIFY(ctx);
}

KTEST("panic-expected-generic", "panic",
      "SUBSYS_PANIC is caught by KT_EXPECT_PANIC and does not halt the system",
      KT_FLAG_CRITICAL | KT_FLAG_PANIC, test_panic_generic);

/* ------------------------------------------------------------------ */

static void test_panic_message_match(ktest_ctx_t *ctx) {
    KT_EXPECT_PANIC(ctx, "needle-string") {
        SUBSYS_PANIC("ktest", "this message contains needle-string inside it");
    }
    KT_EXPECT_PANIC_VERIFY(ctx);

    /* Verify the caught message was recorded */
    KT_CHECK(ctx->caught_msg[0] != '\0');
}

KTEST("panic-message-match", "panic",
      "KT_EXPECT_PANIC matches on a message substring",
      KT_FLAG_PANIC, test_panic_message_match);

/* ------------------------------------------------------------------ */

static void test_panic_assert(ktest_ctx_t *ctx) {
    KT_EXPECT_PANIC(ctx, "assertion failed") {
        KERNEL_ASSERT(1 == 2);
    }
    KT_EXPECT_PANIC_VERIFY(ctx);
}

KTEST("panic-assert", "panic",
      "KERNEL_ASSERT(false) is caught by KT_EXPECT_PANIC",
      KT_FLAG_PANIC, test_panic_assert);

/* ------------------------------------------------------------------ */

static void test_panic_bug(ktest_ctx_t *ctx) {
    KT_EXPECT_PANIC(ctx, "BUG") {
        KERNEL_BUG("deliberate test bug");
    }
    KT_EXPECT_PANIC_VERIFY(ctx);
}

KTEST("panic-bug", "panic",
      "KERNEL_BUG is caught by KT_EXPECT_PANIC",
      KT_FLAG_PANIC, test_panic_bug);

/* ------------------------------------------------------------------ */

static void test_panic_sequential(ktest_ctx_t *ctx) {
    /* Two expected panics in the same test - panic state must reset between them */
    KT_EXPECT_PANIC(ctx, "first") {
        SUBSYS_PANIC("ktest", "first panic");
    }
    KT_EXPECT_PANIC_VERIFY(ctx);

    KT_EXPECT_PANIC(ctx, "second") {
        SUBSYS_PANIC("ktest", "second panic");
    }
    KT_EXPECT_PANIC_VERIFY(ctx);
}

KTEST("panic-sequential", "panic",
      "panic state resets correctly between two consecutive expected panics",
      KT_FLAG_PANIC, test_panic_sequential);

#endif /* KTEST_ENABLED */
