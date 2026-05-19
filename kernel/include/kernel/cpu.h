#pragma once

#include <stdint.h>
#include <kernel/mmu.h>
#include <stdbool.h>
#include <kernel/types.h>
#include <kernel/msr.h>
#include <kernel/panic.h>
#include <kernel/list.h>
#include <kernel/spinlock.h>
#include <kernel/gdt.h>

extern uint64_t coreCount;

/* Forward declaration - full definition in kernel/include/kernel/scheduler.h */
struct thread;

static inline uint64_t read_cr0(void) {
    uint64_t cr0_value;
    asm volatile("mov %%cr0, %0" : "=r" (cr0_value));
    return cr0_value;
}

static inline uint64_t read_cr2(void) {
    uint64_t cr2_value;
    asm volatile("mov %%cr2, %0" : "=r" (cr2_value));
    return cr2_value;
}

static inline uint64_t read_cr3(void) {
    uint64_t cr3_value;
    asm volatile("mov %%cr3, %0" : "=r" (cr3_value));
    return cr3_value;
}

static inline uint64_t read_cr4(void) {
    uint64_t cr4_value;
    asm volatile("mov %%cr4, %0" : "=r" (cr4_value));
    return cr4_value;
}

/*
 * core_t - per-CPU state structure.
 *
 * One instance per logical CPU, allocated by smp_init() in a contiguous
 * array (cpu_core_local[]).  The GS base register always points to the
 * current CPU's core_t.  Access via the this_cpu() helper.
 *
 * Ownership rules:
 *   - Fields are owned by the CPU they describe; cross-CPU reads are safe
 *     for stable fields (lapic_id, bsp).
 *   - current_thread and interrupt_depth must only be written by the
 *     owning CPU.  Another CPU may read current_thread under a lock.
 *
 * Scheduler note:
 *   current_thread is NULL while the CPU is idle.  The scheduler sets it
 *   on context switch in and clears it when the thread is descheduled.
 */
typedef struct core {
    /* Stable after init - safe to read from any CPU without a lock */
    uint32_t       lapic_id;       /* xAPIC ID of this CPU                */
    cpu_id_t       cpu_id;         /* logical CPU index (0-based)          */
    bool           bsp;            /* true only on the bootstrap processor */

    /* Modified only by the owning CPU */
    uint32_t       interrupt_depth; /* 0 = normal context, >0 = in IRQ handler */
    struct thread *current_thread;  /* thread currently executing, NULL = idle */

    /* Per-core run queue (owned by this CPU; see scheduler.c) */
    list_head_t    run_queue;       /* THREAD_READY threads for this core   */
    spinlock_t     run_queue_lock;  /* protects run_queue and state changes */

    /* Idle thread: never on any run queue; switched to explicitly when the
     * run queue is empty and no runnable thread exists.  Non-NULL after
     * scheduler_init() returns. */
    struct thread *idle_thread;

    /* Per-core GDT and TSS.  Filled and loaded by gdt_load_core() during
     * core_start().  tss.rsp0 is updated by schedule() on every context
     * switch so that ring-3 → ring-0 transitions land on the correct stack. */
    core_gdt_t     gdt;
    tss_t          tss;
} core_t;

/* Array of all per-CPU state structs, indexed by cpu_id.  Allocated by
 * smp_init(); never freed.  Valid after smp_init() returns. */
extern core_t *cpu_core_local;

typedef struct cpu_info {
	char* vendorId; /* Vendor, Ex: Intel, AMD, Qemu */
	char* cpuName; /* The whole model name */
	uint32_t coreCount; /* The number of cores on the CPU */
	uint64_t cpuFeatures;
} cpu_info_t;

extern uint32_t bsp_lapic_id;

/* cpu_info is populated by cpuinfo_init(); NULL before that call. */
extern cpu_info_t *cpu_info;
void cpuinfo_init(void);

static inline bool interrupt_state(void) {
    uint64_t flags;
    asm volatile ("pushfq; pop %0" : "=rm"(flags) :: "memory");
    return flags & (1 << 9);
}

static inline void enable_interrupts(void) {
    asm ("sti");
}

static inline void disable_interrupts(void) {
    asm ("cli");
}

static inline bool interrupt_toggle(bool state) {
    bool ret = interrupt_state();
    if (state) {
        enable_interrupts();
    } else {
        disable_interrupts();
    }
    return ret;
}

/**
 * Register layout for interrupt context.
 */
struct regs {
	/* Pushed by common stub */
	uintptr_t cr2, gs, fs, es, ds;
	uintptr_t r15, r14, r13, r12;
	uintptr_t r11, r10, r9, r8;
	uintptr_t rbp, rdi, rsi, rdx, rcx, rbx, rax;

	/* Pushed by wrapper */
	uintptr_t int_no, err_code;

	/* Pushed by interrupt */
	uintptr_t rip, cs, rflags, rsp, ss;
};

static inline void* read_gs_register() {
	return (void*)rdmsr(0xC0000101);
}

static inline void set_gs_register(void* toSet) {
    wrmsr(0xC0000101, (uint64_t)toSet);
	wrmsr(0xC0000102, (uint64_t)toSet);
	asm volatile ("swapgs");
}

/*
 * this_cpu - return a pointer to the calling CPU's core_t.
 *
 * Valid after smp_init() has called set_gs_register() on every core.
 * Must not be called before the BSP's core_t is set (before the early
 * set_gs_register() call in kernel.c).
 */
static inline core_t *this_cpu(void) {
    return (core_t *)read_gs_register();
}

/* ------------------------------------------------------------------ */
/* IRQ control primitives                                               */
/* ------------------------------------------------------------------ */

/*
 * irq_save() / irq_restore() - explicit IRQ state management.
 *
 * Use these for non-lock critical sections that need to be
 * interrupt-safe without full mutual exclusion.
 *
 * Nesting is supported: irq_save() returns the state *before* disabling,
 * and irq_restore() re-enables only if that state was enabled.
 *
 * For mutual exclusion + IRQ safety, prefer spinlock_acquire() /
 * spinlock_release() which embed these semantics.
 */
static inline irq_state_t irq_save(void) {
    irq_state_t s = interrupt_state();
    disable_interrupts();
    return s;
}

static inline void irq_restore(irq_state_t state) {
    if (state) enable_interrupts();
}