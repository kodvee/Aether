#pragma once

#include <stdint.h>

/* Segment selectors in the per-core GDT (RPL=3 for user descriptors) */
#define SEG_KCODE  0x08   /* kernel code  64-bit                     */
#define SEG_KDATA  0x10   /* kernel data                             */
#define SEG_UDATA  0x1B   /* user data    (index 3, RPL=3)           */
#define SEG_UCODE  0x23   /* user code    (index 4, RPL=3)           */
#define SEG_TSS    0x28   /* TSS          (16-byte system descriptor) */

/*
 * x86-64 Task State Segment.
 *
 * rsp0 is written by the scheduler on every context switch to the incoming
 * thread's kstack_top; the CPU loads it into RSP on ring-3 -> ring-0
 * transitions (hardware interrupts, SYSCALL).
 */
typedef struct __attribute__((packed)) {
    uint32_t reserved0;
    uint64_t rsp0;          /* ring-0 stack pointer                  */
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];        /* IST1-7 (not used yet)                 */
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;   /* offset to I/O permission bitmap       */
} tss_t;

/*
 * Per-core GDT layout (7 descriptors = 56 bytes):
 *
 *   entries[0]  0x00  null
 *   entries[1]  0x08  kernel code  64-bit  (access=0x9A, L=1)
 *   entries[2]  0x10  kernel data          (access=0x92)
 *   entries[3]  0x18  user data            (access=0xF2)  <- data BEFORE code
 *   entries[4]  0x20  user code   64-bit  (access=0xFA, L=1)
 *   tss_lo      0x28  TSS low 8 bytes  \  16-byte system
 *   tss_hi      0x30  TSS high 8 bytes /  descriptor
 *
 * User data (0x18) is placed before user code (0x20) so SYSRET works:
 *   STAR[63:48] = 0x10  ->  CS = 0x10+16 = 0x20 (+RPL=3 -> 0x23)
 *                           SS = 0x10+8  = 0x18 (+RPL=3 -> 0x1B)
 */
typedef struct {
    uint64_t entries[5];   /* null, kcode, kdata, udata, ucode        */
    uint64_t tss_lo;       /* bits [63:0] of 16-byte TSS descriptor   */
    uint64_t tss_hi;       /* bits [127:64]                           */
} core_gdt_t;

void gdt_init(void);

/*
 * gdt_load_core - build and load the per-core GDT and TSS for cpu.
 *
 * Fills cpu->gdt and cpu->tss, executes lgdt, reloads CS via lretq,
 * reloads DS/ES/SS/FS/GS, and calls ltr SEG_TSS.
 *
 * Must be called after cpu->tss storage is stable (i.e. after core_t
 * fields are set and cpu points to permanent storage).
 * set_gs_register() must be called immediately after to configure GSBASE.
 */
struct core;
void gdt_load_core(struct core *cpu);
