#include <kernel/vmm.h>
#include <kernel/pmm.h>
#include <kernel/kprintf.h>
#include <kernel/macros.h>
#include <kernel/cpu.h>
#include <stdint.h>
#include <stdbool.h>
#include <limine.h>

/* memmap_request is defined in pmm.c */
extern volatile struct limine_memmap_request memmap_request;

__attribute__((used, section(".requests")))
static volatile struct limine_kernel_address_request kaddr_request = {
    .id       = LIMINE_KERNEL_ADDRESS_REQUEST,
    .revision = 0,
};

pagemap_t *mmu_kernel_pagemap = NULL;

extern char text_start[],   text_end[];
extern char rodata_start[], rodata_end[];
extern char data_start[],   data_end[];

/* -- Internal helpers --------------------------------------------------- */

static uint64_t *get_next_level(uint64_t *top, size_t idx, bool alloc) {
    if (top[idx] & PTE_PRESENT)
        return (uint64_t *)(PTE_GET_ADDR(top[idx]) + HHDM_HIGHER_HALF);
    if (!alloc) return NULL;
    uintptr_t frame = mmu_request_frame();
    uint64_t *next  = (uint64_t *)(frame + HHDM_HIGHER_HALF);
    __builtin_memset(next, 0, PAGE_SIZE);
    top[idx] = frame | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
    return next;
}

/* -- Public VMM interface ----------------------------------------------- */

void mmu_map_page(pagemap_t *pagemap, uintptr_t virt, uintptr_t phys, uint64_t flags) {
    uint64_t *pdp = get_next_level(pagemap,  (virt >> 39) & 0x1FF, true);
    uint64_t *pd  = get_next_level(pdp,      (virt >> 30) & 0x1FF, true);
    uint64_t *pt  = get_next_level(pd,       (virt >> 21) & 0x1FF, true);
    pt[(virt >> 12) & 0x1FF] = phys | flags;
}

void mmu_unmap_page(pagemap_t *pagemap, uintptr_t virt) {
    uint64_t *pdp = get_next_level(pagemap,  (virt >> 39) & 0x1FF, false);
    if (!pdp) return;
    uint64_t *pd  = get_next_level(pdp,      (virt >> 30) & 0x1FF, false);
    if (!pd)  return;
    uint64_t *pt  = get_next_level(pd,       (virt >> 21) & 0x1FF, false);
    if (!pt)  return;
    pt[(virt >> 12) & 0x1FF] = 0;
    asm volatile ("invlpg (%0)" :: "r"(virt) : "memory");
}

/*
 * vmm_new_pagemap — allocate a fresh PML4 and copy the kernel's higher-half
 * entries (indices 256–511) so the new pagemap shares kernel mappings.
 * Used by the scheduler to create per-process address spaces.
 */
pagemap_t *vmm_new_pagemap(void) {
    pagemap_t *pm = (pagemap_t *)(mmu_request_frame() + HHDM_HIGHER_HALF);
    __builtin_memset(pm, 0, PAGE_SIZE);
    for (size_t i = 256; i < 512; i++)
        pm[i] = mmu_kernel_pagemap[i];
    return pm;
}

void mmu_switch_pagemap(pagemap_t *pagemap) {
    asm volatile (
        "mov %0, %%cr3"
        :: "r"((uintptr_t)pagemap - HHDM_HIGHER_HALF)
        : "memory"
    );
}

/* -- Initialisation ----------------------------------------------------- */

void __init vmm_init(void) {
    struct limine_memmap_response *resp  = memmap_request.response;
    struct limine_kernel_address_response *kaddr = kaddr_request.response;
    if (!resp || !resp->entry_count || !kaddr) fatal();

    mmu_kernel_pagemap = (pagemap_t *)(mmu_request_frame() + HHDM_HIGHER_HALF);
    __builtin_memset(mmu_kernel_pagemap, 0, PAGE_SIZE);

    /* Map all physical memory into HHDM */
    for (uint64_t i = 0; i < resp->entry_count; i++) {
        struct limine_memmap_entry *e = resp->entries[i];
        for (uint64_t page = e->base; page < e->base + e->length; page += PAGE_SIZE)
            mmu_map_page(mmu_kernel_pagemap,
                         page + HHDM_HIGHER_HALF, page,
                         PTE_PRESENT | PTE_WRITABLE);
    }

    /* Map kernel sections with correct permissions */
    for (uintptr_t p = (uintptr_t)text_start; p < (uintptr_t)text_end; p += PAGE_SIZE)
        mmu_map_page(mmu_kernel_pagemap, p,
                     kaddr->physical_base + (p - kaddr->virtual_base),
                     PTE_PRESENT);

    for (uintptr_t p = (uintptr_t)rodata_start; p < (uintptr_t)rodata_end; p += PAGE_SIZE)
        mmu_map_page(mmu_kernel_pagemap, p,
                     kaddr->physical_base + (p - kaddr->virtual_base),
                     PTE_PRESENT | PTE_NX);

    /* ISR stubs live in .data (int.S line 105: .section .data), so this
     * section cannot be marked NX until they are moved to .text.       */
    for (uintptr_t p = (uintptr_t)data_start; p < (uintptr_t)data_end; p += PAGE_SIZE)
        mmu_map_page(mmu_kernel_pagemap, p,
                     kaddr->physical_base + (p - kaddr->virtual_base),
                     PTE_PRESENT | PTE_WRITABLE);

    mmu_switch_pagemap(mmu_kernel_pagemap);
    kprintf("vmm: kernel pagemap active\n");
}
