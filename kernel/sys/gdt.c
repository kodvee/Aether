/**
 * gdt.c - GDT management
 *
 * gdt_init()     sets up a minimal flat 5-entry GDT used by the BSP
 *                during early boot (before per-core GDTs are loaded).
 *
 * gdt_load_core() builds a per-core 7-entry GDT that embeds the core's
 *                TSS descriptor, loads it, and calls ltr to arm the TSS.
 *                Called once per core from core_start() in smp.c.
 */

#include <stdint.h>
#include <stddef.h>
#include <kernel/gdt.h>
#include <kernel/cpu.h>
#include <kernel/macros.h>

/* Flat 64-bit segment descriptor as a raw uint64_t.
 * access: access byte (bits [47:40])
 * flags:  upper nibble of byte 6  (bits [55:52]); L=1 → 0x2 for 64-bit code */
static inline uint64_t seg64(uint8_t access, uint8_t flags) {
    return ((uint64_t)access << 40) | ((uint64_t)(flags & 0xF) << 52);
}

/* ------------------------------------------------------------------ */
/* Early-boot global GDT (BSP, before per-core GDTs)                  */
/* ------------------------------------------------------------------ */

static uint64_t boot_gdt[5];

struct gdtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct gdtr boot_gdtr;

void __init gdt_init(void) {
    boot_gdt[0] = 0;                   /* null            */
    boot_gdt[1] = seg64(0x9A, 0x2);   /* kernel code 64  */
    boot_gdt[2] = seg64(0x92, 0x0);   /* kernel data     */
    boot_gdt[3] = seg64(0xFA, 0x2);   /* user code 64    */
    boot_gdt[4] = seg64(0xF2, 0x0);   /* user data       */

    boot_gdtr.limit = sizeof(boot_gdt) - 1;
    boot_gdtr.base  = (uint64_t)boot_gdt;

    asm volatile (
        "lgdt %0\n\t"
        "push $0x8\n\t"
        "lea 1f(%%rip), %%rax\n\t"
        "push %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        "mov $0x10, %%eax\n\t"
        "mov %%eax, %%ds\n\t"
        "mov %%eax, %%es\n\t"
        "mov %%eax, %%fs\n\t"
        "mov %%eax, %%gs\n\t"
        "mov %%eax, %%ss\n\t"
        :
        : "m"(boot_gdtr)
        : "rax", "memory"
    );
}

/* ------------------------------------------------------------------ */
/* Per-core GDT + TSS                                                  */
/* ------------------------------------------------------------------ */

void gdt_load_core(struct core *cpu) {
    core_t    *c = (core_t *)cpu;
    core_gdt_t *g = &c->gdt;
    tss_t      *t = &c->tss;

    /* Standard flat segments — same layout as boot_gdt except user data
     * (0x18) and user code (0x20) are swapped for SYSRET compatibility. */
    g->entries[0] = 0;                   /* null           0x00 */
    g->entries[1] = seg64(0x9A, 0x2);   /* kernel code    0x08 */
    g->entries[2] = seg64(0x92, 0x0);   /* kernel data    0x10 */
    g->entries[3] = seg64(0xF2, 0x0);   /* user data      0x18 */
    g->entries[4] = seg64(0xFA, 0x2);   /* user code      0x20 */

    /* Zero TSS; disable IOPB by pointing it past the TSS end */
    *t = (tss_t){ .iopb_offset = sizeof(tss_t) };

    /* Encode the TSS descriptor (16-byte system descriptor at 0x28) */
    uintptr_t base  = (uintptr_t)t;
    uint16_t  limit = sizeof(tss_t) - 1;

    g->tss_lo = (uint64_t)limit
              | ((base & 0x00FFFFFFULL) << 16)
              | (0x89ULL              << 40)   /* P=1, DPL=0, type=9 (TSS avail) */
              | (((base >> 24) & 0xFFULL) << 56);
    g->tss_hi = (base >> 32) & 0xFFFFFFFFULL;

    struct gdtr gdtr = {
        .limit = sizeof(core_gdt_t) - 1,
        .base  = (uint64_t)g,
    };

    asm volatile (
        "lgdt %0\n\t"
        /* Far return to reload CS = SEG_KCODE (0x08) */
        "push $0x8\n\t"
        "lea 1f(%%rip), %%rax\n\t"
        "push %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        "mov $0x10, %%eax\n\t"
        "mov %%eax, %%ds\n\t"
        "mov %%eax, %%es\n\t"
        "mov %%eax, %%ss\n\t"
        "xor %%eax, %%eax\n\t"
        "mov %%eax, %%fs\n\t"
        "mov %%eax, %%gs\n\t"
        :
        : "m"(gdtr)
        : "rax", "memory"
    );

    /* Load the TSS so the CPU knows where to find rsp0 */
    asm volatile ("ltr %%ax" : : "a"((uint16_t)SEG_TSS));
}
