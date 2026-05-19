/*
 * kstack.c - kernel thread stack allocation
 *
 * A kernel stack is a contiguous region of KSTACK_SIZE bytes backed by
 * PMM frames and mapped into the kernel pagemap.  The top of the stack
 * (the initial RSP value) is returned to the caller.
 *
 * No guard pages are inserted in the initial implementation; a thread that
 * overflows its stack will corrupt adjacent kernel memory.  Guard pages
 * can be added by mapping one additional unmapped page below the stack
 * region once the VMM supports that.
 */

#include <kernel/scheduler.h>
#include <kernel/pmm.h>
#include <kernel/vmm.h>
#include <kernel/mmu.h>
#include <stdint.h>
#include <stddef.h>

uintptr_t kstack_alloc(void) {
    uint64_t pages = KSTACK_SIZE / PAGE_SIZE;
    uintptr_t phys = mmu_request_frames(pages);
    if (!phys) return 0;

    /* Map the stack frames into the kernel pagemap (writable, NX, no-user) */
    uintptr_t virt = phys + HHDM_HIGHER_HALF;
    for (uint64_t i = 0; i < pages; i++) {
        mmu_map_page(mmu_kernel_pagemap,
                     virt + i * PAGE_SIZE,
                     phys + i * PAGE_SIZE,
                     PTE_PRESENT | PTE_WRITABLE | PTE_NX);
    }

    /* Return the TOP of the stack (highest address + 1, stack grows down) */
    return virt + KSTACK_SIZE;
}

void kstack_free(uintptr_t stack_top) {
    if (!stack_top) return;
    uintptr_t virt  = stack_top - KSTACK_SIZE;
    uintptr_t phys  = virt - HHDM_HIGHER_HALF;
    uint64_t  pages = KSTACK_SIZE / PAGE_SIZE;

    for (uint64_t i = 0; i < pages; i++)
        mmu_unmap_page(mmu_kernel_pagemap, virt + i * PAGE_SIZE);

    mmu_free_frames((void *)phys, pages);
}
