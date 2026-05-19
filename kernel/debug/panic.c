/**
 * panic.c - unified kernel diagnostics and panic subsystem.
 *
 * Stages on fatal entry:
 *   1. Disable interrupts, capture APIC ID via CPUID.
 *   2. Atomic ownership claim (one CPU renders, others halt).
 *   3. SMP freeze: broadcast halt IPI to all other cores.
 *   4. Initialise serial independently (safe before printf_init).
 *   5. Depth-gated rendering:
 *        depth 1 - full render to framebuffer + serial.
 *        depth 2 - emergency serial-only (framebuffer state unreliable).
 *        depth >= 3 - halt immediately, system is too broken to render.
 *   6. Final halt loop.
 *
 * All paths are allocation-free and spinlock-free.  No kprintf is called;
 * output goes directly to flanterm (via flanterm_write) and COM1 (via
 * outportb polling).
 */

#include <kernel/panic.h>
#include <kernel/kprintf.h>
#include <kernel/elf.h>
#include <kernel/stacktrace.h>
#include <kernel/apic.h>
#include <kernel/ports.h>
#include <kernel/cpu.h>
#include <kernel/mmu.h>        /* for msr.h pull-through; no alloc used */
#include <kernel/version.h>
#include <deps/flanterm.h>
#include <deps/printf.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>

#ifdef KTEST_ENABLED
#include <kernel/ktest.h>
#endif

/* Total SMP core count - set by smp_init */
extern uint64_t coreCount;

/* ------------------------------------------------------------------ */
/* Panic state                                                          */
/* ------------------------------------------------------------------ */

#define PANIC_NO_OWNER 0xFFFFFFFFu

/* APIC ID of the CPU that won the ownership race; 0xFFFFFFFF = nobody */
static volatile uint32_t g_panic_owner = PANIC_NO_OWNER;

/* How many times the owning CPU has re-entered _kpanic_impl */
static volatile uint32_t g_panic_depth = 0;

/* ------------------------------------------------------------------ */
/* Serial (COM1) helpers - polling, interrupt-free                      */
/* ------------------------------------------------------------------ */

#define COM1 0x3F8u

static void _ensure_serial(void) {
    outportb(COM1 + 1, 0x00); /* disable UART interrupts */
    outportb(COM1 + 3, 0x80); /* DLAB on */
    outportb(COM1 + 0, 0x03); /* divisor low  -> 38400 baud */
    outportb(COM1 + 1, 0x00); /* divisor high */
    outportb(COM1 + 3, 0x03); /* 8-N-1, DLAB off */
    outportb(COM1 + 2, 0xC7); /* enable FIFO */
    outportb(COM1 + 4, 0x0B); /* RTS/DSR */
}

static void _serial_putc(char c) {
    /* Busy-wait for transmit holding register empty (LSR bit 5) */
    while (!(inportb(COM1 + 5) & 0x20u))
        asm volatile ("pause");
    outportb(COM1, (unsigned char)c);
}

static void _serial_write(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\n') _serial_putc('\r');
        _serial_putc(s[i]);
    }
}

static void _serial_puts(const char *s) {
    while (*s) {
        if (*s == '\n') _serial_putc('\r');
        _serial_putc(*s++);
    }
}

/* ------------------------------------------------------------------ */
/* Panic output - direct to framebuffer (no spinlock) + serial         */
/* ------------------------------------------------------------------ */

/* g_serial_only: set to true when depth >= 2 (framebuffer unreliable) */
static bool g_serial_only = false;

static void _panic_write(const char *buf, size_t n) {
    if (!g_serial_only) {
        struct flanterm_context *ctx = printf_get_context();
        if (ctx) flanterm_write(ctx, buf, n);
    }
    _serial_write(buf, n);
}

static void _panic_puts(const char *s) {
    size_t n = 0;
    const char *p = s;
    while (*p++) n++;
    _panic_write(s, n);
}

static void _panic_printf(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0)
        _panic_write(buf, (size_t)n);
}

/* ------------------------------------------------------------------ */
/* APIC ID - CPUID leaf 1, no GS segment dependency                    */
/* ------------------------------------------------------------------ */

static uint32_t _get_apic_id(void) {
    uint32_t ebx;
    asm volatile ("cpuid" : "=b"(ebx) : "a"(1u) : "ecx", "edx", "memory");
    return (ebx >> 24) & 0xFFu;
}

/* ------------------------------------------------------------------ */
/* SMP freeze - broadcast halt IPI to all other cores                  */
/* ------------------------------------------------------------------ */

static void _smp_freeze(void) {
    if (lapic_initialized)
        lapic_issue_ipi(0, 255, 3, 0); /* shorthand=3 = all-excluding-self */
}

/* ------------------------------------------------------------------ */
/* Exception name table (vectors 0-31)                                  */
/* ------------------------------------------------------------------ */

static const char *const g_exc_names[32] = {
    "divide error",               /* #DE  0  */
    "debug",                      /* #DB  1  */
    "non-maskable interrupt",     /* NMI  2  */
    "breakpoint",                 /* #BP  3  */
    "overflow",                   /* #OF  4  */
    "bound range exceeded",       /* #BR  5  */
    "invalid opcode",             /* #UD  6  */
    "device not available",       /* #NM  7  */
    "double fault",               /* #DF  8  */
    "coprocessor segment overrun",/*      9  */
    "invalid TSS",                /* #TS  10 */
    "segment not present",        /* #NP  11 */
    "stack-segment fault",        /* #SS  12 */
    "general protection fault",   /* #GP  13 */
    "page fault",                 /* #PF  14 */
    "reserved",                   /*      15 */
    "x87 FPU error",              /* #MF  16 */
    "alignment check",            /* #AC  17 */
    "machine check",              /* #MC  18 */
    "SIMD floating-point",        /* #XM  19 */
    "virtualisation exception",   /* #VE  20 */
    "control protection",         /* #CP  21 */
    "reserved",                   /*      22 */
    "reserved",                   /*      23 */
    "reserved",                   /*      24 */
    "reserved",                   /*      25 */
    "reserved",                   /*      26 */
    "reserved",                   /*      27 */
    "hypervisor injection",       /* #HV  28 */
    "VMM communication",          /* #VC  29 */
    "security exception",         /* #SX  30 */
    "reserved",                   /*      31 */
};

static const char *const g_category_names[] = {
    "generic",
    "cpu exception",
    "page fault",
    "double fault",
    "assertion",
    "bug",
    "out of memory",
    "smp",
    "hardware",
};

/* ------------------------------------------------------------------ */
/* Register dump                                                        */
/* ------------------------------------------------------------------ */

static void _dump_regs(const struct regs *r) {
    _panic_puts("Registers:\n");
    _panic_printf("  rip=0x%016lx  cs=0x%04lx  rflags=0x%016lx\n",
                  r->rip, r->cs & 0xFFFF, r->rflags);
    _panic_printf("  rsp=0x%016lx  ss=0x%04lx\n",
                  r->rsp, r->ss & 0xFFFF);
    _panic_printf("  rax=0x%016lx  rbx=0x%016lx  rcx=0x%016lx  rdx=0x%016lx\n",
                  r->rax, r->rbx, r->rcx, r->rdx);
    _panic_printf("  rdi=0x%016lx  rsi=0x%016lx  rbp=0x%016lx\n",
                  r->rdi, r->rsi, r->rbp);
    _panic_printf("  r8 =0x%016lx  r9 =0x%016lx  r10=0x%016lx  r11=0x%016lx\n",
                  r->r8, r->r9, r->r10, r->r11);
    _panic_printf("  r12=0x%016lx  r13=0x%016lx  r14=0x%016lx  r15=0x%016lx\n",
                  r->r12, r->r13, r->r14, r->r15);
    _panic_printf("  ds=0x%04lx  es=0x%04lx  fs=0x%04lx  gs=0x%04lx\n",
                  r->ds & 0xFFFF, r->es & 0xFFFF,
                  r->fs & 0xFFFF, r->gs & 0xFFFF);
    _panic_printf("  cr2=0x%016lx  int=0x%02lx  err=0x%08lx\n",
                  r->cr2, r->int_no, r->err_code);
}

/* ------------------------------------------------------------------ */
/* Page-fault error code decoder                                        */
/* ------------------------------------------------------------------ */

static void _decode_pf(uintptr_t addr, uint64_t err) {
    _panic_printf("Page fault at 0x%016lx\n", (uint64_t)addr);
    _panic_printf("  Cause: %s, %s, %s%s%s\n",
        (err & 1) ? "protection violation" : "page not present",
        (err & 2) ? "write"    : "read",
        (err & 4) ? "user "    : "kernel ",
        (err & 8) ? "reserved-bit-set " : "",
        (err & 16) ? "instruction-fetch" : "");
}

/* ------------------------------------------------------------------ */
/* Panic-safe inline stack walker (no kprintf)                          */
/* ------------------------------------------------------------------ */

static inline bool _valid_kaddr(uintptr_t a) {
    return (a >> 48) == 0xFFFFuL && (a & 7u) == 0;
}

static void _panic_stacktrace(uintptr_t rip, uintptr_t rbp) {
    _panic_puts("Stack trace:\n");
    for (int depth = 0; depth < 64; depth++) {
        if (rip == 0) break;

        const char      *name = NULL;
        const Elf64_Sym *sym  = elf_sym_by_addr(&kelf, rip, &name);

        if (sym && name && name[0] != '\0') {
            _panic_printf("  #%-2d  %p  %s+0x%lx\n",
                          depth, (void *)rip, name,
                          (uint64_t)(rip - sym->st_value));
        } else {
            _panic_printf("  #%-2d  %p  ???\n", depth, (void *)rip);
        }

        if (!_valid_kaddr(rbp)) break;
        const stack_frame_t *frame = (const stack_frame_t *)rbp;
        uintptr_t next_rip = frame->rip;
        uintptr_t next_rbp = (uintptr_t)frame->rbp;
        if (next_rip == 0 || next_rbp <= rbp) break;
        rip = next_rip;
        rbp = next_rbp;
    }
}

/* ------------------------------------------------------------------ */
/* Full panic renderer (depth == 1)                                     */
/* ------------------------------------------------------------------ */

static void _render_full(panic_category_t cat, const char *subsys,
                         const char *file, int line, const char *msg,
                         const struct regs *r, uintptr_t fault_addr,
                         uint64_t err_code, uint32_t apic_id,
                         uint32_t depth)
{
    struct flanterm_context *fb_ctx = printf_get_context();

    /* Red screen via ANSI - framebuffer only */
    if (fb_ctx) {
        static const char red_esc[] = "\033[0;41m\033[2J\033[H";
        flanterm_write(fb_ctx, red_esc, sizeof(red_esc) - 1);
    }

    /* Serial banner */
    _serial_puts("\r\n");
    _serial_puts("============================================================\r\n");
    _serial_puts("                      KERNEL PANIC                          \r\n");
    _serial_puts("============================================================\r\n");

    /* Framebuffer header */
    if (fb_ctx) {
        static const char hdr[] = "\n  *** KERNEL PANIC ***\n\n";
        flanterm_write(fb_ctx, hdr, sizeof(hdr) - 1);
    }

    /* Kernel version and build info */
    _panic_printf("Kernel   : %s %d.%d.%d-%s (%s)\n",
                  __kernel_name,
                  __kernel_version_major, __kernel_version_minor,
                  __kernel_version_lower, __kernel_version_suffix,
                  __kernel_arch);
    _panic_printf("Built    : %s %s  (%s)\n",
                  __kernel_build_date, __kernel_build_time,
                  __kernel_compiler_version);

    /* CPU info (only if cpuinfo_init has run) */
    {
        cpu_info_t *ci = cpu_info;
        if (ci && ci->vendorId && ci->cpuName) {
            _panic_printf("CPU      : %s - %s  (%u cores active, %lu total)\n",
                          ci->vendorId, ci->cpuName, ci->coreCount, coreCount);
        }
    }
    _panic_puts("\n");

    /* Category and source */
    const char *catname = (cat < (panic_category_t)9)
                          ? g_category_names[(int)cat] : "unknown";
    _panic_printf("Category : %s\n", catname);
    _panic_printf("Subsystem: %s\n", subsys ? subsys : "?");
    _panic_printf("Location : %s:%d\n", file ? file : "?", line);

    if (msg && msg[0])
        _panic_printf("Message  : %s\n", msg);

    /* For CPU exception, show the exception name from int_no */
    if (r && (cat == PANIC_CPU_EXCEPTION || cat == PANIC_PAGE_FAULT ||
              cat == PANIC_DOUBLE_FAULT)) {
        uint64_t vec = r->int_no;
        const char *exc = (vec < 32) ? g_exc_names[vec] : "unknown";
        _panic_printf("Exception: #%lu - %s\n", vec, exc);
    }

    _panic_printf("Core     : APIC %u  (depth %u, %lu cores online)\n",
                  apic_id, depth, coreCount);
    _panic_puts("\n");

    /* Page fault specific decoding */
    if (cat == PANIC_PAGE_FAULT && fault_addr) {
        _decode_pf(fault_addr, err_code);
        _panic_puts("\n");
    }

    /* Register dump */
    if (r) {
        _dump_regs(r);
        _panic_puts("\n");
    }

    /* Stack trace */
    if (r) {
        _panic_stacktrace(r->rip, r->rbp);
    } else {
        uintptr_t rbp;
        asm volatile ("movq %%rbp, %0" : "=r"(rbp));
        if (_valid_kaddr(rbp)) {
            const stack_frame_t *f = (const stack_frame_t *)rbp;
            _panic_stacktrace(f->rip, (uintptr_t)f->rbp);
        } else {
            _panic_puts("Stack trace: [no valid frame pointer]\n");
        }
    }

    _panic_puts("\nSystem halted.\n");
}

/* ------------------------------------------------------------------ */
/* Emergency renderer (depth == 2) - serial only                        */
/* ------------------------------------------------------------------ */

static void _render_emergency(panic_category_t cat, const char *subsys,
                               const char *file, int line, const char *msg,
                               uint32_t apic_id)
{
    (void)cat;
    _serial_puts("\r\n=== NESTED PANIC (emergency) ===\r\n");
    _serial_puts("Subsystem: ");
    _serial_puts(subsys ? subsys : "?");
    _serial_puts("\r\nLocation : ");
    _serial_puts(file ? file : "?");
    _serial_putc(':');
    /* Print line number without printf */
    char lbuf[12];
    int li = 10;
    lbuf[11] = '\0';
    int tmp = line;
    if (tmp == 0) { lbuf[li--] = '0'; }
    while (tmp > 0) { lbuf[li--] = (char)('0' + tmp % 10); tmp /= 10; }
    _serial_puts(lbuf + li + 1);
    _serial_puts("\r\nMessage  : ");
    _serial_puts(msg ? msg : "(none)");
    _serial_puts("\r\nCore APIC: ");
    char abuf[12];
    int ai = 10;
    abuf[11] = '\0';
    uint32_t atmp = apic_id;
    if (atmp == 0) { abuf[ai--] = '0'; }
    while (atmp > 0) { abuf[ai--] = (char)('0' + atmp % 10); atmp /= 10; }
    _serial_puts(abuf + ai + 1);
    _serial_puts("\r\nSystem halted (nested panic).\r\n");
}

/* ------------------------------------------------------------------ */
/* Expected-panic hook for the test framework                           */
/* ------------------------------------------------------------------ */

#ifdef KTEST_ENABLED
static void _ktest_check_expected(const char *msg) {
    volatile ktest_ctx_t *ctx = g_ktest_active_ctx;
    if (!ctx || !ctx->expect_panic) return;

    /* If a substring match is required, verify it */
    if (ctx->expected_panic_msg && msg) {
        if (!strstr(msg, ctx->expected_panic_msg)) return; /* mismatch -> real panic */
    }

    /* Capture message into ctx */
    const char *src = msg ? msg : "(panic)";
    size_t i = 0;
    while (src[i] && i + 1 < sizeof(ctx->caught_msg)) {
        ((ktest_ctx_t *)ctx)->caught_msg[i] = src[i];
        i++;
    }
    ((ktest_ctx_t *)ctx)->caught_msg[i] = '\0';

    ((ktest_ctx_t *)ctx)->panic_caught = true;
    ((ktest_ctx_t *)ctx)->expect_panic = false;
    ((ktest_ctx_t *)ctx)->result       = KT_PANIC_EXPECTED;

    /* Reset panic ownership so future panics (including nested expected ones) work */
    __atomic_store_n((uint32_t *)&g_panic_owner, PANIC_NO_OWNER, __ATOMIC_SEQ_CST);
    __atomic_store_n((uint32_t *)&g_panic_depth, 0u,             __ATOMIC_SEQ_CST);

    /* Restore interrupts (we're about to leave _kpanic_impl via longjmp) */
    asm volatile ("sti" ::: "memory");

    __builtin_longjmp(((ktest_ctx_t *)ctx)->recovery_buf, 1);
    __builtin_unreachable();
}
#endif

/* ------------------------------------------------------------------ */
/* Public entry point                                                   */
/* ------------------------------------------------------------------ */

__attribute__((noreturn))
void _kpanic_impl(panic_category_t cat, const char *subsys,
                  const char *file, int line, const char *msg,
                  struct regs *r, uintptr_t fault_addr, uint64_t err_code)
{
    /* Step 1: Disable interrupts immediately */
    asm volatile ("cli" ::: "memory");

#ifdef KTEST_ENABLED
    /* If a test is expecting this panic, recover into it instead of halting */
    _ktest_check_expected(msg);
#endif

    /* Step 2: Identify ourselves */
    uint32_t my_id = _get_apic_id();

    /* Step 3: Claim atomic ownership */
    uint32_t expected = PANIC_NO_OWNER;
    bool owner = __atomic_compare_exchange_n(
        (uint32_t *)&g_panic_owner, &expected, my_id,
        false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);

    if (!owner && expected != my_id) {
        /* Another CPU got there first - just halt */
        asm volatile ("1: hlt; jmp 1b" ::: "memory");
        __builtin_unreachable();
    }

    /* Increment depth (owner re-entering = nested panic) */
    uint32_t depth = __atomic_add_fetch((uint32_t *)&g_panic_depth, 1u,
                                        __ATOMIC_SEQ_CST);

    if (depth >= 3u) {
        /* Too broken to render anything safely */
        _serial_puts("\r\n!!! triple panic - halting immediately !!!\r\n");
        asm volatile ("1: hlt; jmp 1b" ::: "memory");
        __builtin_unreachable();
    }

    /* Step 4: Freeze other CPUs */
    _smp_freeze();

    /* Step 5: Ensure serial is ready (safe to call multiple times) */
    _ensure_serial();

    if (depth == 2u) {
        /* Step 6a: Emergency serial-only render */
        g_serial_only = true;
        _render_emergency(cat, subsys, file, line, msg, my_id);
    } else {
        /* Step 6b: Full render */
        _render_full(cat, subsys, file, line, msg, r,
                     fault_addr, err_code, my_id, depth);
    }

    /* Step 7: Final halt */
    asm volatile ("1: hlt; jmp 1b" ::: "memory");
    __builtin_unreachable();
}

