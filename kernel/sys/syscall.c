/*
 * syscall.c - SYSCALL/SYSRET infrastructure and dispatch table.
 *
 * syscall_init() must be called once per logical CPU from core_start(),
 * after the per-core GDT and TSS are live.
 *
 * syscall_dispatch() is invoked by the assembly stub in syscall.S with
 * a pointer to the saved register frame on the kernel stack.
 */

#include <kernel/syscall.h>
#include <kernel/msr.h>
#include <kernel/kprintf.h>
#include <kernel/panic.h>
#include <kernel/gdt.h>
#include <stdint.h>

/* EFER bit 0 enables SYSCALL/SYSRET */
#define EFER_SCE  (1ULL << 0)

/*
 * STAR encoding:
 *   [47:32] kernel CS selector (0x08)  -- SYSCALL sets CS = this, SS = this+8
 *   [63:48] user   CS base    (0x10)  -- SYSRETQ sets CS = this+16|3 = 0x23
 *                                                    SS = this+8 |3 = 0x1B
 */
#define STAR_VAL  ((0x0010ULL << 48) | (0x0008ULL << 32))

/*
 * SFMASK: bits set here are cleared from RFLAGS on SYSCALL entry.
 *   bit  8 (TF) -- disable single-step
 *   bit  9 (IF) -- disable interrupts until the kernel stack is ready
 *   bit 10 (DF) -- clear direction flag (SysV ABI requirement)
 */
#define SFMASK_VAL ((1u << 8) | (1u << 9) | (1u << 10))

/* Defined in syscall.S */
extern void syscall_entry(void);

void syscall_init(void) {
    wrmsr(MSR_EFER,  rdmsr(MSR_EFER) | EFER_SCE);
    wrmsr(MSR_STAR,  STAR_VAL);
    wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)syscall_entry);
    wrmsr(MSR_FMASK, SFMASK_VAL);
}

/* ------------------------------------------------------------------ */
/* Dispatch table                                                       */
/* ------------------------------------------------------------------ */

static syscall_fn_t syscall_table[SYSCALL_MAX];

void syscall_register(uint64_t nr, syscall_fn_t fn) {
    KERNEL_ASSERT(nr < SYSCALL_MAX);
    syscall_table[nr] = fn;
}

void syscall_dispatch(syscall_frame_t *f) {
    uint64_t nr = f->rax;

    if (nr < SYSCALL_MAX && syscall_table[nr]) {
        f->rax = syscall_table[nr](f);
        return;
    }

    kprintf("syscall: unhandled nr=%lu from rip=%#lx\n", nr, f->rcx);
    f->rax = (uint64_t)-38; /* -ENOSYS */
}
