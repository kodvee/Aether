/**
 * stacktrace.c — stack frame walker and symbol resolver.
 *
 * Uses the ELF subsystem (kelf) as the sole source of symbol information.
 * All paths are allocation-free and safe to call from interrupt or panic
 * context, including when the kernel heap is in an unknown state.
 */

#include <kernel/stacktrace.h>
#include <kernel/elf.h>
#include <kernel/kprintf.h>
#include <kernel/cpu.h>        /* struct regs */
#include <stdint.h>
#include <stdbool.h>
#include <elf.h>

/* Maximum frames before we give up (prevents loops on corrupted stacks) */
#define MAX_FRAMES 64

/*
 * Canonical x86-64 kernel address: upper 16 bits must be all-ones.
 * Also require 8-byte alignment since rbp is always pointer-aligned.
 */
static inline bool is_valid_kernel_addr(uintptr_t addr) {
    return (addr >> 48) == 0xffffUL && (addr & 7) == 0;
}

/* ------------------------------------------------------------------ */

void stacktrace_print_from(uintptr_t rip, uintptr_t rbp) {
    kprintf("Stack trace:\n");

    for (int depth = 0; depth < MAX_FRAMES; depth++) {
        if (rip == 0) break;

        const char     *name = NULL;
        const Elf64_Sym *sym  = elf_sym_by_addr(&kelf, rip, &name);

        if (sym != NULL && name != NULL && name[0] != '\0') {
            /* Resolve the containing section name for extra context */
            const Elf64_Shdr *sec      = elf_sym_section(&kelf, sym);
            const char       *sec_name = sec ? elf_section_name(&kelf, sec) : "?";

            kprintf("  #%-2d  %p  %s+0x%lx  [%s]\n",
                    depth, (void *)rip, name,
                    elf_sym_offset(sym, rip), sec_name);
        } else {
            kprintf("  #%-2d  %p  ???\n", depth, (void *)rip);
        }

        /* Validate the next frame pointer before touching it */
        if (!is_valid_kernel_addr(rbp)) break;

        stack_frame_t *frame    = (stack_frame_t *)rbp;
        uintptr_t      next_rip = frame->rip;
        uintptr_t      next_rbp = (uintptr_t)frame->rbp;

        if (next_rip == 0) break;

        /*
         * The stack grows downward: each saved rbp must be strictly greater
         * than the current one.  A same-or-lower value means corruption or
         * that we have reached the bottom of the initial stack.
         */
        if (next_rbp <= rbp) break;

        rip = next_rip;
        rbp = next_rbp;
    }
}

void stacktrace_print(void) {
    uintptr_t rbp;
    asm volatile("movq %%rbp, %0" : "=r"(rbp));

    if (!is_valid_kernel_addr(rbp)) {
        kprintf("Stack trace: [no valid frame pointer]\n");
        return;
    }

    /*
     * rbp currently points to our own frame record.  Dereference it once to
     * step into the caller's frame so that stacktrace_print itself does not
     * appear as frame #0.
     */
    stack_frame_t *frame = (stack_frame_t *)rbp;
    stacktrace_print_from(frame->rip, (uintptr_t)frame->rbp);
}

void stacktrace_print_regs(struct regs *r) {
    if (r != NULL) {
        /*
         * The interrupt frame gives us the exact instruction and frame pointer
         * at the moment of the fault — use them directly so the trace starts
         * at the real fault site, not inside the panic handler.
         */
        stacktrace_print_from(r->rip, r->rbp);
    } else {
        stacktrace_print();
    }
}
