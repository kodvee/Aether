#ifdef KTEST_ENABLED

#include <kernel/ktest.h>
#include <kernel/hpet.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ================================================================== */
/* Group 1: HPET presence                                               */
/* ================================================================== */

static void test_hpet_initialized(ktest_ctx_t *ctx) {
    if (!hpet_initialized) {
        ctx->result = KT_SKIP;
        return;
    }
    KT_CHECK(hpet_initialized);
}

KTEST("hpet-initialized", "hpet",
      "hpet_initialized is true after kernel init (skip if no HPET)",
      KT_FLAG_NONE, test_hpet_initialized);

/* ================================================================== */
/* Group 2: tick period                                                  */
/* ================================================================== */

static void test_hpet_tick_period_nonzero(ktest_ctx_t *ctx) {
    if (!hpet_initialized) { ctx->result = KT_SKIP; return; }
    KT_CHECK(hpetTickPeriod != 0u);
}

KTEST("hpet-tick-period-nonzero", "hpet",
      "hpetTickPeriod is non-zero when HPET is present",
      KT_FLAG_NONE, test_hpet_tick_period_nonzero);

/* ------------------------------------------------------------------ */

static void test_hpet_tick_period_range(ktest_ctx_t *ctx) {
    if (!hpet_initialized) { ctx->result = KT_SKIP; return; }

    /*
     * HPET spec: minimum tick period 100 ns (10^8 fs), maximum 100 ns
     * (some platforms use 14 ns / ~14318180 fs).  Real periods fall
     * between 69 ns (HPET min) and 100 ns.  We accept anything <= 200 ns
     * (200,000,000 fs) to be permissive.
     */
    KT_CHECK(hpetTickPeriod <= 200000000u);
}

KTEST("hpet-tick-period-range", "hpet",
      "hpetTickPeriod is <= 200 ns (200,000,000 fs) — plausible hardware value",
      KT_FLAG_NONE, test_hpet_tick_period_range);

/* ================================================================== */
/* Group 3: counter advances                                             */
/* ================================================================== */

static void test_hpet_counter_nonzero(ktest_ctx_t *ctx) {
    if (!hpet_initialized) { ctx->result = KT_SKIP; return; }
    uint64_t count = hpet_get_count();
    KT_CHECK(count != 0u);
}

KTEST("hpet-counter-nonzero", "hpet",
      "hpet_get_count returns a non-zero value",
      KT_FLAG_NONE, test_hpet_counter_nonzero);

/* ------------------------------------------------------------------ */

static void test_hpet_counter_monotone(ktest_ctx_t *ctx) {
    if (!hpet_initialized) { ctx->result = KT_SKIP; return; }

    uint64_t t1 = hpet_get_count();
    /* Spin briefly to ensure the counter advances */
    for (volatile int i = 0; i < 10000; i++) {}
    uint64_t t2 = hpet_get_count();

    KT_CHECK(t2 > t1);
}

KTEST("hpet-counter-monotone", "hpet",
      "hpet_get_count advances monotonically across a short busy-wait",
      KT_FLAG_NONE, test_hpet_counter_monotone);

/* ================================================================== */
/* Group 4: hpet_timer_since                                            */
/* ================================================================== */

static void test_hpet_timer_since_zero_at_reset(ktest_ctx_t *ctx) {
    if (!hpet_initialized) { ctx->result = KT_SKIP; return; }

    hpet_reset_counter();
    uint64_t elapsed = hpet_timer_since();
    /* Immediately after reset the elapsed time should be very small.
     * We accept up to 1 ms (1,000,000 ns) to be generous. */
    KT_CHECK(elapsed < 1000000u);
}

KTEST("hpet-timer-since-near-zero", "hpet",
      "hpet_timer_since immediately after reset returns < 1 ms",
      KT_FLAG_NONE, test_hpet_timer_since_zero_at_reset);

/* ------------------------------------------------------------------ */

static void test_hpet_timer_since_advances(ktest_ctx_t *ctx) {
    if (!hpet_initialized) { ctx->result = KT_SKIP; return; }

    hpet_reset_counter();

    /* Busy-wait: enough iterations to guarantee > 0 ns elapsed */
    for (volatile int i = 0; i < 100000; i++) {}

    uint64_t elapsed = hpet_timer_since();
    KT_CHECK(elapsed > 0u);
}

KTEST("hpet-timer-since-advances", "hpet",
      "hpet_timer_since returns a positive elapsed time after a busy-wait",
      KT_FLAG_NONE, test_hpet_timer_since_advances);

/* ================================================================== */
/* Group 5: hpetAddress set                                             */
/* ================================================================== */

static void test_hpet_address_nonzero(ktest_ctx_t *ctx) {
    if (!hpet_initialized) { ctx->result = KT_SKIP; return; }
    KT_CHECK(hpetAddress != 0u);
}

KTEST("hpet-address-nonzero", "hpet",
      "hpetAddress is non-zero when HPET is initialised",
      KT_FLAG_NONE, test_hpet_address_nonzero);

#endif /* KTEST_ENABLED */
