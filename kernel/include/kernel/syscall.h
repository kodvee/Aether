#pragma once

#include <stdint.h>
#include <stddef.h>

/*
 * syscall_frame_t - GP register state saved by syscall_entry on every
 * SYSCALL instruction.
 *
 * Field offsets match the save/restore sequence in syscall.S exactly.
 * DO NOT reorder without updating the assembly.
 *
 *   rcx     = user RIP  (CPU stores it here on SYSCALL; SYSRETQ reads it back)
 *   r11     = user RFLAGS (same convention)
 *   user_rsp = user stack pointer, saved via %gs:8 before the stack switch
 */
typedef struct {
    uint64_t rax;       /* +0   syscall number on entry; return value on exit */
    uint64_t rbx;       /* +8   */
    uint64_t rcx;       /* +16  user RIP */
    uint64_t rdx;       /* +24  */
    uint64_t rsi;       /* +32  */
    uint64_t rdi;       /* +40  */
    uint64_t rbp;       /* +48  */
    uint64_t r8;        /* +56  */
    uint64_t r9;        /* +64  */
    uint64_t r10;       /* +72  */
    uint64_t r11;       /* +80  user RFLAGS */
    uint64_t r12;       /* +88  */
    uint64_t r13;       /* +96  */
    uint64_t r14;       /* +104 */
    uint64_t r15;       /* +112 */
    uint64_t user_rsp;  /* +120 */
} syscall_frame_t;

/* Syscall handler function type.  Receives the full register frame;
 * returns the value to place in rax (the user-visible return value). */
typedef uint64_t (*syscall_fn_t)(syscall_frame_t *f);

#define SYSCALL_MAX 512u

/*
 * syscall_init - program EFER/STAR/LSTAR/SFMASK for this CPU.
 * Must be called once per logical CPU, after the per-core GDT is live.
 */
void syscall_init(void);

/*
 * syscall_dispatch - called from syscall_entry (assembly) with a pointer
 * to the saved register frame.  Dispatches to the appropriate handler and
 * writes the return value back into frame->rax.
 */
void syscall_dispatch(syscall_frame_t *f);

/*
 * syscall_register - install a handler for the given syscall number.
 * Safe to call any time before the first user process runs.
 */
void syscall_register(uint64_t nr, syscall_fn_t fn);

/*
 * syscalls_init - register all built-in syscall handlers.
 * Called once from kernel.c during boot, before scheduler_enter().
 */
void syscalls_init(void);
