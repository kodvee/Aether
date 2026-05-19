#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Full definition in cpu.h; forward-declare here to break the circular dep */
struct regs;

/* ------------------------------------------------------------------ */
/* Panic categories                                                     */
/* ------------------------------------------------------------------ */

typedef enum {
    PANIC_GENERIC       = 0,
    PANIC_CPU_EXCEPTION,
    PANIC_PAGE_FAULT,
    PANIC_DOUBLE_FAULT,
    PANIC_ASSERT,
    PANIC_BUG,
    PANIC_OOM,
    PANIC_SMP,
    PANIC_HARDWARE,
} panic_category_t;

/* ------------------------------------------------------------------ */
/* Log severity                                                         */
/* ------------------------------------------------------------------ */

typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR,
} log_severity_t;

/* ------------------------------------------------------------------ */
/* Core panic entry point — do not call directly, use the macros        */
/* ------------------------------------------------------------------ */

__attribute__((noreturn))
void _kpanic_impl(panic_category_t cat, const char *subsys,
                  const char *file, int line, const char *msg,
                  struct regs *r, uintptr_t fault_addr, uint64_t err_code);

/* ------------------------------------------------------------------ */
/* Structured logging                                                   */
/* ------------------------------------------------------------------ */

void klog(log_severity_t sev, const char *subsys, const char *fmt, ...);

/* ------------------------------------------------------------------ */
/* Panic macros                                                         */
/* ------------------------------------------------------------------ */

/* Generic panic — no register context */
#define PANIC(msg) \
    _kpanic_impl(PANIC_GENERIC, "kernel", __FILE__, __LINE__, (msg), \
                 (struct regs *)0, (uintptr_t)0, (uint64_t)0)

/* Subsystem-tagged panic — no register context */
#define SUBSYS_PANIC(subsys, msg) \
    _kpanic_impl(PANIC_GENERIC, (subsys), __FILE__, __LINE__, (msg), \
                 (struct regs *)0, (uintptr_t)0, (uint64_t)0)

/* Panic with an interrupt register frame */
#define PANIC_REGS(msg, r) \
    _kpanic_impl(PANIC_GENERIC, "kernel", __FILE__, __LINE__, (msg), \
                 (r), (uintptr_t)0, (uint64_t)0)

/* CPU exception (non-fault — no meaningful fault address) */
#define EXCEPTION_PANIC(subsys, msg, r) \
    _kpanic_impl(PANIC_CPU_EXCEPTION, (subsys), __FILE__, __LINE__, (msg), \
                 (r), (uintptr_t)0, (r)->err_code)

/* Page fault — CR2 carries the faulting virtual address */
#define PAGE_FAULT_PANIC(r) \
    _kpanic_impl(PANIC_PAGE_FAULT, "cpu", __FILE__, __LINE__, "page fault", \
                 (r), (r)->cr2, (r)->err_code)

/* Double fault */
#define DOUBLE_FAULT_PANIC(r) \
    _kpanic_impl(PANIC_DOUBLE_FAULT, "cpu", __FILE__, __LINE__, "double fault", \
                 (r), (uintptr_t)0, (r)->err_code)

/* Assertion — evaluated at runtime, fires on failure */
#define KERNEL_ASSERT(cond) \
    do { \
        if (__builtin_expect(!(cond), 0)) \
            _kpanic_impl(PANIC_ASSERT, "kernel", __FILE__, __LINE__, \
                         "assertion failed: " #cond, \
                         (struct regs *)0, (uintptr_t)0, (uint64_t)0); \
    } while (0)

/* Bug — unreachable code path that was reached */
#define KERNEL_BUG(msg) \
    do { \
        _kpanic_impl(PANIC_BUG, "kernel", __FILE__, __LINE__, "BUG: " msg, \
                     (struct regs *)0, (uintptr_t)0, (uint64_t)0); \
        __builtin_unreachable(); \
    } while (0)

#define KERNEL_UNREACHABLE() KERNEL_BUG("unreachable code reached")

/* ------------------------------------------------------------------ */
/* Logging shorthands                                                   */
/* ------------------------------------------------------------------ */

#define KINFO(subsys, fmt, ...)  klog(LOG_INFO,  (subsys), (fmt), ##__VA_ARGS__)
#define KWARN(subsys, fmt, ...)  klog(LOG_WARN,  (subsys), (fmt), ##__VA_ARGS__)
#define KERROR(subsys, fmt, ...) klog(LOG_ERROR, (subsys), (fmt), ##__VA_ARGS__)
#define KDEBUG(subsys, fmt, ...) klog(LOG_DEBUG, (subsys), (fmt), ##__VA_ARGS__)
