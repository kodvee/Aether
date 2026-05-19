/*
 * process.c - process lifecycle and user address-space management.
 *
 * process_create / process_destroy handle process_t allocation and the
 * pagemap lifetime.  process_mmap / process_munmap maintain the VMA list
 * and drive the page-table entries.
 *
 * Physical frames are allocated via mmu_request_frame() (panics on OOM).
 */

#include <stdint.h>
#include <stddef.h>
#include <kernel/scheduler.h>
#include <kernel/mmu.h>
#include <kernel/panic.h>

/* ------------------------------------------------------------------ */
/* Process lifecycle                                                    */
/* ------------------------------------------------------------------ */

process_t *process_create(const char *name) {
    process_t *proc = malloc(sizeof(process_t));
    if (!proc) return NULL;

    proc->pid          = scheduler_alloc_pid();
    proc->name         = name;
    proc->pagemap      = vmm_new_pagemap();
    proc->lock         = (spinlock_t)SPINLOCK_ZERO;
    list_head_init(&proc->threads);
    proc->thread_count = 0;
    list_head_init(&proc->vma_list);
    proc->mmap_base    = MMAP_BASE;
    proc->brk          = 0;

    return proc;
}

void process_destroy(process_t *proc) {
    KERNEL_ASSERT(proc != NULL);
    KERNEL_ASSERT(proc->thread_count == 0);

    /* Drain VMA list — process_munmap removes each entry individually */
    list_node_t *n, *tmp;
    list_for_each_safe(n, tmp, &proc->vma_list) {
        vma_t *vma = list_entry(n, vma_t, node);
        process_munmap(proc, vma->base, vma->length);
    }

    /* TODO: free the page-table pages (vmm_free_pagemap not yet implemented) */

    free(proc);
}

/* ------------------------------------------------------------------ */
/* Address space management                                             */
/* ------------------------------------------------------------------ */

static uint64_t prot_to_pte(uint32_t prot) {
    uint64_t flags = PTE_PRESENT | PTE_USER;
    if (prot & PROT_WRITE) flags |= PTE_WRITABLE;
    if (!(prot & PROT_EXEC)) flags |= PTE_NX;
    return flags;
}

uintptr_t process_mmap(process_t *proc, uintptr_t hint, size_t length,
                        uint32_t prot) {
    /* Page-align the length */
    length = (length + PAGE_SIZE - 1) & ~(size_t)(PAGE_SIZE - 1);
    if (!length) return 0;

    /* Choose the base address */
    uintptr_t base;
    if (hint) {
        base = hint & ~(uintptr_t)(PAGE_SIZE - 1);
    } else {
        base            = proc->mmap_base;
        proc->mmap_base += length;
    }

    uint64_t flags = prot_to_pte(prot);

    /* Allocate, zero, and map each page */
    for (uintptr_t off = 0; off < length; off += PAGE_SIZE) {
        uintptr_t phys = mmu_request_frame();
        __builtin_memset((void *)(phys + hhdm_offset), 0, PAGE_SIZE);
        mmu_map_page(proc->pagemap, base + off, phys, flags);
    }

    /* Record the mapping */
    vma_t *vma = malloc(sizeof(vma_t));
    KERNEL_ASSERT(vma != NULL);
    vma->base   = base;
    vma->length = length;
    vma->prot   = prot;
    list_node_init(&vma->node);

    bool irq = spinlock_acquire(&proc->lock);
    list_push_back(&proc->vma_list, &vma->node);
    spinlock_release(&proc->lock, irq);

    return base;
}

void process_munmap(process_t *proc, uintptr_t addr, size_t length) {
    length = (length + PAGE_SIZE - 1) & ~(size_t)(PAGE_SIZE - 1);

    bool irq = spinlock_acquire(&proc->lock);

    list_node_t *n;
    list_for_each(n, &proc->vma_list) {
        vma_t *vma = list_entry(n, vma_t, node);
        if (vma->base == addr && vma->length == length) {
            list_remove(n);
            spinlock_release(&proc->lock, irq);

            /* Unmap pages and free their physical frames */
            for (uintptr_t off = 0; off < length; off += PAGE_SIZE) {
                uintptr_t phys = mmu_virt_to_phys(proc->pagemap, addr + off);
                mmu_unmap_page(proc->pagemap, addr + off);
                if (phys) mmu_free_frames((void *)phys, 1);
            }

            free(vma);
            return;
        }
    }

    spinlock_release(&proc->lock, irq);
}

uintptr_t process_alloc_ustack(process_t *proc) {
    uintptr_t base = process_mmap(proc, USTACK_TOP - USTACK_SIZE, USTACK_SIZE,
                                   PROT_READ | PROT_WRITE);
    return base + USTACK_SIZE;   /* return the stack pointer (top of mapping) */
}
