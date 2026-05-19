#ifdef KTEST_ENABLED

#include <kernel/ktest.h>
#include <kernel/scheduler.h>
#include <kernel/vmm.h>
#include <kernel/pmm.h>
#include <kernel/spinlock.h>
#include <kernel/list.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ================================================================== */
/* Helpers                                                              */
/* ================================================================== */

/* Count VMAs in proc->vma_list under proc->lock */
static int count_vmas(process_t *proc) {
    int count = 0;
    bool irq = spinlock_acquire(&proc->lock);
    list_node_t *pos;
    list_for_each(pos, &proc->vma_list) { count++; }
    spinlock_release(&proc->lock, irq);
    return count;
}

/* Get the Nth VMA (0-based) under proc->lock. NULL if out of range. */
static vma_t *get_vma(process_t *proc, int idx) {
    bool irq = spinlock_acquire(&proc->lock);
    int i = 0;
    list_node_t *pos;
    list_for_each(pos, &proc->vma_list) {
        if (i == idx) {
            vma_t *v = list_entry(pos, vma_t, node);
            spinlock_release(&proc->lock, irq);
            return v;
        }
        i++;
    }
    spinlock_release(&proc->lock, irq);
    return NULL;
}

/* Fixed page-aligned hint addresses for predictable VMA placement */
#define VA_A  ((uintptr_t)0x0000000001000000ULL)  /* 16 MB */
#define VA_B  ((uintptr_t)0x0000000002000000ULL)  /* 32 MB */

/* ================================================================== */
/* Group 1: process_create                                              */
/* ================================================================== */

static void test_proc_create_nonnull(ktest_ctx_t *ctx) {
    process_t *proc = process_create("test-proc");
    KT_ASSERT_NONNULL(proc);
    process_destroy(proc);
}

KTEST("proc-create-nonnull", "process",
      "process_create returns a non-NULL pointer",
      KT_FLAG_CRITICAL, test_proc_create_nonnull);

/* ------------------------------------------------------------------ */

static void test_proc_create_pid_positive(ktest_ctx_t *ctx) {
    process_t *proc = process_create("test-proc");
    KT_ASSERT_NONNULL(proc);
    KT_CHECK(proc->pid >= 1);
    process_destroy(proc);
}

KTEST("proc-create-pid-positive", "process",
      "process_create assigns a positive PID",
      KT_FLAG_CRITICAL, test_proc_create_pid_positive);

/* ------------------------------------------------------------------ */

static void test_proc_create_unique_pids(ktest_ctx_t *ctx) {
    process_t *a = process_create("proc-a");
    process_t *b = process_create("proc-b");
    KT_ASSERT_NONNULL(a);
    KT_ASSERT_NONNULL(b);
    KT_CHECK(a->pid != b->pid);
    process_destroy(a);
    process_destroy(b);
}

KTEST("proc-create-unique-pids", "process",
      "two process_create calls assign distinct PIDs",
      KT_FLAG_CRITICAL, test_proc_create_unique_pids);

/* ------------------------------------------------------------------ */

static void test_proc_create_pagemap_nonnull(ktest_ctx_t *ctx) {
    process_t *proc = process_create("test-proc");
    KT_ASSERT_NONNULL(proc);
    KT_ASSERT_NONNULL(proc->pagemap);
    process_destroy(proc);
}

KTEST("proc-create-pagemap-nonnull", "process",
      "process_create allocates a non-NULL pagemap",
      KT_FLAG_CRITICAL, test_proc_create_pagemap_nonnull);

/* ------------------------------------------------------------------ */

static void test_proc_create_no_threads(ktest_ctx_t *ctx) {
    process_t *proc = process_create("test-proc");
    KT_ASSERT_NONNULL(proc);
    KT_CHECK_EQ((int)proc->thread_count, 0);
    KT_CHECK(list_empty(&proc->threads));
    process_destroy(proc);
}

KTEST("proc-create-no-threads", "process",
      "freshly created process has thread_count == 0 and empty threads list",
      KT_FLAG_CRITICAL, test_proc_create_no_threads);

/* ------------------------------------------------------------------ */

static void test_proc_create_vma_list_empty(ktest_ctx_t *ctx) {
    process_t *proc = process_create("test-proc");
    KT_ASSERT_NONNULL(proc);
    KT_CHECK_EQ(count_vmas(proc), 0);
    process_destroy(proc);
}

KTEST("proc-create-vma-empty", "process",
      "freshly created process has an empty VMA list",
      KT_FLAG_CRITICAL, test_proc_create_vma_list_empty);

/* ------------------------------------------------------------------ */

static void test_proc_create_mmap_base(ktest_ctx_t *ctx) {
    process_t *proc = process_create("test-proc");
    KT_ASSERT_NONNULL(proc);
    KT_CHECK_EQ(proc->mmap_base, MMAP_BASE);
    process_destroy(proc);
}

KTEST("proc-create-mmap-base", "process",
      "process_create initialises mmap_base to MMAP_BASE",
      KT_FLAG_NONE, test_proc_create_mmap_base);

/* ================================================================== */
/* Group 2: process_mmap                                                */
/* ================================================================== */

static void test_proc_mmap_nonzero(ktest_ctx_t *ctx) {
    process_t *proc = process_create("mmap-test");
    KT_ASSERT_NONNULL(proc);

    uintptr_t addr = process_mmap(proc, VA_A, PAGE_SIZE, PROT_READ | PROT_WRITE);
    KT_CHECK(addr != 0);

    process_destroy(proc);
}

KTEST("proc-mmap-nonzero", "process",
      "process_mmap returns a non-zero virtual address",
      KT_FLAG_CRITICAL, test_proc_mmap_nonzero);

/* ------------------------------------------------------------------ */

static void test_proc_mmap_hint(ktest_ctx_t *ctx) {
    process_t *proc = process_create("mmap-hint");
    KT_ASSERT_NONNULL(proc);

    uintptr_t addr = process_mmap(proc, VA_A, PAGE_SIZE, PROT_READ | PROT_WRITE);
    KT_CHECK_EQ(addr, VA_A);

    process_destroy(proc);
}

KTEST("proc-mmap-hint", "process",
      "process_mmap respects the hint address",
      KT_FLAG_CRITICAL, test_proc_mmap_hint);

/* ------------------------------------------------------------------ */

static void test_proc_mmap_adds_vma(ktest_ctx_t *ctx) {
    process_t *proc = process_create("mmap-vma");
    KT_ASSERT_NONNULL(proc);

    KT_CHECK_EQ(count_vmas(proc), 0);
    process_mmap(proc, VA_A, PAGE_SIZE, PROT_READ | PROT_WRITE);
    KT_CHECK_EQ(count_vmas(proc), 1);

    process_destroy(proc);
}

KTEST("proc-mmap-adds-vma", "process",
      "process_mmap inserts exactly one VMA into the VMA list",
      KT_FLAG_CRITICAL, test_proc_mmap_adds_vma);

/* ------------------------------------------------------------------ */

static void test_proc_mmap_vma_fields(ktest_ctx_t *ctx) {
    process_t *proc = process_create("mmap-fields");
    KT_ASSERT_NONNULL(proc);

    uintptr_t addr = process_mmap(proc, VA_A, PAGE_SIZE,
                                  PROT_READ | PROT_WRITE);
    KT_ASSERT(addr != 0);

    vma_t *vma = get_vma(proc, 0);
    KT_ASSERT_NONNULL(vma);
    KT_CHECK_EQ(vma->base,   VA_A);
    KT_CHECK_EQ(vma->length, (size_t)PAGE_SIZE);
    KT_CHECK_EQ(vma->prot,   (uint32_t)(PROT_READ | PROT_WRITE));

    process_destroy(proc);
}

KTEST("proc-mmap-vma-fields", "process",
      "VMA inserted by process_mmap has correct base, length, and prot",
      KT_FLAG_CRITICAL, test_proc_mmap_vma_fields);

/* ------------------------------------------------------------------ */

static void test_proc_mmap_watermark(ktest_ctx_t *ctx) {
    process_t *proc = process_create("mmap-watermark");
    KT_ASSERT_NONNULL(proc);

    uintptr_t base = proc->mmap_base;
    uintptr_t addr = process_mmap(proc, 0, PAGE_SIZE, PROT_READ);
    KT_CHECK_EQ(addr, base);
    KT_CHECK_EQ(proc->mmap_base, base + PAGE_SIZE);

    process_destroy(proc);
}

KTEST("proc-mmap-watermark", "process",
      "process_mmap with hint=0 advances the watermark by the mapping length",
      KT_FLAG_NONE, test_proc_mmap_watermark);

/* ================================================================== */
/* Group 3: process_munmap -- exact coverage                            */
/* ================================================================== */

static void test_proc_munmap_exact(ktest_ctx_t *ctx) {
    process_t *proc = process_create("munmap-exact");
    KT_ASSERT_NONNULL(proc);

    process_mmap(proc, VA_A, PAGE_SIZE, PROT_READ | PROT_WRITE);
    KT_CHECK_EQ(count_vmas(proc), 1);

    process_munmap(proc, VA_A, PAGE_SIZE);
    KT_CHECK_EQ(count_vmas(proc), 0);

    process_destroy(proc);
}

KTEST("proc-munmap-exact", "process",
      "munmap of the exact VMA range removes it entirely",
      KT_FLAG_CRITICAL, test_proc_munmap_exact);

/* ================================================================== */
/* Group 4: process_munmap -- tail trim                                 */
/*                                                                      */
/*  VMA: [VA_A, VA_A + 3*PAGE)                                         */
/*  Unmap: [VA_A + PAGE, VA_A + 3*PAGE) -- last 2 pages               */
/*  Result: VMA shrinks to [VA_A, VA_A + PAGE)                         */
/* ================================================================== */

static void test_proc_munmap_tail_trim(ktest_ctx_t *ctx) {
    process_t *proc = process_create("munmap-tail");
    KT_ASSERT_NONNULL(proc);

    process_mmap(proc, VA_A, 3 * PAGE_SIZE, PROT_READ | PROT_WRITE);

    /* Unmap the last 2 pages (tail trim) */
    process_munmap(proc, VA_A + PAGE_SIZE, 2 * PAGE_SIZE);

    KT_CHECK_EQ(count_vmas(proc), 1);

    vma_t *vma = get_vma(proc, 0);
    KT_ASSERT_NONNULL(vma);
    KT_CHECK_EQ(vma->base,   VA_A);
    KT_CHECK_EQ(vma->length, (size_t)PAGE_SIZE);

    process_destroy(proc);
}

KTEST("proc-munmap-tail-trim", "process",
      "munmap of the tail shrinks VMA length, leaving base unchanged",
      KT_FLAG_CRITICAL, test_proc_munmap_tail_trim);

/* ================================================================== */
/* Group 5: process_munmap -- head trim                                 */
/*                                                                      */
/*  VMA: [VA_A, VA_A + 3*PAGE)                                         */
/*  Unmap: [VA_A, VA_A + 2*PAGE) -- first 2 pages                     */
/*  Result: VMA shrinks to [VA_A + 2*PAGE, VA_A + 3*PAGE)             */
/* ================================================================== */

static void test_proc_munmap_head_trim(ktest_ctx_t *ctx) {
    process_t *proc = process_create("munmap-head");
    KT_ASSERT_NONNULL(proc);

    process_mmap(proc, VA_A, 3 * PAGE_SIZE, PROT_READ | PROT_WRITE);

    /* Unmap the first 2 pages (head trim) */
    process_munmap(proc, VA_A, 2 * PAGE_SIZE);

    KT_CHECK_EQ(count_vmas(proc), 1);

    vma_t *vma = get_vma(proc, 0);
    KT_ASSERT_NONNULL(vma);
    KT_CHECK_EQ(vma->base,   VA_A + 2 * PAGE_SIZE);
    KT_CHECK_EQ(vma->length, (size_t)PAGE_SIZE);

    process_destroy(proc);
}

KTEST("proc-munmap-head-trim", "process",
      "munmap of the head advances VMA base, shrinks length accordingly",
      KT_FLAG_CRITICAL, test_proc_munmap_head_trim);

/* ================================================================== */
/* Group 6: process_munmap -- middle split                              */
/*                                                                      */
/*  VMA: [VA_A, VA_A + 4*PAGE)                                         */
/*  Unmap: [VA_A + PAGE, VA_A + 3*PAGE) -- middle 2 pages             */
/*  Result: two VMAs:                                                   */
/*    left:  [VA_A,          VA_A + PAGE)                              */
/*    right: [VA_A + 3*PAGE, VA_A + 4*PAGE)                           */
/* ================================================================== */

static void test_proc_munmap_middle_split(ktest_ctx_t *ctx) {
    process_t *proc = process_create("munmap-split");
    KT_ASSERT_NONNULL(proc);

    process_mmap(proc, VA_A, 4 * PAGE_SIZE, PROT_READ | PROT_WRITE);

    /* Unmap the middle 2 pages */
    process_munmap(proc, VA_A + PAGE_SIZE, 2 * PAGE_SIZE);

    KT_CHECK_EQ(count_vmas(proc), 2);

    vma_t *left  = get_vma(proc, 0);
    vma_t *right = get_vma(proc, 1);
    KT_ASSERT_NONNULL(left);
    KT_ASSERT_NONNULL(right);

    KT_CHECK_EQ(left->base,    VA_A);
    KT_CHECK_EQ(left->length,  (size_t)PAGE_SIZE);
    KT_CHECK_EQ(right->base,   VA_A + 3 * PAGE_SIZE);
    KT_CHECK_EQ(right->length, (size_t)PAGE_SIZE);

    process_destroy(proc);
}

KTEST("proc-munmap-middle-split", "process",
      "munmap of a middle range splits the VMA into two",
      KT_FLAG_CRITICAL, test_proc_munmap_middle_split);

/* ================================================================== */
/* Group 7: process_munmap -- nop on unmatched range                   */
/* ================================================================== */

static void test_proc_munmap_nop(ktest_ctx_t *ctx) {
    process_t *proc = process_create("munmap-nop");
    KT_ASSERT_NONNULL(proc);

    process_mmap(proc, VA_A, PAGE_SIZE, PROT_READ);

    /* Unmapping a completely different range must be a no-op */
    process_munmap(proc, VA_B, PAGE_SIZE);

    KT_CHECK_EQ(count_vmas(proc), 1);

    vma_t *vma = get_vma(proc, 0);
    KT_ASSERT_NONNULL(vma);
    KT_CHECK_EQ(vma->base,   VA_A);
    KT_CHECK_EQ(vma->length, (size_t)PAGE_SIZE);

    process_destroy(proc);
}

KTEST("proc-munmap-nop", "process",
      "munmap of an address not covered by any VMA is a safe no-op",
      KT_FLAG_NONE, test_proc_munmap_nop);

/* ================================================================== */
/* Group 8: process_ensure_page                                         */
/* ================================================================== */

static void test_proc_ensure_page_basic(ktest_ctx_t *ctx) {
    process_t *proc = process_create("ensure-page");
    KT_ASSERT_NONNULL(proc);

    process_mmap(proc, VA_A, PAGE_SIZE, PROT_READ | PROT_WRITE);

    uintptr_t phys = process_ensure_page(proc, VA_A);
    KT_CHECK(phys != 0);

    /* Frame must be accessible via HHDM */
    volatile uint8_t *hhdm = (volatile uint8_t *)(phys + HHDM_HIGHER_HALF);
    hhdm[0] = 0x42;
    KT_CHECK_EQ((int)hhdm[0], 0x42);

    process_destroy(proc);
}

KTEST("proc-ensure-page-basic", "process",
      "process_ensure_page backs an unmapped VMA page and returns its phys addr",
      KT_FLAG_CRITICAL, test_proc_ensure_page_basic);

/* ------------------------------------------------------------------ */

static void test_proc_ensure_page_idempotent(ktest_ctx_t *ctx) {
    process_t *proc = process_create("ensure-idempotent");
    KT_ASSERT_NONNULL(proc);

    process_mmap(proc, VA_A, PAGE_SIZE, PROT_READ | PROT_WRITE);

    uintptr_t phys1 = process_ensure_page(proc, VA_A);
    uintptr_t phys2 = process_ensure_page(proc, VA_A);

    KT_CHECK(phys1 != 0);
    KT_CHECK_EQ(phys1, phys2);  /* second call returns same frame */

    process_destroy(proc);
}

KTEST("proc-ensure-page-idempotent", "process",
      "calling process_ensure_page twice for the same page returns the same frame",
      KT_FLAG_NONE, test_proc_ensure_page_idempotent);

/* ------------------------------------------------------------------ */

static void test_proc_ensure_page_out_of_vma(ktest_ctx_t *ctx) {
    process_t *proc = process_create("ensure-no-vma");
    KT_ASSERT_NONNULL(proc);

    /* No VMA at VA_B */
    uintptr_t phys = process_ensure_page(proc, VA_B);
    KT_CHECK_EQ(phys, (uintptr_t)0);

    process_destroy(proc);
}

KTEST("proc-ensure-page-out-of-vma", "process",
      "process_ensure_page returns 0 for an address not covered by any VMA",
      KT_FLAG_NONE, test_proc_ensure_page_out_of_vma);

/* ------------------------------------------------------------------ */

static void test_proc_ensure_page_prot_none(ktest_ctx_t *ctx) {
    process_t *proc = process_create("ensure-prot-none");
    KT_ASSERT_NONNULL(proc);

    process_mmap(proc, VA_A, PAGE_SIZE, PROT_NONE);
    uintptr_t phys = process_ensure_page(proc, VA_A);
    KT_CHECK_EQ(phys, (uintptr_t)0);

    process_destroy(proc);
}

KTEST("proc-ensure-page-prot-none", "process",
      "process_ensure_page returns 0 for a PROT_NONE VMA",
      KT_FLAG_NONE, test_proc_ensure_page_prot_none);

/* ================================================================== */
/* Group 9: thread_count tracking                                       */
/* ================================================================== */

static void test_proc_thread_count_increments(ktest_ctx_t *ctx) {
    process_t *proc = process_create("thread-count");
    KT_ASSERT_NONNULL(proc);

    KT_CHECK_EQ((int)proc->thread_count, 0);

    thread_t *t = thread_create(proc, NULL);
    KT_ASSERT_NONNULL(t);
    KT_CHECK_EQ((int)proc->thread_count, 1);

    thread_t *t2 = thread_create(proc, NULL);
    KT_ASSERT_NONNULL(t2);
    KT_CHECK_EQ((int)proc->thread_count, 2);

    /* Manual cleanup: decrement count and destroy (not via reaper) */
    bool irq = spinlock_acquire(&proc->lock);
    proc->thread_count -= 2;
    spinlock_release(&proc->lock, irq);

    process_destroy(proc);
}

KTEST("proc-thread-count-increment", "process",
      "thread_create increments parent->thread_count on each call",
      KT_FLAG_CRITICAL, test_proc_thread_count_increments);

#endif /* KTEST_ENABLED */
