.section .text
.align 4

/*
 * context_switch - cooperative kernel context switch
 *
 * void context_switch(context_regs_t *outgoing, context_regs_t *incoming)
 *   %rdi = outgoing (context_regs_t *)
 *   %rsi = incoming (context_regs_t *)
 *
 * Saves callee-saved registers of the outgoing thread into *outgoing, then
 * restores them from *incoming and jumps to incoming->rip.
 *
 * context_regs_t layout (byte offsets):
 *   0  rip    8  rsp   16 rbx   24 rbp
 *  32  r12   40 r13   48 r14   56 r15
 *
 * For the outgoing thread:
 *   - rip is set to the return address (the caller resumes here on next switch-in)
 *   - rsp is set to the caller's rsp BEFORE the return address (8(%rsp) at call time)
 *     so that when we restore rsp and jump to rip it looks like a normal return
 *
 * For a brand-new thread:
 *   - rip -> thread_entry_trampoline
 *   - rsp -> kstack_top (empty 16-byte-aligned stack)
 *   - r12 -> thread entry function (trampoline reads this)
 */
.global context_switch
.type   context_switch, @function
context_switch:
    /* --- save outgoing --- */
    mov    (%rsp),   %rax
    mov    %rax,     0(%rdi)    /* outgoing->rip = return address           */

    lea    8(%rsp),  %rax
    mov    %rax,     8(%rdi)    /* outgoing->rsp = caller rsp past ret addr */

    mov    %rbx,    16(%rdi)
    mov    %rbp,    24(%rdi)
    mov    %r12,    32(%rdi)
    mov    %r13,    40(%rdi)
    mov    %r14,    48(%rdi)
    mov    %r15,    56(%rdi)

    /* --- restore incoming --- */
    mov    16(%rsi), %rbx
    mov    24(%rsi), %rbp
    mov    32(%rsi), %r12
    mov    40(%rsi), %r13
    mov    48(%rsi), %r14
    mov    56(%rsi), %r15
    mov     8(%rsi), %rsp       /* switch to incoming's kernel stack        */

    jmp    *0(%rsi)             /* jump to incoming->rip                    */

/*
 * thread_entry_trampoline - first-run entry stub for newly created threads.
 *
 * Jumped to by context_switch when a thread runs for the first time.
 * r12 holds the thread's entry function address (set during thread_create).
 *
 * Enables interrupts before calling the entry function so the thread runs
 * in normal kernel context regardless of whether it was first scheduled
 * from interrupt context (LAPIC preemption) or from scheduler_enter().
 *
 * If the entry function returns, falls through to thread_exit().
 */
.global thread_entry_trampoline
.type   thread_entry_trampoline, @function
thread_entry_trampoline:
    sti                 /* re-enable interrupts (may have been off in IRQ) */
    call   *%r12        /* call thread entry function                      */
    call   thread_exit  /* entry returned - clean up                       */
    ud2                 /* unreachable                                     */

/*
 * context_enter - one-way context switch (load only, no save).
 *
 * void context_enter(context_regs_t *incoming)
 *   %rdi = incoming (context_regs_t *)
 *
 * Restores callee-saved registers and RSP from *incoming, then jumps to
 * incoming->rip.  No outgoing context is saved.
 *
 * Used in scheduler_enter() and the current==NULL path of schedule() where
 * there is no outgoing thread context to preserve.  Using context_switch with
 * a stack-local dummy there is dangerous: the dummy captures the return
 * address (which is a KERNEL_BUG), and if something ever restores from it
 * the kernel panics.
 */
.global context_enter
.type   context_enter, @function
context_enter:
    mov    16(%rdi), %rbx
    mov    24(%rdi), %rbp
    mov    32(%rdi), %r12
    mov    40(%rdi), %r13
    mov    48(%rdi), %r14
    mov    56(%rdi), %r15
    mov     8(%rdi), %rsp
    jmp    *0(%rdi)

/*
 * user_thread_trampoline - first-run entry for user-mode threads.
 *
 * Jumped to by context_switch when a user thread is scheduled for the first
 * time.  Register state loaded from the thread's context_regs_t by
 * context_switch (see thread_create_user in scheduler.c):
 *
 *   r12 = user RIP  (ELF e_entry)
 *   r13 = user RSP  (initial user stack pointer)
 *   r14 = CR3 value (process pagemap physical address)
 *
 * Kernel higher-half mappings are present in the user pagemap (copied from
 * mmu_kernel_pagemap by vmm_new_pagemap), so the kernel stack and code
 * remain accessible after the CR3 switch until IRETQ drops to ring-3.
 *
 * IRETQ frame (64-bit inter-privilege, 5 × 8 bytes, pushed in reverse):
 *   SS    = 0x1B (SEG_UDATA | RPL3)
 *   RSP   = user stack pointer
 *   RFLAGS= 0x202 (IF=1, reserved bit 1 set)
 *   CS    = 0x23 (SEG_UCODE | RPL3)
 *   RIP   = user entry point
 */
.global user_thread_trampoline
.type   user_thread_trampoline, @function
user_thread_trampoline:
    sti                     /* re-enable interrupts: on a valid kstack now  */
    mov    %r14, %cr3       /* switch to user pagemap                       */

    push   $0x1b            /* SS   = SEG_UDATA | 3                         */
    push   %r13             /* RSP  = user stack pointer                    */
    pushq  $0x202           /* RFLAGS: IF=1, bit 1 reserved (must be set)  */
    push   $0x23            /* CS   = SEG_UCODE | 3                         */
    push   %r12             /* RIP  = user entry point                      */
    iretq
