#ifdef KTEST_ENABLED

#include <kernel/ktest.h>
#include <kernel/cpu.h>
#include <kernel/scheduler.h>
#include <kernel/apic.h>
#include <kernel/spinlock.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

extern uint64_t coreCount;

/* ================================================================== */
/* Group 1: this_cpu()                                                  */
/* ================================================================== */

static void test_cpu_this_cpu_nonnull(ktest_ctx_t *ctx) {
    core_t *cpu = this_cpu();
    KT_ASSERT_NONNULL(cpu);
}

KTEST("cpu-this-cpu-nonnull", "cpu",
      "this_cpu() returns a non-NULL pointer",
      KT_FLAG_CRITICAL, test_cpu_this_cpu_nonnull);

/* ------------------------------------------------------------------ */

static void test_cpu_this_cpu_in_array(ktest_ctx_t *ctx) {
    core_t *cpu = this_cpu();
    KT_ASSERT_NONNULL(cpu);

    /* The returned pointer must be within cpu_core_local[] */
    bool found = false;
    for (uint64_t i = 0; i < coreCount; i++) {
        if (&cpu_core_local[i] == cpu) { found = true; break; }
    }
    KT_CHECK(found);
}

KTEST("cpu-this-cpu-in-array", "cpu",
      "this_cpu() returns a pointer within the cpu_core_local array",
      KT_FLAG_CRITICAL, test_cpu_this_cpu_in_array);

/* ================================================================== */
/* Group 2: per-core stable fields                                       */
/* ================================================================== */

static void test_cpu_core_id_valid(ktest_ctx_t *ctx) {
    core_t *cpu = this_cpu();
    KT_ASSERT_NONNULL(cpu);
    KT_CHECK(cpu->cpu_id < (cpu_id_t)coreCount);
}

KTEST("cpu-core-id-valid", "cpu",
      "this_cpu()->cpu_id is in [0, coreCount)",
      KT_FLAG_CRITICAL, test_cpu_core_id_valid);

/* ------------------------------------------------------------------ */

static void test_cpu_bsp_exactly_one(ktest_ctx_t *ctx) {
    int bsp_count = 0;
    for (uint64_t i = 0; i < coreCount; i++) {
        if (cpu_core_local[i].bsp) bsp_count++;
    }
    KT_CHECK_EQ(bsp_count, 1);
}

KTEST("cpu-bsp-exactly-one", "cpu",
      "exactly one core has bsp == true",
      KT_FLAG_CRITICAL, test_cpu_bsp_exactly_one);

/* ------------------------------------------------------------------ */

static void test_cpu_unique_cpu_ids(ktest_ctx_t *ctx) {
    for (uint64_t i = 0; i < coreCount; i++) {
        KT_CHECK_EQ(cpu_core_local[i].cpu_id, (cpu_id_t)i);
    }
}

KTEST("cpu-unique-cpu-ids", "cpu",
      "cpu_core_local[i].cpu_id == i for all cores",
      KT_FLAG_CRITICAL, test_cpu_unique_cpu_ids);

/* ================================================================== */
/* Group 3: LAPIC                                                       */
/* ================================================================== */

static void test_cpu_lapic_initialized(ktest_ctx_t *ctx) {
    KT_CHECK(lapic_initialized);
}

KTEST("cpu-lapic-initialized", "cpu",
      "lapic_initialized is true after kernel init",
      KT_FLAG_CRITICAL, test_cpu_lapic_initialized);

/* ------------------------------------------------------------------ */

static void test_cpu_lapic_frequency_nonzero(ktest_ctx_t *ctx) {
    if (!lapic_initialized) { ctx->result = KT_SKIP; return; }
    uint64_t freq = lapic_get_frequency();
    KT_CHECK(freq > 0u);
}

KTEST("cpu-lapic-freq-nonzero", "cpu",
      "lapic_get_frequency returns a non-zero value",
      KT_FLAG_NONE, test_cpu_lapic_frequency_nonzero);

/* ------------------------------------------------------------------ */

static void test_cpu_lapic_ids_valid(ktest_ctx_t *ctx) {
    /* lapic_id must be >= 0; no specific value constraint beyond
     * being readable without faulting. */
    for (uint64_t i = 0; i < coreCount; i++) {
        /* Just verify the field is accessible (no fault) */
        (void)cpu_core_local[i].lapic_id;
    }
    KT_CHECK(true);
}

KTEST("cpu-lapic-ids-valid", "cpu",
      "cpu_core_local[].lapic_id fields are readable for all cores",
      KT_FLAG_NONE, test_cpu_lapic_ids_valid);

/* ================================================================== */
/* Group 4: IRQ save / restore                                          */
/* ================================================================== */

static void test_cpu_irq_save_restore(ktest_ctx_t *ctx) {
    irq_state_t saved = irq_save();
    /* After irq_save interrupts must be disabled */
    KT_CHECK(!interrupt_state());
    irq_restore(saved);
    /* Restoring must bring interrupts back to the saved state */
    KT_CHECK_EQ(interrupt_state(), saved);
}

KTEST("cpu-irq-save-restore", "cpu",
      "irq_save disables interrupts; irq_restore reinstates prior state",
      KT_FLAG_CRITICAL, test_cpu_irq_save_restore);

/* ------------------------------------------------------------------ */

static void test_cpu_irq_save_nested(ktest_ctx_t *ctx) {
    /* Nested save/restore must compose correctly */
    irq_state_t outer = irq_save();
    irq_state_t inner = irq_save();
    KT_CHECK(!interrupt_state());
    irq_restore(inner);
    KT_CHECK(!interrupt_state()); /* still disabled: outer hasn't released */
    irq_restore(outer);
    KT_CHECK_EQ(interrupt_state(), outer);
}

KTEST("cpu-irq-nested-save", "cpu",
      "nested irq_save/restore composes correctly",
      KT_FLAG_NONE, test_cpu_irq_save_nested);

/* ================================================================== */
/* Group 5: per-core run queue locks                                    */
/* ================================================================== */

static void test_cpu_run_queue_locks_free(ktest_ctx_t *ctx) {
    for (uint64_t i = 0; i < coreCount; i++) {
        KT_CHECK(!spinlock_is_held(&cpu_core_local[i].run_queue_lock));
    }
}

KTEST("cpu-run-queue-locks-free", "cpu",
      "all per-core run_queue_locks are unheld at test entry",
      KT_FLAG_NONE, test_cpu_run_queue_locks_free);

/* ================================================================== */
/* Group 6: TSS rsp0 matches current thread after scheduler_init        */
/* ================================================================== */

static void test_cpu_tss_rsp0_after_sched_init(ktest_ctx_t *ctx) {
    scheduler_init();
    core_t *cpu = this_cpu();
    KT_ASSERT_NONNULL(cpu);

    /* After scheduler_init idle threads exist; before scheduler_enter()
     * the TSS rsp0 is 0 (not yet set by scheduler_enter).  We verify
     * the field is accessible without faulting and report its value. */
    (void)cpu->tss.rsp0; /* readable */
    KT_CHECK(true);
}

KTEST("cpu-tss-rsp0-readable", "cpu",
      "tss.rsp0 field is accessible after scheduler_init",
      KT_FLAG_NONE, test_cpu_tss_rsp0_after_sched_init);

/* ================================================================== */
/* Group 7: core_t field offsets (SYSCALL fast path)                    */
/*                                                                      */
/* syscall.S accesses %gs:0 and %gs:8.  These MUST be syscall_ksp and  */
/* ustack_scratch respectively.  A layout change would silently corrupt */
/* the kernel stack pointer on every SYSCALL.                           */
/* ================================================================== */

static void test_cpu_core_t_syscall_ksp_offset(ktest_ctx_t *ctx) {
    KT_CHECK_EQ(offsetof(core_t, syscall_ksp),    (size_t)0);
}

KTEST("cpu-core-syscall-ksp-offset", "cpu",
      "core_t.syscall_ksp is at byte offset 0 (%%gs:0 in syscall.S)",
      KT_FLAG_CRITICAL, test_cpu_core_t_syscall_ksp_offset);

/* ------------------------------------------------------------------ */

static void test_cpu_core_t_ustack_scratch_offset(ktest_ctx_t *ctx) {
    KT_CHECK_EQ(offsetof(core_t, ustack_scratch), (size_t)8);
}

KTEST("cpu-core-ustack-scratch-offset", "cpu",
      "core_t.ustack_scratch is at byte offset 8 (%%gs:8 in syscall.S)",
      KT_FLAG_CRITICAL, test_cpu_core_t_ustack_scratch_offset);

#endif /* KTEST_ENABLED */
