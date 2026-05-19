/*
 * syscalls.c - built-in Linux-ABI syscall implementations.
 *
 * Linux syscall numbers are used throughout so that unmodified static
 * binaries linked against musl will work without a shim layer.
 *
 * Argument convention (x86-64 Linux ABI):
 *   rdi, rsi, rdx, r10, r8, r9  -- args 1-6
 *   Note: r10 carries arg 4 (NOT rcx, which SYSCALL clobbers with the RIP).
 */

#include <kernel/syscall.h>
#include <kernel/scheduler.h>
#include <kernel/cpu.h>
#include <kernel/vmm.h>
#include <kernel/kprintf.h>
#include <kernel/types.h>
#include <kernel/msr.h>
#include <kernel/hpet.h>
#include <kernel/version.h>
#include <deps/printf.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define PAGE_ALIGN_UP(x)  (((x) + PAGE_SIZE - 1) & ~(uintptr_t)(PAGE_SIZE - 1))

/* Linux MAP_* constants */
#define MAP_ANONYMOUS  0x20
#define MAP_FAILED     ((uintptr_t)-1)

/* ------------------------------------------------------------------ */
/* access_ok -- user pointer validation                                  */
/* ------------------------------------------------------------------ */

/*
 * Canonical user address space on x86-64 runs from 0 to just below
 * 0x0000800000000000.  Anything at or above that is kernel space.
 *
 * Checks:
 *   1. addr is not NULL.
 *   2. [addr, addr+len) does not wrap around 64-bit space.
 *   3. The entire range is below the user/kernel boundary.
 *
 * This is a necessary but not sufficient check -- it does not verify that
 * every page in the range is actually mapped.  That will be added once we
 * have a reliable "walk the VMA list" helper.
 */
#define USER_ADDR_MAX  ((uintptr_t)0x0000800000000000ULL)

static bool access_ok(const void *addr, size_t len) {
    uintptr_t start = (uintptr_t)addr;
    if (!start) return false;
    if (len == 0) return true;
    uintptr_t end = start + len;
    if (end < start) return false;   /* wrap */
    return end <= USER_ADDR_MAX;
}

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

static inline process_t *current_proc(void) {
    return this_cpu()->current_thread->parent;
}

static inline thread_t *current_thread(void) {
    return this_cpu()->current_thread;
}

/* ------------------------------------------------------------------ */
/* sys_write (nr = 1)                                                   */
/* ------------------------------------------------------------------ */

static uint64_t sys_write(syscall_frame_t *f) {
    int         fd    = (int)(int32_t)f->rdi;
    const char *buf   = (const char *)(uintptr_t)f->rsi;
    size_t      count = (size_t)f->rdx;

    if (fd != 1 && fd != 2)
        return (uint64_t)-(int64_t)EBADF;

    if (!count) return 0;
    if (!access_ok(buf, count))
        return (uint64_t)-(int64_t)EFAULT;

    kwrite(buf, count);
    return (uint64_t)count;
}

/* ------------------------------------------------------------------ */
/* sys_open / sys_close (nr = 2 / 3)                                   */
/* ------------------------------------------------------------------ */

/*
 * open(path, flags, mode) -- no VFS yet; every path returns -ENOENT.
 * close(fd) -- fd 0/1/2 are always valid sinks; others don't exist.
 */
static uint64_t sys_open(syscall_frame_t *f) {
    (void)f;
    return (uint64_t)-(int64_t)ENOENT;
}

static uint64_t sys_close(syscall_frame_t *f) {
    int fd = (int)(int32_t)f->rdi;
    if (fd >= 0 && fd <= 2) return 0;
    return (uint64_t)-(int64_t)EBADF;
}

/* ------------------------------------------------------------------ */
/* sys_mprotect (nr = 10)                                               */
/* ------------------------------------------------------------------ */

/*
 * mprotect(addr, len, prot)
 *
 * Full remapping requires walking each VMA and calling mmu_map_page with
 * new flags -- deferred until the VMM gets a vma_protect helper.  For now
 * we return success so musl can mark its stack-guard page PROT_NONE
 * without faulting; the guard simply won't enforce.
 */
static uint64_t sys_mprotect(syscall_frame_t *f) {
    (void)f;
    return 0;
}

/* ------------------------------------------------------------------ */
/* sys_mmap (nr = 9)                                                    */
/* ------------------------------------------------------------------ */

static uint64_t sys_mmap(syscall_frame_t *f) {
    uintptr_t hint   = (uintptr_t)f->rdi;
    size_t    length = (size_t)f->rsi;
    uint32_t  prot   = (uint32_t)f->rdx;
    uint32_t  flags  = (uint32_t)f->r10;
    int       fd     = (int)(int32_t)f->r8;

    if (!(flags & MAP_ANONYMOUS) || fd != -1)
        return (uint64_t)-(int64_t)ENOSYS;

    if (!length)
        return (uint64_t)-(int64_t)EINVAL;

    process_t *proc = current_proc();
    if (!proc)
        return (uint64_t)-(int64_t)ENOMEM;

    uintptr_t result = process_mmap(proc, hint, length, prot);
    if (!result)
        return (uint64_t)-(int64_t)ENOMEM;

    return result;
}

/* ------------------------------------------------------------------ */
/* sys_brk (nr = 12)                                                    */
/* ------------------------------------------------------------------ */

static uint64_t sys_brk(syscall_frame_t *f) {
    process_t *proc    = current_proc();
    uintptr_t  new_brk = (uintptr_t)f->rdi;

    if (!new_brk || !proc->brk)
        return proc->brk;

    if (new_brk <= proc->brk) {
        proc->brk = new_brk;
        return proc->brk;
    }

    uintptr_t page_start = PAGE_ALIGN_UP(proc->brk);
    uintptr_t page_end   = PAGE_ALIGN_UP(new_brk);

    if (page_end > page_start) {
        uintptr_t mapped = process_mmap(proc, page_start,
                                        page_end - page_start,
                                        PROT_READ | PROT_WRITE);
        if (!mapped)
            return proc->brk;
    }

    proc->brk = new_brk;
    return proc->brk;
}

/* ------------------------------------------------------------------ */
/* sys_ioctl (nr = 16)                                                  */
/* ------------------------------------------------------------------ */

/*
 * ioctl(fd, request, ...) -- no tty support yet.
 * -ENOTTY tells musl that fd is not a terminal; it will fall back to
 * unbuffered I/O rather than line-buffering, which is the right behavior.
 */
static uint64_t sys_ioctl(syscall_frame_t *f) {
    (void)f;
    return (uint64_t)-(int64_t)ENOTTY;
}

/* ------------------------------------------------------------------ */
/* sys_getpid / sys_getppid / sys_gettid (nr = 39 / 110 / 186)        */
/* ------------------------------------------------------------------ */

static uint64_t sys_getpid(syscall_frame_t *f) {
    (void)f;
    return (uint64_t)current_proc()->pid;
}

static uint64_t sys_getppid(syscall_frame_t *f) {
    (void)f;
    return 1;   /* kernel is parent until we track parent PIDs */
}

static uint64_t sys_gettid(syscall_frame_t *f) {
    (void)f;
    return (uint64_t)current_thread()->tid;
}

/* ------------------------------------------------------------------ */
/* sys_uname (nr = 63)                                                  */
/* ------------------------------------------------------------------ */

struct utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

static void uname_copy(char *dst, const char *src, size_t max) {
    size_t i;
    for (i = 0; i + 1 < max && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

static uint64_t sys_uname(syscall_frame_t *f) {
    struct utsname *buf = (struct utsname *)(uintptr_t)f->rdi;

    if (!access_ok(buf, sizeof(*buf)))
        return (uint64_t)-(int64_t)EFAULT;

    __builtin_memset(buf, 0, sizeof(*buf));

    uname_copy(buf->sysname,    __kernel_name, 65);
    uname_copy(buf->nodename,   "aether",       65);
    snprintf(buf->release, 65, "%d.%d.%d-%s",
             __kernel_version_major, __kernel_version_minor,
             __kernel_version_lower, __kernel_version_suffix);
    snprintf(buf->version, 65, "%s %s", __kernel_build_date, __kernel_build_time);
    uname_copy(buf->machine,    __kernel_arch,  65);
    uname_copy(buf->domainname, "(none)",        65);

    return 0;
}

/* ------------------------------------------------------------------ */
/* sys_clock_gettime (nr = 228)                                         */
/* ------------------------------------------------------------------ */

struct timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

#define CLOCK_REALTIME   0
#define CLOCK_MONOTONIC  1

/*
 * clock_gettime(clockid, tp)
 *
 * Both CLOCK_REALTIME and CLOCK_MONOTONIC are served from the HPET counter,
 * which gives monotonic time since hpet_init().  Absolute wall time is not
 * available until we read the RTC -- callers that need wall time get a
 * monotonic value, which is correct for relative measurements.
 *
 * hpetTickPeriod is in femtoseconds per tick (10^-15 s).
 * nanoseconds = count * period / 1_000_000
 */
static uint64_t sys_clock_gettime(syscall_frame_t *f) {
    int              clockid = (int)(int32_t)f->rdi;
    struct timespec *tp      = (struct timespec *)(uintptr_t)f->rsi;

    if (clockid != CLOCK_REALTIME && clockid != CLOCK_MONOTONIC)
        return (uint64_t)-(int64_t)EINVAL;

    if (!access_ok(tp, sizeof(*tp)))
        return (uint64_t)-(int64_t)EFAULT;

    uint64_t count   = hpet_get_count();
    /* hpetTickPeriod is in femtoseconds/tick; divide by 1e6 for ns/tick.
     * Integer truncation is acceptable -- max error ~1 ns/tick. */
    uint64_t ns_per_tick = hpetTickPeriod / 1000000ULL;
    uint64_t ns          = count * ns_per_tick;

    tp->tv_sec  = (int64_t)(ns / 1000000000ULL);
    tp->tv_nsec = (int64_t)(ns % 1000000000ULL);
    return 0;
}

/* ------------------------------------------------------------------ */
/* sys_exit / sys_exit_group (nr = 60 / 231)                           */
/* ------------------------------------------------------------------ */

static uint64_t sys_exit(syscall_frame_t *f) {
    (void)f;
    thread_exit();
    __builtin_unreachable();
}

/* ------------------------------------------------------------------ */
/* sys_arch_prctl (nr = 158)                                            */
/* ------------------------------------------------------------------ */

#define ARCH_SET_GS  0x1001
#define ARCH_SET_FS  0x1002
#define ARCH_GET_FS  0x1003
#define ARCH_GET_GS  0x1004

static uint64_t sys_arch_prctl(syscall_frame_t *f) {
    int       code = (int)(int32_t)f->rdi;
    uintptr_t addr = (uintptr_t)f->rsi;

    switch (code) {
    case ARCH_SET_FS:
        wrmsr(MSR_FSBASE, addr);
        return 0;
    case ARCH_GET_FS:
        if (!access_ok((void *)addr, sizeof(uintptr_t)))
            return (uint64_t)-(int64_t)EFAULT;
        *(uintptr_t *)addr = rdmsr(MSR_FSBASE);
        return 0;
    default:
        return (uint64_t)-(int64_t)EINVAL;
    }
}

/* ------------------------------------------------------------------ */
/* sys_set_tid_address (nr = 218)                                       */
/* ------------------------------------------------------------------ */

static uint64_t sys_set_tid_address(syscall_frame_t *f) {
    (void)f;
    return (uint64_t)current_thread()->tid;
}

/* ------------------------------------------------------------------ */
/* Registration                                                         */
/* ------------------------------------------------------------------ */

void syscalls_init(void) {
    syscall_register(1,   sys_write);
    syscall_register(2,   sys_open);
    syscall_register(3,   sys_close);
    syscall_register(9,   sys_mmap);
    syscall_register(10,  sys_mprotect);
    syscall_register(12,  sys_brk);
    syscall_register(16,  sys_ioctl);
    syscall_register(39,  sys_getpid);
    syscall_register(60,  sys_exit);
    syscall_register(63,  sys_uname);
    syscall_register(110, sys_getppid);
    syscall_register(158, sys_arch_prctl);
    syscall_register(186, sys_gettid);
    syscall_register(218, sys_set_tid_address);
    syscall_register(228, sys_clock_gettime);
    syscall_register(231, sys_exit);
}
