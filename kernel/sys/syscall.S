.section .text
.align 4

/*
 * syscall_entry - x86-64 SYSCALL entry point (LSTAR target).
 *
 * CPU state on entry:
 *   RCX    = user RIP  (saved by the SYSCALL instruction)
 *   R11    = user RFLAGS (saved by the SYSCALL instruction)
 *   RSP    = user stack pointer (NOT switched by hardware for SYSCALL)
 *   CS/SS  = kernel code/data selectors from STAR
 *   IF=0, TF=0, DF=0 (cleared by SFMASK)
 *
 * %gs offsets inside core_t (must match cpu.h):
 *   %gs:0   syscall_ksp     — kernel stack pointer for this thread
 *   %gs:8   ustack_scratch  — one-word scratch for saving the user RSP
 *
 * syscall_frame_t layout on the kernel stack (must match syscall.h):
 *   0(%rsp)   rax       syscall number / return value
 *   8         rbx
 *   16        rcx       user RIP
 *   24        rdx
 *   32        rsi
 *   40        rdi
 *   48        rbp
 *   56        r8
 *   64        r9
 *   72        r10
 *   80        r11       user RFLAGS
 *   88        r12
 *   96        r13
 *   104       r14
 *   112       r15
 *   120       user_rsp
 */
.global syscall_entry
.type   syscall_entry, @function
syscall_entry:
    /* Step 1: swap GS so %gs points to the kernel core_t.
     * In ring-3, GSBASE holds user TLS (or 0); KERNELGSBASE holds core_t.
     * After swapgs, GSBASE = core_t, KERNELGSBASE = user GS. */
    swapgs

    /* Step 2: save user RSP in the per-cpu scratch cell, switch to
     * the kernel stack.  From this point the stack is valid. */
    mov  %rsp, %gs:8          /* ustack_scratch = user rsp */
    mov  %gs:0, %rsp          /* rsp = syscall_ksp (= current thread's kstack_top) */

    /* Step 3: re-enable interrupts.  We are now on the kernel stack so
     * any IRQ that fires will land on a valid stack frame. */
    sti

    /* Step 4: build syscall_frame_t (128 bytes) on the kernel stack. */
    sub  $128, %rsp

    mov  %rax,  0(%rsp)       /* rax: syscall number */
    mov  %rbx,  8(%rsp)
    mov  %rcx, 16(%rsp)       /* rcx: user RIP */
    mov  %rdx, 24(%rsp)
    mov  %rsi, 32(%rsp)
    mov  %rdi, 40(%rsp)
    mov  %rbp, 48(%rsp)
    mov  %r8,  56(%rsp)
    mov  %r9,  64(%rsp)
    mov  %r10, 72(%rsp)
    mov  %r11, 80(%rsp)       /* r11: user RFLAGS */
    mov  %r12, 88(%rsp)
    mov  %r13, 96(%rsp)
    mov  %r14, 104(%rsp)
    mov  %r15, 112(%rsp)

    /* user_rsp is in the scratch cell; move it into the frame.
     * Clobber %rax temporarily — it will be overwritten by the return value. */
    mov  %gs:8, %rax
    mov  %rax, 120(%rsp)      /* user_rsp */

    /* Step 5: dispatch.  Arg = pointer to the frame (SysV ABI: %rdi). */
    mov  %rsp, %rdi
    cld
    call syscall_dispatch      /* modifies frame->rax in place */

    /* Step 6: disable interrupts before touching user-facing state. */
    cli

    /* Step 7: restore all registers from the frame. */
    mov   0(%rsp), %rax
    mov   8(%rsp), %rbx
    mov  16(%rsp), %rcx       /* user RIP   -> rcx for SYSRETQ */
    mov  24(%rsp), %rdx
    mov  32(%rsp), %rsi
    mov  40(%rsp), %rdi
    mov  48(%rsp), %rbp
    mov  56(%rsp), %r8
    mov  64(%rsp), %r9
    mov  72(%rsp), %r10
    mov  80(%rsp), %r11       /* user RFLAGS -> r11 for SYSRETQ */
    mov  88(%rsp), %r12
    mov  96(%rsp), %r13
    mov 104(%rsp), %r14
    mov 112(%rsp), %r15
    mov 120(%rsp), %rsp       /* restore user RSP — must be last stack access */

    /* Step 8: swap GS back to user GS before returning to ring-3. */
    swapgs

    /* Step 9: return to user space. */
    sysretq
