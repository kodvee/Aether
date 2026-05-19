#ifdef KTEST_ENABLED

#include <kernel/ktest.h>
#include <kernel/apic.h>
#include <kernel/int.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* lapic_initialized is set by lapic_init() */
extern bool lapic_initialized;

/* coreCount is set by smp_init() */
extern uint64_t coreCount;

/* ------------------------------------------------------------------ */

static void test_smp_lapic_initialized(ktest_ctx_t *ctx) {
    KT_CHECK(lapic_initialized);
}

KTEST("smp-lapic-init", "smp",
      "LAPIC is marked as initialized after smp_init",
      KT_FLAG_CRITICAL | KT_FLAG_SMP, test_smp_lapic_initialized);

/* ------------------------------------------------------------------ */

static void test_smp_core_count(ktest_ctx_t *ctx) {
    /* Must have at least 1 core (the BSP) */
    KT_CHECK(coreCount >= 1);
}

KTEST("smp-core-count", "smp",
      "coreCount is at least 1 after SMP init",
      KT_FLAG_CRITICAL | KT_FLAG_SMP, test_smp_core_count);

/* ------------------------------------------------------------------ */

static void test_smp_irq_array(ktest_ctx_t *ctx) {
    /* The IRQ dispatch array must be non-NULL after idt_init */
    KT_ASSERT_NONNULL(irqs);

    /* irqs[0] (vector 32) is the LAPIC timer - check it is installed */
    KT_CHECK(irqs[0] != NULL);

    /* High-numbered slots (well above any system handler) must be NULL */
    KT_CHECK_EQ((uintptr_t)irqs[100], (uintptr_t)NULL);
    KT_CHECK_EQ((uintptr_t)irqs[IRQ_COUNT - 1], (uintptr_t)NULL);
}

KTEST("smp-irq-array", "smp",
      "irqs[] array is allocated and uninstalled slots are NULL",
      KT_FLAG_CRITICAL | KT_FLAG_SMP, test_smp_irq_array);

/* ------------------------------------------------------------------ */

static volatile uint32_t g_ipi_counter = 0;

static struct regs *_test_ipi_handler(struct regs *r) {
    __atomic_fetch_add(&g_ipi_counter, 1u, __ATOMIC_SEQ_CST);
    return r;
}

static void test_smp_irq_install(ktest_ctx_t *ctx) {
    /* Allocate a dynamic vector and install a handler */
    uint8_t vec = idt_allocate();
    KT_ASSERT(vec >= 32 && vec < 255);

    irq_install(_test_ipi_handler, vec);
    KT_CHECK_EQ((uintptr_t)irqs[vec - 32], (uintptr_t)_test_ipi_handler);

    /* Clean up: remove the handler */
    irqs[vec - 32] = NULL;
}

KTEST("smp-irq-install", "smp",
      "idt_allocate + irq_install registers a handler in the dynamic IRQ table",
      KT_FLAG_SMP, test_smp_irq_install);

#endif /* KTEST_ENABLED */
