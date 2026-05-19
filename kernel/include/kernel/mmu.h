#pragma once

#include <stddef.h>
#include <kernel/spinlock.h>
#include <kernel/pmm.h>
#include <kernel/vmm.h>

static inline void mmu_init(void) {
    pmm_init();
    vmm_init();
}

void  slab_init(void);
void *malloc(size_t size);
void *realloc(void *addr, size_t new_size);
void  free(void *addr);
