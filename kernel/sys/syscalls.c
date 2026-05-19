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
#include <kernel/vfs.h>
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
 * every page in the range is backed by a VMA.  A full check would walk
 * proc->vma_list (available via process_ensure_page) but is not yet done.
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

    if (!count) return 0;
    if (!access_ok(buf, count))
        return (uint64_t)-(int64_t)EFAULT;

    size_t  written = 0;
    errno_t e = vfs_write(current_proc(), fd, buf, count, &written);
    if (e < 0) return (uint64_t)(int64_t)e;
    return (uint64_t)written;
}

/* ------------------------------------------------------------------ */
/* sys_open / sys_close (nr = 2 / 3)                                   */
/* ------------------------------------------------------------------ */

static uint64_t sys_open(syscall_frame_t *f) {
    const char *path  = (const char *)(uintptr_t)f->rdi;
    int         flags = (int)(int32_t)f->rsi;
    uint32_t    mode  = (uint32_t)f->rdx;

    if (!path || !access_ok(path, 1))
        return (uint64_t)-(int64_t)EFAULT;

    int fd = -1;
    errno_t e = vfs_open(current_proc(), path, flags, mode, &fd);
    if (e < 0) return (uint64_t)(int64_t)e;
    return (uint64_t)(uint32_t)fd;
}

static uint64_t sys_close(syscall_frame_t *f) {
    int fd = (int)(int32_t)f->rdi;
    errno_t e = vfs_close(current_proc(), fd);
    if (e < 0) return (uint64_t)(int64_t)e;
    return 0;
}

/* ------------------------------------------------------------------ */
/* sys_munmap (nr = 11)                                                 */
/* ------------------------------------------------------------------ */

static uint64_t sys_munmap(syscall_frame_t *f) {
    uintptr_t addr   = (uintptr_t)f->rdi;
    size_t    length = (size_t)f->rsi;

    if (addr & (PAGE_SIZE - 1)) return (uint64_t)-(int64_t)EINVAL;
    if (!length)                return (uint64_t)-(int64_t)EINVAL;

    process_t *proc = current_proc();
    if (!proc) return (uint64_t)-(int64_t)ENOMEM;

    process_munmap(proc, addr, length);
    return 0;
}

/* ------------------------------------------------------------------ */
/* sys_mprotect (nr = 10)                                               */
/* ------------------------------------------------------------------ */

static uint64_t sys_mprotect(syscall_frame_t *f) {
    uintptr_t addr   = (uintptr_t)f->rdi;
    size_t    length = (size_t)f->rsi;
    uint32_t  prot   = (uint32_t)f->rdx;

    if (!length) return 0;

    process_t *proc = current_proc();
    if (!proc) return (uint64_t)-(int64_t)ENOMEM;

    errno_t err = process_mprotect(proc, addr, length, prot);
    if (err < 0) return (uint64_t)(int64_t)err;
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
        uintptr_t page_new = PAGE_ALIGN_UP(new_brk);
        uintptr_t page_old = PAGE_ALIGN_UP(proc->brk);
        if (page_old > page_new)
            process_munmap(proc, page_new, page_old - page_new);
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

static uint64_t sys_ioctl(syscall_frame_t *f) {
    int           fd  = (int)(int32_t)f->rdi;
    unsigned long req = (unsigned long)f->rsi;
    uintptr_t     arg = (uintptr_t)f->rdx;

    errno_t e = vfs_ioctl(current_proc(), fd, req, arg);
    if (e < 0) return (uint64_t)(int64_t)e;
    return 0;
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
/* sys_read (nr = 0)                                                    */
/* ------------------------------------------------------------------ */

static uint64_t sys_read(syscall_frame_t *f) {
    int    fd    = (int)(int32_t)f->rdi;
    char  *buf   = (char *)(uintptr_t)f->rsi;
    size_t count = (size_t)f->rdx;

    if (!count) return 0;
    if (!access_ok(buf, count))
        return (uint64_t)-(int64_t)EFAULT;

    size_t  n = 0;
    errno_t e = vfs_read(current_proc(), fd, buf, count, &n);
    if (e < 0) return (uint64_t)(int64_t)e;
    return (uint64_t)n;
}

/* ------------------------------------------------------------------ */
/* sys_fstat (nr = 5)                                                   */
/* ------------------------------------------------------------------ */

/*
 * Linux x86-64 struct stat layout, 144 bytes total.
 * Defined here to avoid including a POSIX stat.h in kernel code.
 */
struct kernel_stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t __pad0;
    uint64_t st_rdev;
    int64_t  st_size;
    int64_t  st_blksize;
    int64_t  st_blocks;
    int64_t  st_atim_sec;
    int64_t  st_atim_nsec;
    int64_t  st_mtim_sec;
    int64_t  st_mtim_nsec;
    int64_t  st_ctim_sec;
    int64_t  st_ctim_nsec;
    int64_t  __unused[3];
};

static uint64_t sys_fstat(syscall_frame_t *f) {
    int                 fd = (int)(int32_t)f->rdi;
    struct kernel_stat *st = (struct kernel_stat *)(uintptr_t)f->rsi;

    if (!access_ok(st, sizeof(*st)))
        return (uint64_t)-(int64_t)EFAULT;

    vfs_stat_t vs;
    errno_t e = vfs_fstat(current_proc(), fd, &vs);
    if (e < 0) return (uint64_t)(int64_t)e;

    __builtin_memset(st, 0, sizeof(*st));
    st->st_ino     = vs.ino;
    st->st_mode    = vs.mode;
    st->st_nlink   = vs.nlink;
    st->st_uid     = vs.uid;
    st->st_gid     = vs.gid;
    st->st_rdev    = vs.rdev;
    st->st_size    = (int64_t)vs.size;
    st->st_blksize = vs.blksize ? vs.blksize : PAGE_SIZE;
    st->st_blocks  = vs.blocks;
    return 0;
}

/* ------------------------------------------------------------------ */
/* sys_rt_sigaction / sys_rt_sigprocmask (nr = 13 / 14)               */
/* ------------------------------------------------------------------ */

/*
 * No signal delivery yet.  Return 0 and zero the old-action / old-mask
 * output buffers so callers see a clean slate (SIG_DFL, empty mask).
 */
static uint64_t sys_rt_sigaction(syscall_frame_t *f) {
    void *oldact = (void *)(uintptr_t)f->rdx;
    if (oldact) {
        if (!access_ok(oldact, 32u))
            return (uint64_t)-(int64_t)EFAULT;
        __builtin_memset(oldact, 0, 32u);
    }
    return 0;
}

static uint64_t sys_rt_sigprocmask(syscall_frame_t *f) {
    void  *oldset  = (void *)(uintptr_t)f->rdx;
    size_t setsize = (size_t)f->r10;
    if (oldset) {
        if (!access_ok(oldset, setsize))
            return (uint64_t)-(int64_t)EFAULT;
        __builtin_memset(oldset, 0, setsize);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* sys_writev (nr = 20)                                                 */
/* ------------------------------------------------------------------ */

struct iovec {
    void  *iov_base;
    size_t iov_len;
};

static uint64_t sys_writev(syscall_frame_t *f) {
    int                fd     = (int)(int32_t)f->rdi;
    const struct iovec *iov   = (const struct iovec *)(uintptr_t)f->rsi;
    int                iovcnt = (int)(int32_t)f->rdx;

    if (iovcnt <= 0 || iovcnt > 1024)
        return (uint64_t)-(int64_t)EINVAL;
    if (!access_ok(iov, (size_t)iovcnt * sizeof(struct iovec)))
        return (uint64_t)-(int64_t)EFAULT;

    size_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        const char *base = (const char *)iov[i].iov_base;
        size_t      len  = iov[i].iov_len;
        if (!len) continue;
        if (!access_ok(base, len))
            return (uint64_t)-(int64_t)EFAULT;
        size_t  written = 0;
        errno_t e = vfs_write(current_proc(), fd, base, len, &written);
        if (e < 0) return total ? (uint64_t)total : (uint64_t)(int64_t)e;
        total += written;
    }
    return (uint64_t)total;
}

/* ------------------------------------------------------------------ */
/* sys_getuid / sys_getgid / sys_geteuid / sys_getegid (102/104/107/108) */
/* ------------------------------------------------------------------ */

static uint64_t sys_getuid(syscall_frame_t *f)  { (void)f; return 0; }
static uint64_t sys_getgid(syscall_frame_t *f)  { (void)f; return 0; }
static uint64_t sys_geteuid(syscall_frame_t *f) { (void)f; return 0; }
static uint64_t sys_getegid(syscall_frame_t *f) { (void)f; return 0; }

/* ------------------------------------------------------------------ */
/* sys_futex (nr = 202)                                                 */
/* ------------------------------------------------------------------ */

static uint64_t sys_futex(syscall_frame_t *f) {
    (void)f;
    return (uint64_t)-(int64_t)ENOSYS;
}

/* ------------------------------------------------------------------ */
/* sys_lseek (nr = 8)                                                   */
/* ------------------------------------------------------------------ */

static uint64_t sys_lseek(syscall_frame_t *f) {
    int     fd     = (int)(int32_t)f->rdi;
    int64_t offset = (int64_t)f->rsi;
    int     whence = (int)(int32_t)f->rdx;

    int64_t new_pos = 0;
    errno_t e = vfs_seek(current_proc(), fd, offset, whence, &new_pos);
    if (e < 0) return (uint64_t)(int64_t)e;
    return (uint64_t)new_pos;
}

/* ------------------------------------------------------------------ */
/* sys_stat (nr = 4)                                                    */
/* ------------------------------------------------------------------ */

static uint64_t sys_stat(syscall_frame_t *f) {
    const char         *path = (const char *)(uintptr_t)f->rdi;
    struct kernel_stat *st   = (struct kernel_stat *)(uintptr_t)f->rsi;

    if (!path || !access_ok(path, 1))
        return (uint64_t)-(int64_t)EFAULT;
    if (!access_ok(st, sizeof(*st)))
        return (uint64_t)-(int64_t)EFAULT;

    vfs_stat_t vs;
    errno_t e = vfs_stat(current_proc(), path, &vs);
    if (e < 0) return (uint64_t)(int64_t)e;

    __builtin_memset(st, 0, sizeof(*st));
    st->st_ino     = vs.ino;
    st->st_mode    = vs.mode;
    st->st_nlink   = vs.nlink;
    st->st_uid     = vs.uid;
    st->st_gid     = vs.gid;
    st->st_rdev    = vs.rdev;
    st->st_size    = (int64_t)vs.size;
    st->st_blksize = vs.blksize ? vs.blksize : PAGE_SIZE;
    st->st_blocks  = vs.blocks;
    return 0;
}

/* ------------------------------------------------------------------ */
/* sys_getdents64 (nr = 217)                                            */
/* ------------------------------------------------------------------ */

/*
 * linux_dirent64 layout (x86-64):
 *   uint64_t d_ino      offset  0
 *   int64_t  d_off      offset  8
 *   uint16_t d_reclen   offset 16
 *   uint8_t  d_type     offset 18
 *   char     d_name[]   offset 19  (NUL-terminated; record padded to 8 bytes)
 *
 * d_type is derived from the inode mode: (mode & S_IFMT) >> 12 gives the
 * DT_* value on Linux (DT_DIR=4, DT_REG=8, DT_CHR=2, DT_FIFO=1, etc.).
 *
 * Maximum record length: 19 + 255 (name) + 1 (NUL), padded to 8 = 280 bytes.
 * We check for 280 bytes of headroom before reading each entry so that we
 * never read an entry we cannot fit; avoids losing entries on buffer overflow.
 */
#define DIRENT64_FIXED  19u          /* bytes before d_name              */
#define DIRENT64_MAXREC 280u         /* max record (255-char name + pad) */

static uint64_t sys_getdents64(syscall_frame_t *f) {
    int    fd    = (int)(int32_t)f->rdi;
    void  *buf   = (void *)(uintptr_t)f->rsi;
    size_t count = (size_t)f->rdx;

    if (!buf || !access_ok(buf, count))
        return (uint64_t)-(int64_t)EFAULT;
    if (count < DIRENT64_MAXREC)
        return (uint64_t)-(int64_t)EINVAL;

    char     name[256];
    uint64_t ino;
    uint32_t mode;
    size_t   total = 0;

    while (count - total >= DIRENT64_MAXREC) {
        /* Save position so we can rewind if the entry somehow overflows
         * (shouldn't happen given the headroom check above). */
        int64_t saved_pos = 0;
        vfs_seek(current_proc(), fd, 0, SEEK_CUR, &saved_pos);
        /* SEEK_CUR with offset=0 on a directory returns current index. */

        errno_t e = vfs_readdir(current_proc(), fd, name, sizeof(name),
                                &ino, &mode);
        if (e == -ENOENT) break;   /* end of directory */
        if (e < 0) return total ? (uint64_t)total : (uint64_t)(int64_t)e;

        extern size_t strlen(const char *);
        size_t namelen = strlen(name);
        size_t reclen  = (DIRENT64_FIXED + namelen + 1u + 7u) & ~(size_t)7u;

        if (total + reclen > count) {
            /* Rewind to the entry we just consumed so the next call sees it. */
            vfs_seek(current_proc(), fd, saved_pos, SEEK_SET, &saved_pos);
            break;
        }

        uint8_t *p = (uint8_t *)buf + total;
        __builtin_memset(p, 0, reclen);                   /* zero padding  */
        *(uint64_t *)(p +  0) = ino;
        *(int64_t  *)(p +  8) = (int64_t)(total + reclen);
        *(uint16_t *)(p + 16) = (uint16_t)reclen;
        *(uint8_t  *)(p + 18) = (uint8_t)((mode & S_IFMT) >> 12);
        __builtin_memcpy(p + 19, name, namelen + 1);

        total += reclen;
    }

    return (uint64_t)total;
}

/* ------------------------------------------------------------------ */
/* sys_dup / sys_dup2 (nr = 32 / 33)                                   */
/* ------------------------------------------------------------------ */

static uint64_t sys_dup(syscall_frame_t *f) {
    int     oldfd  = (int)(int32_t)f->rdi;
    int     newfd  = -1;
    errno_t e      = vfs_dup(current_proc(), oldfd, &newfd);
    if (e < 0) return (uint64_t)(int64_t)e;
    return (uint64_t)(uint32_t)newfd;
}

static uint64_t sys_dup2(syscall_frame_t *f) {
    int     oldfd = (int)(int32_t)f->rdi;
    int     newfd = (int)(int32_t)f->rsi;
    errno_t e     = vfs_dup2(current_proc(), oldfd, newfd);
    if (e < 0) return (uint64_t)(int64_t)e;
    return (uint64_t)(uint32_t)newfd;
}

/* ------------------------------------------------------------------ */
/* sys_pipe (nr = 22)                                                   */
/* ------------------------------------------------------------------ */

static uint64_t sys_pipe(syscall_frame_t *f) {
    int *pipefd = (int *)(uintptr_t)f->rdi;

    if (!access_ok(pipefd, 2 * sizeof(int)))
        return (uint64_t)-(int64_t)EFAULT;

    int     fds[2] = { -1, -1 };
    errno_t e      = vfs_pipe(current_proc(), fds);
    if (e < 0) return (uint64_t)(int64_t)e;

    pipefd[0] = fds[0];
    pipefd[1] = fds[1];
    return 0;
}

/* ------------------------------------------------------------------ */
/* sys_prlimit64 (nr = 302)                                             */
/* ------------------------------------------------------------------ */

#define RLIM_INFINITY  0xFFFFFFFFFFFFFFFFULL
#define RLIMIT_STACK   3
#define RLIMIT_NOFILE  7

static uint64_t sys_prlimit64(syscall_frame_t *f) {
    int       pid      = (int)(int32_t)f->rdi;
    int       resource = (int)(int32_t)f->rsi;
    uint64_t *old_lim  = (uint64_t *)(uintptr_t)f->r10;

    if (pid != 0)
        return (uint64_t)-(int64_t)EPERM;

    if (old_lim) {
        if (!access_ok(old_lim, 16u))
            return (uint64_t)-(int64_t)EFAULT;
        uint64_t cur = RLIM_INFINITY, max = RLIM_INFINITY;
        if (resource == RLIMIT_STACK)  cur = 8u * 1024u * 1024u;
        if (resource == RLIMIT_NOFILE) { cur = 1024; max = 4096; }
        old_lim[0] = cur;
        old_lim[1] = max;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* sys_readv (nr = 19)                                                  */
/* ------------------------------------------------------------------ */

static uint64_t sys_readv(syscall_frame_t *f) {
    int          fd     = (int)(int32_t)f->rdi;
    struct iovec *iov   = (struct iovec *)(uintptr_t)f->rsi;
    int          iovcnt = (int)(int32_t)f->rdx;

    if (iovcnt <= 0 || iovcnt > 1024)
        return (uint64_t)-(int64_t)EINVAL;
    if (!access_ok(iov, (size_t)iovcnt * sizeof(struct iovec)))
        return (uint64_t)-(int64_t)EFAULT;

    size_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        char  *base = (char *)iov[i].iov_base;
        size_t len  = iov[i].iov_len;
        if (!len) continue;
        if (!access_ok(base, len))
            return (uint64_t)-(int64_t)EFAULT;
        size_t  n = 0;
        errno_t e = vfs_read(current_proc(), fd, base, len, &n);
        if (e < 0) return total ? (uint64_t)total : (uint64_t)(int64_t)e;
        total += n;
        if (n < len) break;   /* short read / EOF */
    }
    return (uint64_t)total;
}

/* ------------------------------------------------------------------ */
/* sys_pread64 / sys_pwrite64 (nr = 17 / 18)                          */
/* ------------------------------------------------------------------ */

static uint64_t sys_pread64(syscall_frame_t *f) {
    int     fd     = (int)(int32_t)f->rdi;
    char   *buf    = (char *)(uintptr_t)f->rsi;
    size_t  count  = (size_t)f->rdx;
    int64_t offset = (int64_t)f->r10;

    if (!count) return 0;
    if (!access_ok(buf, count)) return (uint64_t)-(int64_t)EFAULT;
    if (offset < 0) return (uint64_t)-(int64_t)EINVAL;

    process_t *proc = current_proc();
    int64_t saved = 0, dummy = 0;
    vfs_seek(proc, fd, 0, SEEK_CUR, &saved);
    errno_t e = vfs_seek(proc, fd, offset, SEEK_SET, &dummy);
    if (e < 0) return (uint64_t)(int64_t)e;
    size_t n = 0;
    e = vfs_read(proc, fd, buf, count, &n);
    vfs_seek(proc, fd, saved, SEEK_SET, &dummy);
    if (e < 0) return (uint64_t)(int64_t)e;
    return (uint64_t)n;
}

static uint64_t sys_pwrite64(syscall_frame_t *f) {
    int          fd     = (int)(int32_t)f->rdi;
    const char  *buf    = (const char *)(uintptr_t)f->rsi;
    size_t       count  = (size_t)f->rdx;
    int64_t      offset = (int64_t)f->r10;

    if (!count) return 0;
    if (!access_ok(buf, count)) return (uint64_t)-(int64_t)EFAULT;
    if (offset < 0) return (uint64_t)-(int64_t)EINVAL;

    process_t *proc = current_proc();
    int64_t saved = 0, dummy = 0;
    vfs_seek(proc, fd, 0, SEEK_CUR, &saved);
    errno_t e = vfs_seek(proc, fd, offset, SEEK_SET, &dummy);
    if (e < 0) return (uint64_t)(int64_t)e;
    size_t n = 0;
    e = vfs_write(proc, fd, buf, count, &n);
    vfs_seek(proc, fd, saved, SEEK_SET, &dummy);
    if (e < 0) return (uint64_t)(int64_t)e;
    return (uint64_t)n;
}

/* ------------------------------------------------------------------ */
/* sys_access (nr = 21)                                                 */
/* ------------------------------------------------------------------ */

static uint64_t sys_access(syscall_frame_t *f) {
    const char *path  = (const char *)(uintptr_t)f->rdi;
    int         amode = (int)(int32_t)f->rsi;

    if (!path || !access_ok(path, 1))
        return (uint64_t)-(int64_t)EFAULT;

    errno_t e = vfs_access(current_proc(), path, amode);
    if (e < 0) return (uint64_t)(int64_t)e;
    return 0;
}

/* ------------------------------------------------------------------ */
/* sys_fcntl (nr = 72)                                                  */
/* ------------------------------------------------------------------ */

static uint64_t sys_fcntl(syscall_frame_t *f) {
    int       fd  = (int)(int32_t)f->rdi;
    int       cmd = (int)(int32_t)f->rsi;
    uintptr_t arg = (uintptr_t)f->rdx;

    int     result = 0;
    errno_t e      = vfs_fcntl(current_proc(), fd, cmd, arg, &result);
    if (e < 0) return (uint64_t)(int64_t)e;
    return (uint64_t)(int64_t)result;
}

/* ------------------------------------------------------------------ */
/* sys_truncate / sys_ftruncate (nr = 76 / 77)                         */
/* ------------------------------------------------------------------ */

static uint64_t sys_truncate(syscall_frame_t *f) {
    const char *path   = (const char *)(uintptr_t)f->rdi;
    int64_t     length = (int64_t)f->rsi;

    if (!path || !access_ok(path, 1))
        return (uint64_t)-(int64_t)EFAULT;
    if (length < 0) return (uint64_t)-(int64_t)EINVAL;

    errno_t e = vfs_truncate(current_proc(), path, (uint64_t)length);
    if (e < 0) return (uint64_t)(int64_t)e;
    return 0;
}

static uint64_t sys_ftruncate(syscall_frame_t *f) {
    int     fd     = (int)(int32_t)f->rdi;
    int64_t length = (int64_t)f->rsi;

    if (length < 0) return (uint64_t)-(int64_t)EINVAL;

    errno_t e = vfs_ftruncate(current_proc(), fd, (uint64_t)length);
    if (e < 0) return (uint64_t)(int64_t)e;
    return 0;
}

/* ------------------------------------------------------------------ */
/* sys_rename (nr = 82)                                                 */
/* ------------------------------------------------------------------ */

static uint64_t sys_rename(syscall_frame_t *f) {
    const char *oldpath = (const char *)(uintptr_t)f->rdi;
    const char *newpath = (const char *)(uintptr_t)f->rsi;

    if (!oldpath || !access_ok(oldpath, 1)) return (uint64_t)-(int64_t)EFAULT;
    if (!newpath || !access_ok(newpath, 1)) return (uint64_t)-(int64_t)EFAULT;

    errno_t e = vfs_rename(current_proc(), oldpath, newpath);
    if (e < 0) return (uint64_t)(int64_t)e;
    return 0;
}

/* ------------------------------------------------------------------ */
/* sys_mkdir (nr = 83)                                                  */
/* ------------------------------------------------------------------ */

static uint64_t sys_mkdir(syscall_frame_t *f) {
    const char *path = (const char *)(uintptr_t)f->rdi;
    uint32_t    mode = (uint32_t)f->rsi;

    if (!path || !access_ok(path, 1))
        return (uint64_t)-(int64_t)EFAULT;

    errno_t e = vfs_mkdir(current_proc(), path, mode);
    if (e < 0) return (uint64_t)(int64_t)e;
    return 0;
}

/* ------------------------------------------------------------------ */
/* sys_rmdir (nr = 84)                                                  */
/* ------------------------------------------------------------------ */

static uint64_t sys_rmdir(syscall_frame_t *f) {
    const char *path = (const char *)(uintptr_t)f->rdi;

    if (!path || !access_ok(path, 1))
        return (uint64_t)-(int64_t)EFAULT;

    errno_t e = vfs_rmdir(current_proc(), path);
    if (e < 0) return (uint64_t)(int64_t)e;
    return 0;
}

/* ------------------------------------------------------------------ */
/* sys_unlink (nr = 87)                                                 */
/* ------------------------------------------------------------------ */

static uint64_t sys_unlink(syscall_frame_t *f) {
    const char *path = (const char *)(uintptr_t)f->rdi;

    if (!path || !access_ok(path, 1))
        return (uint64_t)-(int64_t)EFAULT;

    errno_t e = vfs_unlink(current_proc(), path);
    if (e < 0) return (uint64_t)(int64_t)e;
    return 0;
}

/* ------------------------------------------------------------------ */
/* sys_umask (nr = 95)                                                  */
/* ------------------------------------------------------------------ */

static uint64_t sys_umask(syscall_frame_t *f) {
    uint32_t   mask = (uint32_t)f->rdi & 0777u;
    process_t *proc = current_proc();

    bool irq = spinlock_acquire(&proc->lock);
    uint32_t old = proc->umask;
    proc->umask  = mask;
    spinlock_release(&proc->lock, irq);

    return (uint64_t)old;
}

/* ------------------------------------------------------------------ */
/* sys_getcwd (nr = 79)                                                 */
/* ------------------------------------------------------------------ */

static uint64_t sys_getcwd(syscall_frame_t *f) {
    char  *buf  = (char *)(uintptr_t)f->rdi;
    size_t size = (size_t)f->rsi;

    if (!buf || !size) return (uint64_t)-(int64_t)EINVAL;
    if (!access_ok(buf, size)) return (uint64_t)-(int64_t)EFAULT;

    errno_t e = vfs_getcwd(current_proc(), buf, size);
    if (e < 0) return (uint64_t)(int64_t)e;
    return (uint64_t)(uintptr_t)buf;   /* Linux returns buf pointer on success */
}

/* ------------------------------------------------------------------ */
/* sys_chdir (nr = 80)                                                  */
/* ------------------------------------------------------------------ */

static uint64_t sys_chdir(syscall_frame_t *f) {
    const char *path = (const char *)(uintptr_t)f->rdi;

    if (!path || !access_ok(path, 1))
        return (uint64_t)-(int64_t)EFAULT;

    errno_t e = vfs_chdir(current_proc(), path);
    if (e < 0) return (uint64_t)(int64_t)e;
    return 0;
}

/* ------------------------------------------------------------------ */
/* sys_fsync / sys_fdatasync / sys_sync (nr = 74 / 75 / 162)          */
/* ------------------------------------------------------------------ */

static uint64_t sys_fsync(syscall_frame_t *f) {
    (void)f;
    return 0;   /* tmpfs is in-memory: always "synced" */
}

static uint64_t sys_sync(syscall_frame_t *f) {
    (void)f;
    return 0;
}

/* ------------------------------------------------------------------ */
/* sys_pipe2 (nr = 293)                                                 */
/* ------------------------------------------------------------------ */

static uint64_t sys_pipe2(syscall_frame_t *f) {
    int *pipefd = (int *)(uintptr_t)f->rdi;
    int  flags  = (int)(int32_t)f->rsi;

    if (!access_ok(pipefd, 2 * sizeof(int)))
        return (uint64_t)-(int64_t)EFAULT;

    int     fds[2] = { -1, -1 };
    errno_t e      = vfs_pipe2(current_proc(), fds, flags);
    if (e < 0) return (uint64_t)(int64_t)e;

    pipefd[0] = fds[0];
    pipefd[1] = fds[1];
    return 0;
}

/* ------------------------------------------------------------------ */
/* sys_dup3 (nr = 292)                                                  */
/* ------------------------------------------------------------------ */

static uint64_t sys_dup3(syscall_frame_t *f) {
    int     oldfd = (int)(int32_t)f->rdi;
    int     newfd = (int)(int32_t)f->rsi;
    int     flags = (int)(int32_t)f->rdx;
    errno_t e     = vfs_dup3(current_proc(), oldfd, newfd, flags);
    if (e < 0) return (uint64_t)(int64_t)e;
    return (uint64_t)(uint32_t)newfd;
}

/* ------------------------------------------------------------------ */
/* sys_openat (nr = 257)                                                */
/* ------------------------------------------------------------------ */

static uint64_t sys_openat(syscall_frame_t *f) {
    int         dirfd = (int)(int32_t)f->rdi;
    const char *path  = (const char *)(uintptr_t)f->rsi;
    int         flags = (int)(int32_t)f->rdx;
    uint32_t    mode  = (uint32_t)f->r10;

    if (!path || !access_ok(path, 1))
        return (uint64_t)-(int64_t)EFAULT;

    int fd = -1;
    errno_t e = vfs_openat(current_proc(), dirfd, path, flags, mode, &fd);
    if (e < 0) return (uint64_t)(int64_t)e;
    return (uint64_t)(uint32_t)fd;
}

/* ------------------------------------------------------------------ */
/* sys_newfstatat (nr = 262)                                            */
/* ------------------------------------------------------------------ */

static uint64_t sys_newfstatat(syscall_frame_t *f) {
    int                 dirfd   = (int)(int32_t)f->rdi;
    const char         *path    = (const char *)(uintptr_t)f->rsi;
    struct kernel_stat *statbuf = (struct kernel_stat *)(uintptr_t)f->rdx;
    int                 flags   = (int)(int32_t)f->r10;

    if (!access_ok(statbuf, sizeof(*statbuf)))
        return (uint64_t)-(int64_t)EFAULT;

    vfs_stat_t vs;
    errno_t    e;

    if ((flags & AT_EMPTY_PATH) && path && path[0] == '\0') {
        /* fstat the dirfd itself */
        e = vfs_fstat(current_proc(), dirfd, &vs);
    } else {
        if (!path || !access_ok(path, 1))
            return (uint64_t)-(int64_t)EFAULT;
        e = vfs_statat(current_proc(), dirfd, path, flags, &vs);
    }
    if (e < 0) return (uint64_t)(int64_t)e;

    __builtin_memset(statbuf, 0, sizeof(*statbuf));
    statbuf->st_ino     = vs.ino;
    statbuf->st_mode    = vs.mode;
    statbuf->st_nlink   = vs.nlink;
    statbuf->st_uid     = vs.uid;
    statbuf->st_gid     = vs.gid;
    statbuf->st_rdev    = vs.rdev;
    statbuf->st_size    = (int64_t)vs.size;
    statbuf->st_blksize = vs.blksize ? vs.blksize : PAGE_SIZE;
    statbuf->st_blocks  = vs.blocks;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Registration                                                         */
/* ------------------------------------------------------------------ */

void syscalls_init(void) {
    syscall_register(0,   sys_read);
    syscall_register(1,   sys_write);
    syscall_register(2,   sys_open);
    syscall_register(3,   sys_close);
    syscall_register(4,   sys_stat);
    syscall_register(5,   sys_fstat);
    syscall_register(8,   sys_lseek);
    syscall_register(9,   sys_mmap);
    syscall_register(10,  sys_mprotect);
    syscall_register(11,  sys_munmap);
    syscall_register(12,  sys_brk);
    syscall_register(13,  sys_rt_sigaction);
    syscall_register(14,  sys_rt_sigprocmask);
    syscall_register(16,  sys_ioctl);
    syscall_register(17,  sys_pread64);
    syscall_register(18,  sys_pwrite64);
    syscall_register(19,  sys_readv);
    syscall_register(20,  sys_writev);
    syscall_register(21,  sys_access);
    syscall_register(22,  sys_pipe);
    syscall_register(32,  sys_dup);
    syscall_register(33,  sys_dup2);
    syscall_register(39,  sys_getpid);
    syscall_register(60,  sys_exit);
    syscall_register(63,  sys_uname);
    syscall_register(72,  sys_fcntl);
    syscall_register(74,  sys_fsync);
    syscall_register(75,  sys_fsync);    /* fdatasync: same no-op */
    syscall_register(76,  sys_truncate);
    syscall_register(77,  sys_ftruncate);
    syscall_register(79,  sys_getcwd);
    syscall_register(80,  sys_chdir);
    syscall_register(82,  sys_rename);
    syscall_register(83,  sys_mkdir);
    syscall_register(84,  sys_rmdir);
    syscall_register(87,  sys_unlink);
    syscall_register(95,  sys_umask);
    syscall_register(102, sys_getuid);
    syscall_register(104, sys_getgid);
    syscall_register(107, sys_geteuid);
    syscall_register(108, sys_getegid);
    syscall_register(110, sys_getppid);
    syscall_register(158, sys_arch_prctl);
    syscall_register(162, sys_sync);
    syscall_register(186, sys_gettid);
    syscall_register(202, sys_futex);
    syscall_register(217, sys_getdents64);
    syscall_register(218, sys_set_tid_address);
    syscall_register(228, sys_clock_gettime);
    syscall_register(231, sys_exit);
    syscall_register(257, sys_openat);
    syscall_register(262, sys_newfstatat);
    syscall_register(292, sys_dup3);
    syscall_register(293, sys_pipe2);
    syscall_register(302, sys_prlimit64);
}
