#ifdef KTEST_ENABLED

#include <kernel/ktest.h>
#include <kernel/vmm.h>
#include <kernel/pmm.h>
#include <kernel/mmu.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * VMM tests use a private pagemap (from vmm_new_pagemap) for all mapping
 * operations so that the live kernel pagemap is never disturbed.  Physical
 * frames are allocated via mmu_request_frame() and accessed for write/read
 * verification through the permanent HHDM mapping at phys + HHDM_HIGHER_HALF.
 *
 * The test virtual addresses live in the user half (below the canonical
 * hole) so they cannot conflict with kernel or HHDM ranges.
 */

#define TEST_VIRT  ((uintptr_t)0x0000000020000000ULL) /* arbitrary user VA */

/* ================================================================== */
/* Group 1: kernel pagemap                                              */
/* ================================================================== */

static void test_vmm_kernel_pagemap_nonnull(ktest_ctx_t *ctx) {
    KT_ASSERT_NONNULL(mmu_kernel_pagemap);
}

KTEST("vmm-kernel-pagemap-nonnull", "vmm",
      "mmu_kernel_pagemap is non-NULL after vmm_init",
      KT_FLAG_CRITICAL, test_vmm_kernel_pagemap_nonnull);

/* ================================================================== */
/* Group 2: PTE flag values                                             */
/*                                                                      */
/* These values are embedded in hardware page table entries and must    */
/* match the x86-64 architecture specification exactly.                 */
/* ================================================================== */

static void test_vmm_pte_flags(ktest_ctx_t *ctx) {
    KT_CHECK_EQ(PTE_PRESENT,       (uint64_t)(1ULL << 0));
    KT_CHECK_EQ(PTE_WRITABLE,      (uint64_t)(1ULL << 1));
    KT_CHECK_EQ(PTE_USER,          (uint64_t)(1ULL << 2));
    KT_CHECK_EQ(PTE_WRITE_THOUGH,  (uint64_t)(1ULL << 3));
    KT_CHECK_EQ(PTE_CACHE_DISABLE, (uint64_t)(1ULL << 4));
    KT_CHECK_EQ(PTE_LARGER_PAGE,   (uint64_t)(1ULL << 6));
    KT_CHECK_EQ(PTE_NX,            (uint64_t)(1ULL << 63));
}

KTEST("vmm-pte-flags", "vmm",
      "PTE_* flag constants match x86-64 page table entry bit positions",
      KT_FLAG_CRITICAL, test_vmm_pte_flags);

/* ================================================================== */
/* Group 3: PTE_GET_ADDR / PTE_GET_FLAGS                               */
/* ================================================================== */

static void test_vmm_pte_addr_mask(ktest_ctx_t *ctx) {
    uint64_t pte = (uint64_t)0x0000DEADBEEF3007ULL;
    uintptr_t addr  = PTE_GET_ADDR(pte);
    uint64_t  flags = PTE_GET_FLAGS(pte);

    /* Address must be 4 KiB aligned */
    KT_CHECK_EQ(addr & (PAGE_SIZE - 1), (uintptr_t)0);

    /* Flags must not include bits 12..51 */
    KT_CHECK_EQ(flags & PTE_ADDR_MASK, (uint64_t)0);
}

KTEST("vmm-pte-addr-mask", "vmm",
      "PTE_GET_ADDR clears flag bits; PTE_GET_FLAGS clears address bits",
      KT_FLAG_CRITICAL, test_vmm_pte_addr_mask);

/* ================================================================== */
/* Group 4: vmm_new_pagemap                                             */
/* ================================================================== */

static void test_vmm_new_pagemap_nonnull(ktest_ctx_t *ctx) {
    pagemap_t *pm = vmm_new_pagemap();
    KT_ASSERT_NONNULL(pm);
    /* Leak: acceptable in test mode (bounded quantity) */
}

KTEST("vmm-new-pagemap-nonnull", "vmm",
      "vmm_new_pagemap returns a non-NULL pagemap",
      KT_FLAG_CRITICAL, test_vmm_new_pagemap_nonnull);

/* ------------------------------------------------------------------ */

static void test_vmm_new_pagemap_distinct(ktest_ctx_t *ctx) {
    pagemap_t *pm1 = vmm_new_pagemap();
    pagemap_t *pm2 = vmm_new_pagemap();
    KT_ASSERT_NONNULL(pm1);
    KT_ASSERT_NONNULL(pm2);
    KT_CHECK(pm1 != pm2);
}

KTEST("vmm-new-pagemap-distinct", "vmm",
      "two vmm_new_pagemap calls return distinct pagemaps",
      KT_FLAG_NONE, test_vmm_new_pagemap_distinct);

/* ================================================================== */
/* Group 5: mmu_map_page / mmu_virt_to_phys                            */
/* ================================================================== */

static void test_vmm_map_virt_to_phys(ktest_ctx_t *ctx) {
    pagemap_t *pm = vmm_new_pagemap();
    KT_ASSERT_NONNULL(pm);

    uintptr_t phys = mmu_request_frame();
    KT_ASSERT(phys != 0);

    mmu_map_page(pm, TEST_VIRT, phys, PTE_PRESENT | PTE_WRITABLE);

    uintptr_t resolved = mmu_virt_to_phys(pm, TEST_VIRT);
    KT_CHECK_EQ(resolved, phys);

    /* Cleanup */
    mmu_unmap_page(pm, TEST_VIRT);
    mmu_free_frames((void *)phys, 1);
}

KTEST("vmm-map-virt-to-phys", "vmm",
      "mmu_virt_to_phys returns the correct physical address after mmu_map_page",
      KT_FLAG_CRITICAL, test_vmm_map_virt_to_phys);

/* ------------------------------------------------------------------ */

static void test_vmm_map_write_read_hhdm(ktest_ctx_t *ctx) {
    pagemap_t *pm = vmm_new_pagemap();
    KT_ASSERT_NONNULL(pm);

    uintptr_t phys = mmu_request_frame();
    KT_ASSERT(phys != 0);

    mmu_map_page(pm, TEST_VIRT, phys, PTE_PRESENT | PTE_WRITABLE);

    /* Access the frame via HHDM while the kernel pagemap is active */
    volatile uint8_t *hhdm_ptr = (volatile uint8_t *)(phys + HHDM_HIGHER_HALF);
    hhdm_ptr[0] = 0xDE;
    hhdm_ptr[PAGE_SIZE - 1] = 0xAD;
    KT_CHECK_EQ((int)hhdm_ptr[0],           0xDE);
    KT_CHECK_EQ((int)hhdm_ptr[PAGE_SIZE-1], 0xAD);

    mmu_unmap_page(pm, TEST_VIRT);
    mmu_free_frames((void *)phys, 1);
}

KTEST("vmm-map-write-read-hhdm", "vmm",
      "physical frame is writable via HHDM after being mapped into a pagemap",
      KT_FLAG_CRITICAL, test_vmm_map_write_read_hhdm);

/* ------------------------------------------------------------------ */

static void test_vmm_unmap_clears_mapping(ktest_ctx_t *ctx) {
    pagemap_t *pm = vmm_new_pagemap();
    KT_ASSERT_NONNULL(pm);

    uintptr_t phys = mmu_request_frame();
    KT_ASSERT(phys != 0);

    mmu_map_page(pm, TEST_VIRT, phys, PTE_PRESENT | PTE_WRITABLE);
    KT_CHECK_EQ(mmu_virt_to_phys(pm, TEST_VIRT), phys);

    mmu_unmap_page(pm, TEST_VIRT);
    uintptr_t after = mmu_virt_to_phys(pm, TEST_VIRT);
    KT_CHECK_EQ(after, (uintptr_t)0);

    mmu_free_frames((void *)phys, 1);
}

KTEST("vmm-unmap-clears-mapping", "vmm",
      "mmu_unmap_page causes mmu_virt_to_phys to return 0 for that address",
      KT_FLAG_CRITICAL, test_vmm_unmap_clears_mapping);

/* ================================================================== */
/* Group 6: HHDM invariants                                             */
/* ================================================================== */

static void test_vmm_hhdm_offset_nonzero(ktest_ctx_t *ctx) {
    KT_CHECK(hhdm_offset != 0u);
}

KTEST("vmm-hhdm-offset-nonzero", "vmm",
      "hhdm_offset (HHDM_HIGHER_HALF) is non-zero after boot",
      KT_FLAG_CRITICAL, test_vmm_hhdm_offset_nonzero);

/* ------------------------------------------------------------------ */

static void test_vmm_hhdm_in_upper_half(ktest_ctx_t *ctx) {
    /* HHDM must be in the canonical upper half (bit 63 set) */
    KT_CHECK(hhdm_offset & (1ULL << 63));
}

KTEST("vmm-hhdm-upper-half", "vmm",
      "hhdm_offset has bit 63 set (maps into canonical upper half)",
      KT_FLAG_CRITICAL, test_vmm_hhdm_in_upper_half);

/* ------------------------------------------------------------------ */

static void test_vmm_hhdm_frame_accessible(ktest_ctx_t *ctx) {
    /* Allocate a frame and access it via HHDM -- the kernel's HHDM
     * permanently maps all USABLE physical memory, so this must work. */
    uintptr_t phys = mmu_request_frame();
    KT_ASSERT(phys != 0);

    volatile uint64_t *p = (volatile uint64_t *)(phys + HHDM_HIGHER_HALF);
    *p = 0xCAFEBABEDEADBEEFULL;
    KT_CHECK_EQ(*p, 0xCAFEBABEDEADBEEFULL);

    mmu_free_frames((void *)phys, 1);
}

KTEST("vmm-hhdm-frame-accessible", "vmm",
      "PMM-allocated frame is readable/writable via HHDM mapping",
      KT_FLAG_CRITICAL, test_vmm_hhdm_frame_accessible);

/* ================================================================== */
/* Group 7: PAGE_SIZE constant                                          */
/* ================================================================== */

static void test_vmm_page_size(ktest_ctx_t *ctx) {
    KT_CHECK_EQ((int)PAGE_SIZE, 0x1000);
}

KTEST("vmm-page-size", "vmm",
      "PAGE_SIZE is 4096 (0x1000)",
      KT_FLAG_CRITICAL, test_vmm_page_size);

/* ================================================================== */
/* Group 8: multiple frame mapping                                      */
/* ================================================================== */

static void test_vmm_multi_page_mapping(ktest_ctx_t *ctx) {
    pagemap_t *pm = vmm_new_pagemap();
    KT_ASSERT_NONNULL(pm);

    #define NPAGES 4
    uintptr_t frames[NPAGES];
    for (int i = 0; i < NPAGES; i++) {
        frames[i] = mmu_request_frame();
        KT_ASSERT(frames[i] != 0);
        uintptr_t virt = TEST_VIRT + (uintptr_t)(i * PAGE_SIZE);
        mmu_map_page(pm, virt, frames[i], PTE_PRESENT | PTE_WRITABLE);
    }

    for (int i = 0; i < NPAGES; i++) {
        uintptr_t virt = TEST_VIRT + (uintptr_t)(i * PAGE_SIZE);
        KT_CHECK_EQ(mmu_virt_to_phys(pm, virt), frames[i]);
        mmu_unmap_page(pm, virt);
        mmu_free_frames((void *)frames[i], 1);
    }
    #undef NPAGES
}

KTEST("vmm-multi-page-mapping", "vmm",
      "4 contiguous virtual pages map to and resolve from correct distinct frames",
      KT_FLAG_NONE, test_vmm_multi_page_mapping);

#endif /* KTEST_ENABLED */
