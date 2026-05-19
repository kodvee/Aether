#pragma once
#include <stdint.h>

/*
 * x86-64 frame record as laid down by the ABI when -fno-omit-frame-pointer
 * is in effect (the compiler default for kernel builds):
 *
 *   push rbp
 *   mov  rbp, rsp
 *
 * From any function, rbp -> { saved_caller_rbp, return_rip }.
 */
typedef struct stack_frame {
    struct stack_frame *rbp;
    uint64_t            rip;
} stack_frame_t;

/* struct regs is defined in cpu.h; forward-declare to avoid the header pull */
struct regs;

/*
 * stacktrace_print_from — core frame walker.
 *
 * Prints a numbered backtrace beginning at @rip, then following the frame
 * chain rooted at @rbp.  Symbol names are resolved through the global kelf
 * image.  No dynamic allocation; safe in interrupt and panic contexts.
 */
void stacktrace_print_from(uintptr_t rip, uintptr_t rbp);

/*
 * stacktrace_print — capture the current frame pointer and walk from the
 * call site.  The function's own frame is skipped so frame #0 is the caller.
 */
void stacktrace_print(void);

/*
 * stacktrace_print_regs — walk starting from the RIP/RBP saved in an
 * interrupt register context.  Falls back to stacktrace_print when r is NULL.
 */
void stacktrace_print_regs(struct regs *r);
