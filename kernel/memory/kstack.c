/*
 * kstack.c - kernel thread stack allocation
 *
 * A kernel stack is a contiguous region of KSTACK_SIZE bytes backed by
 * PMM frames.  vmm_init() permanently maps every USABLE physical frame in
 * the HHDM (HHDM_HIGHER_HALF + phys → phys, PTE_PRESENT|PTE_WRITABLE).
 * That mapping is the invariant this subsystem relies on and must not be
 * disturbed: clearing or replacing HHDM PTEs corrupts every pagemap that
 * shares the intermediate page-table chain (vmm_new_pagemap copies PML4
 * entries by reference, not deep-copy).
 *
 * Therefore kstack_alloc/kstack_free do NO page-table manipulation.
 * They simply allocate / release contiguous physical frames from the PMM
 * and surface the pre-existing HHDM virtual range as the stack region.
 */

#include <kernel/scheduler.h>
#include <kernel/pmm.h>
#include <kernel/vmm.h>
#include <stdint.h>
#include <stddef.h>

uintptr_t kstack_alloc(void) {
    uint64_t  pages = KSTACK_SIZE / PAGE_SIZE;
    uintptr_t phys  = mmu_request_frames(pages);
    if (!phys) return 0;

    /* vmm_init already mapped phys..phys+KSTACK_SIZE in the HHDM */
    return phys + HHDM_HIGHER_HALF + KSTACK_SIZE;
}

void kstack_free(uintptr_t stack_top) {
    if (!stack_top) return;
    uintptr_t phys  = (stack_top - KSTACK_SIZE) - HHDM_HIGHER_HALF;
    uint64_t  pages = KSTACK_SIZE / PAGE_SIZE;

    /* Return frames to the PMM; leave HHDM PTEs intact */
    mmu_free_frames((void *)phys, pages);
}
