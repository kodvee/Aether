/*
 * process.c - process lifecycle and user address-space management.
 *
 * process_create / process_destroy handle process_t allocation and the
 * pagemap lifetime.
 *
 * process_mmap records VMAs without allocating physical pages; pages are
 * committed on first access via page_fault_handle / process_ensure_page
 * (demand paging).
 *
 * process_munmap handles exact, head-trim, tail-trim, and middle-split
 * cases so that arbitrary sub-region unmapping is correct.
 *
 * process_mprotect splits VMAs at the range boundaries, updates prot flags,
 * and re-maps already-present pages with new PTE flags.
 *
 * page_fault_handle dispatches vector-14 exceptions:
 *   - kernel faults    -> panic
 *   - user prot faults -> thread_exit (SIGSEGV analog)
 *   - user not-present -> process_ensure_page (demand paging)
 */

#include <stdint.h>
#include <stddef.h>
#include <kernel/scheduler.h>
#include <kernel/mmu.h>
#include <kernel/panic.h>
#include <kernel/cpu.h>

/* ------------------------------------------------------------------ */
/* Internal helpers                                                     */
/* ------------------------------------------------------------------ */

#define PAGE_ALIGN_UP(x) (((uintptr_t)(x) + PAGE_SIZE - 1) & ~(uintptr_t)(PAGE_SIZE - 1))

/*
 * Convert PROT_* flags to x86-64 PTE flags.
 * PROT_NONE produces a present-but-read-only mapping; the caller must
 * not call this for PROT_NONE -- process_ensure_page returns 0 instead.
 */
static uint64_t prot_to_pte(uint32_t prot) {
    uint64_t flags = PTE_PRESENT | PTE_USER;
    if (prot & PROT_WRITE) flags |= PTE_WRITABLE;
    if (!(prot & PROT_EXEC)) flags |= PTE_NX;
    return flags;
}

/*
 * vma_insert_sorted - insert vma into proc->vma_list in ascending base order.
 * Caller must hold proc->lock.
 */
static void vma_insert_sorted(process_t *proc, vma_t *vma) {
    list_node_t *n;
    list_for_each(n, &proc->vma_list) {
        vma_t *cur = list_entry(n, vma_t, node);
        if (vma->base < cur->base) {
            _list_insert(n->prev, n, &vma->node);
            return;
        }
    }
    list_push_back(&proc->vma_list, &vma->node);
}

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

    list_node_t *n, *tmp;
    list_for_each_safe(n, tmp, &proc->vma_list) {
        vma_t *vma = list_entry(n, vma_t, node);
        process_munmap(proc, vma->base, vma->length);
    }

    /* page-table pages are not freed: vmm_free_pagemap is not yet implemented */
    free(proc);
}

/* ------------------------------------------------------------------ */
/* Address space management                                             */
/* ------------------------------------------------------------------ */

/*
 * process_mmap - record a virtual address range as a VMA without allocating
 * physical pages. Pages are committed on first access (demand paging).
 *
 * hint: preferred base address (page-aligned), or 0 to use the watermark.
 * length: byte length (rounded up to PAGE_SIZE).
 * prot: PROT_* flags.
 *
 * Returns the virtual base address, or 0 on failure.
 */
uintptr_t process_mmap(process_t *proc, uintptr_t hint, size_t length,
                        uint32_t prot) {
    length = PAGE_ALIGN_UP(length);
    if (!length) return 0;

    uintptr_t base;
    if (hint) {
        base = hint & ~(uintptr_t)(PAGE_SIZE - 1);
    } else {
        base             = proc->mmap_base;
        proc->mmap_base += length;
    }

    vma_t *vma = malloc(sizeof(vma_t));
    KERNEL_ASSERT(vma != NULL);
    vma->base   = base;
    vma->length = length;
    vma->prot   = prot;
    list_node_init(&vma->node);

    bool irq = spinlock_acquire(&proc->lock);
    vma_insert_sorted(proc, vma);
    spinlock_release(&proc->lock, irq);

    return base;
}

/*
 * process_ensure_page - back the page covering vaddr with a physical frame.
 *
 * If the page is already present, returns its physical address immediately.
 * Otherwise finds the owning VMA, allocates a zeroed frame, maps it, and
 * returns the physical address.
 *
 * Returns 0 if vaddr is not covered by any VMA or the VMA has PROT_NONE.
 */
uintptr_t process_ensure_page(process_t *proc, uintptr_t vaddr) {
    uintptr_t page_base = vaddr & ~(uintptr_t)(PAGE_SIZE - 1);

    bool irq = spinlock_acquire(&proc->lock);

    /* fast path: page already backed */
    uintptr_t phys = mmu_virt_to_phys(proc->pagemap, page_base);
    if (phys) {
        spinlock_release(&proc->lock, irq);
        return phys;
    }

    /* find the owning VMA */
    uint32_t prot = 0;
    list_node_t *n;
    list_for_each(n, &proc->vma_list) {
        vma_t *vma = list_entry(n, vma_t, node);
        if (page_base >= vma->base && page_base < vma->base + vma->length) {
            prot = vma->prot;
            break;
        }
    }

    /* PROT_NONE or no covering VMA -- do not map */
    if (!(prot & (PROT_READ | PROT_WRITE | PROT_EXEC))) {
        spinlock_release(&proc->lock, irq);
        return 0;
    }

    phys = mmu_request_frame();
    __builtin_memset((void *)(phys + hhdm_offset), 0, PAGE_SIZE);
    mmu_map_page(proc->pagemap, page_base, phys, prot_to_pte(prot));

    spinlock_release(&proc->lock, irq);
    return phys;
}

/*
 * process_munmap - remove the mapping for [addr, addr+length).
 *
 * Handles four overlap cases relative to an existing VMA:
 *   entire coverage  -> remove and free the VMA
 *   tail trim        -> shrink VMA length
 *   head trim        -> advance VMA base
 *   middle split     -> split into two VMAs, remove the hole
 *
 * For pages that were never demand-faulted (not present in the page table),
 * no physical frame is freed.
 */
void process_munmap(process_t *proc, uintptr_t addr, size_t length) {
    length = PAGE_ALIGN_UP(length);
    if (!length) return;
    uintptr_t end = addr + length;

    bool irq = spinlock_acquire(&proc->lock);

    list_node_t *n, *tmp;
    list_for_each_safe(n, tmp, &proc->vma_list) {
        vma_t    *vma     = list_entry(n, vma_t, node);
        uintptr_t vma_end = vma->base + vma->length;

        if (vma->base >= end || vma_end <= addr)
            continue;

        uintptr_t unmap_start = (vma->base >= addr) ? vma->base : addr;
        uintptr_t unmap_end   = (vma_end  <= end)   ? vma_end   : end;

        if (addr <= vma->base && end >= vma_end) {
            /* entire VMA falls within the unmap range */
            list_remove(n);
            spinlock_release(&proc->lock, irq);
            for (uintptr_t off = unmap_start; off < unmap_end; off += PAGE_SIZE) {
                uintptr_t phys = mmu_virt_to_phys(proc->pagemap, off);
                if (phys) {
                    mmu_unmap_page(proc->pagemap, off);
                    mmu_free_frames((void *)phys, 1);
                }
            }
            free(vma);
            irq = spinlock_acquire(&proc->lock);

        } else if (vma->base < addr && end >= vma_end) {
            /* tail trim: unmap [addr, vma_end) */
            vma->length = addr - vma->base;
            spinlock_release(&proc->lock, irq);
            for (uintptr_t off = unmap_start; off < unmap_end; off += PAGE_SIZE) {
                uintptr_t phys = mmu_virt_to_phys(proc->pagemap, off);
                if (phys) {
                    mmu_unmap_page(proc->pagemap, off);
                    mmu_free_frames((void *)phys, 1);
                }
            }
            irq = spinlock_acquire(&proc->lock);

        } else if (addr <= vma->base && end < vma_end) {
            /* head trim: unmap [vma->base, end) */
            vma->base   = end;
            vma->length = vma_end - end;
            spinlock_release(&proc->lock, irq);
            for (uintptr_t off = unmap_start; off < unmap_end; off += PAGE_SIZE) {
                uintptr_t phys = mmu_virt_to_phys(proc->pagemap, off);
                if (phys) {
                    mmu_unmap_page(proc->pagemap, off);
                    mmu_free_frames((void *)phys, 1);
                }
            }
            irq = spinlock_acquire(&proc->lock);

        } else {
            /*
             * middle split: [vma->base, addr) survives; [addr, end) is freed;
             * [end, vma_end) becomes a new tail VMA.
             */
            uintptr_t  tail_base   = end;
            uintptr_t  tail_length = vma_end - end;
            uint32_t   tail_prot   = vma->prot;
            list_node_t *anchor    = &vma->node;
            vma->length = addr - vma->base;
            spinlock_release(&proc->lock, irq);

            for (uintptr_t off = addr; off < end; off += PAGE_SIZE) {
                uintptr_t phys = mmu_virt_to_phys(proc->pagemap, off);
                if (phys) {
                    mmu_unmap_page(proc->pagemap, off);
                    mmu_free_frames((void *)phys, 1);
                }
            }

            vma_t *tail = malloc(sizeof(vma_t));
            KERNEL_ASSERT(tail != NULL);
            tail->base   = tail_base;
            tail->length = tail_length;
            tail->prot   = tail_prot;
            list_node_init(&tail->node);

            irq = spinlock_acquire(&proc->lock);
            _list_insert(anchor, anchor->next, &tail->node);
            /* only one VMA can contain the middle; stop iterating */
            break;
        }
    }

    spinlock_release(&proc->lock, irq);
}

uintptr_t process_alloc_ustack(process_t *proc) {
    uintptr_t base = process_mmap(proc, USTACK_TOP - USTACK_SIZE, USTACK_SIZE,
                                   PROT_READ | PROT_WRITE);
    return base + USTACK_SIZE;
}

/* ------------------------------------------------------------------ */
/* mprotect                                                             */
/* ------------------------------------------------------------------ */

/*
 * process_mprotect - update the protection flags for [addr, addr+length).
 *
 * Algorithm:
 *   Pass 1: split any VMA that straddles addr.
 *   Pass 2: split any VMA that straddles addr+length.
 *   Pass 3: for every VMA entirely within [addr, addr+length), update prot
 *           and remap all present pages (mmu_map_page + INVLPG per page).
 *
 * The lock is released around malloc calls (Pass 1/2) and around page-table
 * walks (Pass 3) to keep the critical section short.
 */
errno_t process_mprotect(process_t *proc, uintptr_t addr, size_t length,
                          uint32_t prot) {
    if (addr & (PAGE_SIZE - 1)) return -EINVAL;
    length = PAGE_ALIGN_UP(length);
    if (!length) return EOK;
    uintptr_t end = addr + length;

    bool irq = spinlock_acquire(&proc->lock);

    /* Pass 1: split at addr */
    list_node_t *n;
    list_for_each(n, &proc->vma_list) {
        vma_t    *vma     = list_entry(n, vma_t, node);
        uintptr_t vma_end = vma->base + vma->length;
        if (vma->base < addr && vma_end > addr) {
            uintptr_t  tail_base   = addr;
            uintptr_t  tail_length = vma_end - addr;
            uint32_t   tail_prot   = vma->prot;
            list_node_t *anchor    = &vma->node;
            vma->length = addr - vma->base;
            spinlock_release(&proc->lock, irq);

            vma_t *tail = malloc(sizeof(vma_t));
            KERNEL_ASSERT(tail != NULL);
            tail->base   = tail_base;
            tail->length = tail_length;
            tail->prot   = tail_prot;
            list_node_init(&tail->node);

            irq = spinlock_acquire(&proc->lock);
            _list_insert(anchor, anchor->next, &tail->node);
            break;
        }
    }

    /* Pass 2: split at end */
    list_for_each(n, &proc->vma_list) {
        vma_t    *vma     = list_entry(n, vma_t, node);
        uintptr_t vma_end = vma->base + vma->length;
        if (vma->base < end && vma_end > end) {
            uintptr_t  tail_base   = end;
            uintptr_t  tail_length = vma_end - end;
            uint32_t   tail_prot   = vma->prot;
            list_node_t *anchor    = &vma->node;
            vma->length = end - vma->base;
            spinlock_release(&proc->lock, irq);

            vma_t *tail = malloc(sizeof(vma_t));
            KERNEL_ASSERT(tail != NULL);
            tail->base   = tail_base;
            tail->length = tail_length;
            tail->prot   = tail_prot;
            list_node_init(&tail->node);

            irq = spinlock_acquire(&proc->lock);
            _list_insert(anchor, anchor->next, &tail->node);
            break;
        }
    }

    /* Pass 3: update prot and remap all present pages within [addr, end) */
    list_for_each(n, &proc->vma_list) {
        vma_t *vma = list_entry(n, vma_t, node);
        if (vma->base < addr || vma->base + vma->length > end) continue;

        vma->prot = prot;
        uintptr_t vbase = vma->base;
        uintptr_t vlen  = vma->length;
        uint64_t  flags = prot_to_pte(prot);
        spinlock_release(&proc->lock, irq);

        for (uintptr_t off = vbase; off < vbase + vlen; off += PAGE_SIZE) {
            uintptr_t phys = mmu_virt_to_phys(proc->pagemap, off);
            if (phys) {
                mmu_map_page(proc->pagemap, off, phys, flags);
                asm volatile("invlpg (%0)" :: "r"(off) : "memory");
            }
        }

        irq = spinlock_acquire(&proc->lock);
    }

    spinlock_release(&proc->lock, irq);
    return EOK;
}

/* ------------------------------------------------------------------ */
/* Page fault handler (vector 14)                                       */
/* ------------------------------------------------------------------ */

/*
 * page_fault_handle - resolve or escalate a page fault.
 *
 * Page fault error code bits (Intel SDM Vol 3, Section 4.7):
 *   bit 0 (P):  0 = not-present; 1 = protection violation
 *   bit 1 (W):  0 = read; 1 = write
 *   bit 2 (U):  0 = supervisor; 1 = user mode
 *   bit 4 (I):  1 = instruction fetch
 *
 * Kernel faults (U=0) always panic -- no recovery path exists.
 * User protection faults (P=1, U=1) kill the thread; no signal yet.
 * User not-present faults (P=0, U=1) trigger demand paging; if the
 * faulting address is not covered by a VMA the thread is killed.
 */
void page_fault_handle(struct regs *r) {
    uintptr_t fault_addr = r->cr2;
    int       user_mode  = (r->cs & 0x3) == 3;

    if (!user_mode) {
        PAGE_FAULT_PANIC(r);
    }

    if (r->err_code & 1) {
        /* protection violation: write to read-only page, PROT_NONE access, etc. */
        KWARN("vmm", "user prot fault addr=%#lx err=%#lx pid=%d",
              fault_addr, r->err_code,
              (int)this_cpu()->current_thread->parent->pid);
        thread_exit();
        __builtin_unreachable();
    }

    /* not-present: attempt demand paging */
    process_t *proc = this_cpu()->current_thread->parent;
    if (!proc) {
        PAGE_FAULT_PANIC(r);
    }

    uintptr_t phys = process_ensure_page(proc, fault_addr);
    if (!phys) {
        KWARN("vmm", "user fault addr=%#lx outside VMA, pid=%d",
              fault_addr, (int)proc->pid);
        thread_exit();
        __builtin_unreachable();
    }
    /* fault resolved; isr_common will IRETQ and re-execute the faulting insn */
}
