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
