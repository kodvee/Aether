#include <kernel/pmm.h>
#include <kernel/vmm.h>
#include <kernel/spinlock.h>
#include <kernel/kprintf.h>
#include <kernel/macros.h>
#include <kernel/cpu.h>
#include <kernel/panic.h>
#include <stdint.h>
#include <stdbool.h>
#include <limine.h>

/* Limine requests — memmap is non-static so vmm.c can read it too */
__attribute__((used, section(".requests")))
volatile struct limine_memmap_request memmap_request = {
    .id       = LIMINE_MEMMAP_REQUEST,
    .revision = 0,
};

__attribute__((used, section(".requests")))
volatile struct limine_hhdm_request hhdm_request = {
    .id       = LIMINE_HHDM_REQUEST,
    .revision = 0,
};

static spinlock_t pmm_lock = SPINLOCK_ZERO;

static uint8_t  *bitmap    = NULL;
static uint64_t  nframes   = 0;
static uint64_t  lastFrame = 0;

/* -- Unlocked bit helpers (caller must hold pmm_lock) ------------------- */

static inline void _pmm_set(uint64_t idx) {
    bitmap[idx >> 3] |= (uint8_t)(1u << (idx & 7));
}

static inline void _pmm_clear(uint64_t idx) {
    bitmap[idx >> 3] &= (uint8_t)~(1u << (idx & 7));
}

static inline bool _pmm_test(uint64_t idx) {
    return (bitmap[idx >> 3] >> (idx & 7)) & 1u;
}

/* -- Public frame operations -------------------------------------------- */

void mmu_frame_set(uintptr_t addr) {
    bool s = spinlock_acquire(&pmm_lock);
    uint64_t idx = addr / PAGE_SIZE;
    if (idx < nframes)
        _pmm_set(idx);
    spinlock_release(&pmm_lock, s);
}

void mmu_frame_clear(uintptr_t addr) {
    bool s = spinlock_acquire(&pmm_lock);
    uint64_t idx = addr / PAGE_SIZE;
    if (idx < nframes)
        _pmm_clear(idx);
    spinlock_release(&pmm_lock, s);
}

/*
 * mmu_request_frame — atomically test-and-set one free frame.
 * lastFrame wraps modulo nframes so freed frames are eventually reused
 * without rescanning from 0 on every allocation.
 */
uintptr_t mmu_request_frame(void) {
    bool s = spinlock_acquire(&pmm_lock);
    for (uint64_t i = 0; i < nframes; i++) {
        uint64_t idx = (lastFrame + i) % nframes;
        if (!_pmm_test(idx)) {
            _pmm_set(idx);
            lastFrame = (idx + 1) % nframes;
            spinlock_release(&pmm_lock, s);
            return idx * PAGE_SIZE;
        }
    }
    spinlock_release(&pmm_lock, s);
    SUBSYS_PANIC("pmm", "Out of physical memory");
}

/* mmu_request_frames — find num physically contiguous free frames. */
uintptr_t mmu_request_frames(uint64_t num) {
    if (num == 0) return 0;
    bool s = spinlock_acquire(&pmm_lock);
    uint64_t start = 0, run = 0;
    for (uint64_t i = 0; i < nframes; i++) {
        if (!_pmm_test(i)) {
            if (run == 0) start = i;
            if (++run == num) {
                for (uint64_t j = start; j < start + num; j++)
                    _pmm_set(j);
                lastFrame = start + num;
                spinlock_release(&pmm_lock, s);
                return start * PAGE_SIZE;
            }
        } else {
            run = 0;
        }
    }
    spinlock_release(&pmm_lock, s);
    kprintf("pmm: out of contiguous memory (%lu frames)\n", num);
    SUBSYS_PANIC("pmm", "Out of contiguous physical memory");
}

/*
 * mmu_free_frames — release pages frames back to the pool.
 * addr is the PHYSICAL base address; pages is the count.
 * Operates under a single lock acquisition to avoid the nested-lock
 * deadlock the old implementation had (mmu_free_frames → mmu_frame_clear
 * both acquired mmu_lock).
 */
void mmu_free_frames(void *addr, uint64_t pages) {
    bool s = spinlock_acquire(&pmm_lock);
    uint64_t base_idx = (uintptr_t)addr / PAGE_SIZE;
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t idx = base_idx + i;
        if (idx < nframes)
            _pmm_clear(idx);
    }
    spinlock_release(&pmm_lock, s);
}

uint64_t clean_reclaimable_memory(void) {
    struct limine_memmap_response *resp = memmap_request.response;
    if (!resp || !resp->entry_count) SUBSYS_PANIC("pmm", "Memory map unavailable during reclaim");

    uint64_t cleared = 0;
    for (uint64_t i = 0; i < resp->entry_count; i++) {
        struct limine_memmap_entry *e = resp->entries[i];
        if (e->type != LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE) continue;
        for (uint64_t page = e->base; page < e->base + e->length; page += PAGE_SIZE)
            mmu_frame_clear(page);
        cleared += e->length;
        kprintf("pmm: reclaimed %p-%p (%lu bytes)\n",
                (void *)e->base, (void *)(e->base + e->length), e->length);
    }
    return cleared;
}

/* -- Initialisation ----------------------------------------------------- */

void __init pmm_init(void) {
    struct limine_memmap_response *resp = memmap_request.response;
    if (!resp || !resp->entry_count) SUBSYS_PANIC("pmm", "No memory map from bootloader");

    /* Total addressable memory span */
    uint64_t top = 0;
    for (uint64_t i = 0; i < resp->entry_count; i++) {
        struct limine_memmap_entry *e = resp->entries[i];
        uint64_t end = e->base + e->length;
        if (end > top) top = end;
    }

    nframes = top / PAGE_SIZE;
    uint64_t bitmap_bytes = (nframes + 7) / 8;

    /* Find first usable region large enough to hold the bitmap */
    uintptr_t bitmap_phys = 0;
    for (uint64_t i = 0; i < resp->entry_count; i++) {
        struct limine_memmap_entry *e = resp->entries[i];
        if (e->type == LIMINE_MEMMAP_USABLE && e->length >= bitmap_bytes) {
            bitmap_phys = e->base;
            break;
        }
    }
    if (!bitmap_phys) SUBSYS_PANIC("pmm", "No usable region large enough for PMM bitmap");

    /* Access the bitmap through the HHDM (limine provides this before we switch) */
    bitmap = (uint8_t *)(bitmap_phys + HHDM_HIGHER_HALF);

    /* Mark everything used */
    __builtin_memset(bitmap, 0xFF, bitmap_bytes);

    /* Free all USABLE frames */
    for (uint64_t i = 0; i < resp->entry_count; i++) {
        struct limine_memmap_entry *e = resp->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE) continue;
        for (uint64_t page = e->base; page < e->base + e->length; page += PAGE_SIZE)
            _pmm_clear(page / PAGE_SIZE);
    }

    /* Re-mark the bitmap pages as used */
    uint64_t bitmap_pages = (bitmap_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint64_t i = 0; i < bitmap_pages; i++)
        _pmm_set((bitmap_phys / PAGE_SIZE) + i);

    kprintf("pmm: %lu frames tracked, bitmap at %p (%lu pages)\n",
            nframes, bitmap, bitmap_pages);
}
