#pragma once

#include <stdint.h>

/*
 * PMM invariants:
 *   - pmm_init() must be called before any other PMM function.
 *   - All returned physical addresses are 4 KiB aligned and non-zero.
 *     Physical frame 0 is permanently marked used during init.
 *   - mmu_request_frame / mmu_request_frames panic on OOM; they must only
 *     be used for kernel-internal allocations where OOM is a bug.
 *   - hhdm_offset is stable after pmm_init() and must not be written again.
 */

/* HHDM base offset — set once by pmm_init(), valid for the kernel lifetime. */
extern uint64_t hhdm_offset;
#define HHDM_HIGHER_HALF hhdm_offset

void      pmm_init(void);
uintptr_t mmu_request_frame(void);
uintptr_t mmu_request_frames(uint64_t num);
void      mmu_free_frames(void *addr, uint64_t pages);
void      mmu_frame_set(uintptr_t addr);
void      mmu_frame_clear(uintptr_t addr);
uint64_t  clean_reclaimable_memory(void);
