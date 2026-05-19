#pragma once

#include <stdint.h>
#include <stddef.h>
#include <kernel/pmm.h>

/*
 * VMM invariants:
 *   - vmm_init() must be called after pmm_init().
 *   - mmu_map_page / mmu_unmap_page are NOT internally synchronized.
 *     Callers that may race on the same pagemap must hold an external lock.
 *   - All virtual and physical addresses must be page-aligned (PAGE_SIZE).
 *   - mmu_kernel_pagemap is read-only after vmm_init(); it must not be
 *     reassigned by any caller outside vmm.c.
 */

typedef uint64_t pagemap_t;

#define PAGE_SIZE         0x1000
#define PTE_ADDR_MASK     0x000ffffffffff000ULL

#define PTE_GET_ADDR(v)   ((v) & PTE_ADDR_MASK)
#define PTE_GET_FLAGS(v)  ((v) & ~PTE_ADDR_MASK)

#define PTE_PRESENT       ((uint64_t)1 << 0)
#define PTE_WRITABLE      ((uint64_t)1 << 1)
#define PTE_USER          ((uint64_t)1 << 2)
#define PTE_WRITE_THOUGH  ((uint64_t)1 << 3)
#define PTE_CACHE_DISABLE ((uint64_t)1 << 4)
#define PTE_LARGER_PAGE   ((uint64_t)1 << 6)
#define PTE_NX            ((uint64_t)1 << 63)

extern pagemap_t *mmu_kernel_pagemap;

void       vmm_init(void);
void       mmu_map_page(pagemap_t *pagemap, uintptr_t virt, uintptr_t phys, uint64_t flags);
void       mmu_unmap_page(pagemap_t *pagemap, uintptr_t virt);
uintptr_t  mmu_virt_to_phys(pagemap_t *pagemap, uintptr_t virt);
pagemap_t *vmm_new_pagemap(void);
void       mmu_switch_pagemap(pagemap_t *pagemap);
