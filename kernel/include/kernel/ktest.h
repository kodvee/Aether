#pragma once

#ifdef KTEST_ENABLED

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ------------------------------------------------------------------ */
/* Test flags (bitmask)                                                 */
/* ------------------------------------------------------------------ */

#define KT_FLAG_NONE        0x00u
#define KT_FLAG_CRITICAL    0x01u   /* must pass; suite aborts on failure */
#define KT_FLAG_SMP         0x02u   /* exercises multi-core paths */
#define KT_FLAG_STRESS      0x04u   /* long-running / resource intensive */
#define KT_FLAG_DESTRUCTIVE 0x08u   /* may corrupt state; run last */
#define KT_FLAG_SLOW        0x10u   /* skip in fast-mode runs */
#define KT_FLAG_PANIC       0x20u   /* test expects kernel panic */
#define KT_FLAG_SKIP        0x40u   /* always skipped */

/* ------------------------------------------------------------------ */
/* Test result codes                                                    */
/* ------------------------------------------------------------------ */

typedef enum {
    KT_PASS = 0,
    KT_FAIL,
    KT_SKIP,
    KT_PANIC_EXPECTED,   /* panic was expected and matched */
    KT_PANIC_UNEXPECTED, /* panic happened but wasn't expected */
    KT_TIMEOUT,
} ktest_result_t;

/* ------------------------------------------------------------------ */
/* Test context (passed to every test function)                         */
/* ------------------------------------------------------------------ */

typedef struct ktest_ctx {
    const char     *name;
    const char     *subsystem;
    uint32_t        flags;

    /* Expected-panic recovery: set by KT_EXPECT_PANIC, cleared after match */
    volatile bool   expect_panic;
    const char     *expected_panic_msg;  /* substring to match, or NULL=any */

    /* Populated by panic hook if an expected panic fires */
    bool            panic_caught;
    char            caught_msg[128];

    /* setjmp buffer: saved by KT_EXPECT_PANIC, longjmp'd to by panic hook */
    uintptr_t       recovery_buf[8];     /* rbx rbp r12 r13 r14 r15 rsp rip */

    /* Stats for this test */
    uint32_t        checks_passed;
    uint32_t        checks_failed;
    ktest_result_t  result;
} ktest_ctx_t;

/* ------------------------------------------------------------------ */
/* Test entry (placed in .ktest linker section)                         */
/* ------------------------------------------------------------------ */

typedef void (*ktest_fn_t)(ktest_ctx_t *ctx);

typedef struct {
    const char  *name;
    const char  *subsystem;
    const char  *description;
    uint32_t     flags;
    ktest_fn_t   fn;
} ktest_entry_t;

/* ------------------------------------------------------------------ */
/* Registration macro                                                   */
/* ------------------------------------------------------------------ */

#define KTEST(test_name, test_subsys, test_desc, test_flags, test_fn)    \
    static const ktest_entry_t _ktest_entry_##test_fn                    \
        __attribute__((used, section(".ktest"), aligned(8))) = {         \
        .name        = (test_name),                                      \
        .subsystem   = (test_subsys),                                    \
        .description = (test_desc),                                      \
        .flags       = (test_flags),                                     \
        .fn          = (test_fn),                                        \
    }

/* ------------------------------------------------------------------ */
/* Runner entry points                                                  */
/* ------------------------------------------------------------------ */

/*
 * ktest_run - discover and run all registered tests.
 * Called from kernel.c after full init.  Returns the number of failures
 * (0 = all passed).  On headless / CI builds also writes QEMU exit code.
 */
int ktest_run(void);

/* ------------------------------------------------------------------ */
/* Assertion helpers (use inside test functions)                        */
/* ------------------------------------------------------------------ */

/* Internal: record a single check result */
void _kt_record(ktest_ctx_t *ctx, bool ok, const char *expr,
                const char *file, int line);

#define KT_ASSERT(expr) \
    do { \
        bool _ok = !!(expr); \
        _kt_record(ctx, _ok, #expr, __FILE__, __LINE__); \
        if (!_ok) { ctx->result = KT_FAIL; return; } \
    } while (0)

#define KT_CHECK(expr) \
    do { \
        bool _ok = !!(expr); \
        _kt_record(ctx, _ok, #expr, __FILE__, __LINE__); \
    } while (0)

#define KT_ASSERT_EQ(a, b) \
    KT_ASSERT((a) == (b))

#define KT_CHECK_EQ(a, b) \
    KT_CHECK((a) == (b))

#define KT_ASSERT_NE(a, b) \
    KT_ASSERT((a) != (b))

#define KT_CHECK_NE(a, b) \
    KT_CHECK((a) != (b))

#define KT_ASSERT_NULL(p) \
    KT_ASSERT((p) == NULL)

#define KT_ASSERT_NONNULL(p) \
    KT_ASSERT((p) != NULL)

/*
 * KT_EXPECT_PANIC - assert that the block of code triggers a kernel panic.
 *
 * Usage:
 *   KT_EXPECT_PANIC(ctx, "divide-by-zero") {
 *       int x = 1 / 0;
 *   }
 *
 * The macro saves a setjmp checkpoint.  If a panic fires and the panic hook
 * is compiled in (KTEST_ENABLED), the panic hook calls longjmp back here.
 * If the block completes without a panic, the test fails.
 *
 * msg_substr: substring the panic message must contain (NULL = accept any).
 */
#define KT_EXPECT_PANIC(ctx_ptr, msg_substr) \
    (ctx_ptr)->expected_panic_msg = (msg_substr); \
    (ctx_ptr)->panic_caught       = false; \
    (ctx_ptr)->expect_panic       = true; \
    if (__builtin_setjmp((ctx_ptr)->recovery_buf) == 0)

/* Called after the KT_EXPECT_PANIC block to verify the panic was caught */
#define KT_EXPECT_PANIC_VERIFY(ctx_ptr) \
    do { \
        (ctx_ptr)->expect_panic = false; \
        if (!(ctx_ptr)->panic_caught) { \
            _kt_record((ctx_ptr), false, \
                       "expected panic did not fire", __FILE__, __LINE__); \
            (ctx_ptr)->result = KT_FAIL; \
            return; \
        } \
        _kt_record((ctx_ptr), true, "expected panic caught", __FILE__, __LINE__); \
    } while (0)

/* ------------------------------------------------------------------ */
/* Global state accessed by panic hook (in panic.c)                     */
/* ------------------------------------------------------------------ */

extern volatile ktest_ctx_t *g_ktest_active_ctx;

#endif /* KTEST_ENABLED */
