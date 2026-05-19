#pragma once

#include <stdint.h>

void      pmm_init(void);
uintptr_t mmu_request_frame(void);
uintptr_t mmu_request_frames(uint64_t num);
void      mmu_free_frames(void *addr, uint64_t pages);
void      mmu_frame_set(uintptr_t addr);
void      mmu_frame_clear(uintptr_t addr);
uint64_t  clean_reclaimable_memory(void);
