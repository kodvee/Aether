/*
 * devfs.c - device filesystem.
 *
 * Provides a minimal /dev directory with three character devices:
 *
 *   /dev/null  (major=1, minor=3) - reads return 0 bytes (EOF); writes discard.
 *   /dev/zero  (major=1, minor=5) - reads return zeroed bytes; writes discard.
 *   /dev/tty   (major=5, minor=0) - writes go to the kernel terminal (kwrite);
 *                                   reads return 0 (EOF) until stdin is wired;
 *                                   ioctl handles TCGETS and TIOCGWINSZ.
 *
 * The devfs root directory is a synthetic read-only directory backed by a
 * static table of (name, inode) pairs.  No files can be created or deleted
 * in the root.  Subdirectories are not supported in Stage 2.
 *
 * Device inodes are heap-allocated at devfs_init() and held alive by a
 * reference from the devfs directory table.  They are never freed in normal
 * operation (no umount support).
 */

#include <kernel/vfs.h>
#include <kernel/mmu.h>
#include <kernel/panic.h>
#include <kernel/kprintf.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

extern size_t strlen(const char *s);
extern int    strcmp(const char *s1, const char *s2);

/* ------------------------------------------------------------------ */
/* ioctl request codes (Linux x86-64 values)                           */
/* ------------------------------------------------------------------ */

#define TCGETS      0x5401UL
#define TIOCGWINSZ  0x5413UL
#define TCSETSW     0x5403UL
#define TCSETSF     0x5404UL

/* ------------------------------------------------------------------ */
/* Device number encoding (Linux: (major<<8)|minor for small numbers)  */
/* ------------------------------------------------------------------ */

static inline uint64_t makedev(uint32_t major, uint32_t minor) {
    return ((uint64_t)(major & 0xfff) << 8) | (minor & 0xff);
}

/* ------------------------------------------------------------------ */
/* Devfs directory table                                               */
/* ------------------------------------------------------------------ */

#define DEVFS_MAX_ENTRIES 16

typedef struct {
    const char *name;
    inode_t    *inode;
} devfs_entry_t;

static devfs_entry_t devfs_dir_entries[DEVFS_MAX_ENTRIES];
static int           devfs_dir_count = 0;

/* ------------------------------------------------------------------ */
/* Forward declarations                                                */
/* ------------------------------------------------------------------ */

static const inode_ops_t devfs_dir_inode_ops;
static const inode_ops_t devfs_dev_inode_ops;

static const file_ops_t devfs_null_file_ops;
static const file_ops_t devfs_zero_file_ops;
static const file_ops_t devfs_tty_file_ops;

/* ------------------------------------------------------------------ */
/* Devfs root directory inode operations                               */
/* ------------------------------------------------------------------ */

static errno_t devfs_dir_lookup(inode_t *dir, const char *name,
                                 inode_t **out) {
    (void)dir;
    for (int i = 0; i < devfs_dir_count; i++) {
        if (strcmp(devfs_dir_entries[i].name, name) == 0) {
            inode_ref(devfs_dir_entries[i].inode);
            *out = devfs_dir_entries[i].inode;
            return EOK;
        }
    }
    return -ENOENT;
}

static errno_t devfs_dir_readdir(inode_t *dir, uint64_t idx,
                                  char *name_out, size_t name_max,
                                  inode_t **inode_out) {
    /* idx 0 is always "." (self) */
    if (idx == 0) {
        if (name_max < 2) return -EINVAL;
        name_out[0] = '.'; name_out[1] = '\0';
        inode_ref(dir);
        *inode_out = dir;
        return EOK;
    }
    idx--;   /* actual entry index */

    if (idx >= (uint64_t)devfs_dir_count) return -ENOENT;
    const char *name = devfs_dir_entries[idx].name;
    size_t      nlen = strlen(name);
    if (nlen >= name_max) nlen = name_max - 1;
    __builtin_memcpy(name_out, name, nlen);
    name_out[nlen] = '\0';
    inode_ref(devfs_dir_entries[idx].inode);
    *inode_out = devfs_dir_entries[idx].inode;
    return EOK;
}

static errno_t devfs_dir_open(inode_t *inode, int flags, file_t **out) {
    (void)flags;
    file_t *f = file_alloc(inode, flags, NULL);
    if (!f) return -ENOSPC;
    *out = f;
    return EOK;
}

static errno_t devfs_dir_stat(inode_t *inode, vfs_stat_t *out) {
    out->ino     = inode->ino;
    out->mode    = inode->mode;
    out->nlink   = 2;
    out->uid     = 0;
    out->gid     = 0;
    out->size    = 0;
    out->blksize = 4096;
    out->blocks  = 0;
    out->rdev    = 0;
    return EOK;
}

static const inode_ops_t devfs_dir_inode_ops = {
    .lookup  = devfs_dir_lookup,
    .create  = NULL,   /* /dev is read-only for directory operations */
    .mkdir   = NULL,
    .unlink  = NULL,
    .readdir = devfs_dir_readdir,
    .open    = devfs_dir_open,
    .stat    = devfs_dir_stat,
    .destroy = NULL,
};

/* ------------------------------------------------------------------ */
/* Device inode operations (shared by all char devices)               */
/* ------------------------------------------------------------------ */

static errno_t devfs_dev_open_null(inode_t *inode, int flags, file_t **out) {
    file_t *f = file_alloc(inode, flags, &devfs_null_file_ops);
    if (!f) return -ENOSPC;
    *out = f;
    return EOK;
}

static errno_t devfs_dev_open_zero(inode_t *inode, int flags, file_t **out) {
    file_t *f = file_alloc(inode, flags, &devfs_zero_file_ops);
    if (!f) return -ENOSPC;
    *out = f;
    return EOK;
}

static errno_t devfs_dev_open_tty(inode_t *inode, int flags, file_t **out) {
    file_t *f = file_alloc(inode, flags, &devfs_tty_file_ops);
    if (!f) return -ENOSPC;
    *out = f;
    return EOK;
}

static errno_t devfs_dev_stat(inode_t *inode, vfs_stat_t *out) {
    out->ino     = inode->ino;
    out->mode    = inode->mode;
    out->nlink   = 1;
    out->uid     = 0;
    out->gid     = 0;
    out->size    = 0;
    out->blksize = 4096;
    out->blocks  = 0;
    out->rdev    = inode->rdev;
    return EOK;
}

/* Each device has its own inode_ops because open differs per device. */

static const inode_ops_t devfs_null_inode_ops = {
    .open    = devfs_dev_open_null,
    .stat    = devfs_dev_stat,
    .destroy = NULL,
};
static const inode_ops_t devfs_zero_inode_ops = {
    .open    = devfs_dev_open_zero,
    .stat    = devfs_dev_stat,
    .destroy = NULL,
};
static const inode_ops_t devfs_tty_inode_ops = {
    .open    = devfs_dev_open_tty,
    .stat    = devfs_dev_stat,
    .destroy = NULL,
};

/* ------------------------------------------------------------------ */
/* /dev/null file operations                                            */
/* ------------------------------------------------------------------ */

static errno_t null_read(file_t *f, void *buf, size_t n, size_t *out) {
    (void)f; (void)buf; (void)n;
    *out = 0;   /* EOF */
    return EOK;
}

static errno_t null_write(file_t *f, const void *buf, size_t n, size_t *out) {
    (void)f; (void)buf;
    *out = n;   /* discard; pretend success */
    return EOK;
}

static const file_ops_t devfs_null_file_ops = {
    .read  = null_read,
    .write = null_write,
    .seek  = NULL,
    .ioctl = NULL,
    .close = NULL,
};

/* ------------------------------------------------------------------ */
/* /dev/zero file operations                                            */
/* ------------------------------------------------------------------ */

static errno_t zero_read(file_t *f, void *buf, size_t n, size_t *out) {
    (void)f;
    __builtin_memset(buf, 0, n);
    *out = n;
    return EOK;
}

static errno_t zero_write(file_t *f, const void *buf, size_t n, size_t *out) {
    (void)f; (void)buf;
    *out = n;
    return EOK;
}

static const file_ops_t devfs_zero_file_ops = {
    .read  = zero_read,
    .write = zero_write,
    .seek  = NULL,
    .ioctl = NULL,
    .close = NULL,
};

/* ------------------------------------------------------------------ */
/* /dev/tty file operations                                             */
/* ------------------------------------------------------------------ */

static errno_t tty_read(file_t *f, void *buf, size_t n, size_t *out) {
    (void)f; (void)buf; (void)n;
    *out = 0;   /* EOF -- no keyboard driver yet */
    return EOK;
}

static errno_t tty_write(file_t *f, const void *buf, size_t n, size_t *out) {
    (void)f;
    kwrite((const char *)buf, n);
    *out = n;
    return EOK;
}

/*
 * tty_ioctl - handle terminal control requests.
 *
 * TCGETS (0x5401): fill in a zero termios struct; return 0 so musl
 *   thinks fd is a terminal and uses line-buffered output for stdout.
 * TCSETSW / TCSETSF (0x5403/0x5404): accept silently (no real tty state).
 * TIOCGWINSZ (0x5413): report an 80x24 terminal window.
 * All others: return -ENOTTY.
 *
 * Linux struct termios (x86-64): 4+4+4+4+1+19+4+4 = 44 bytes (before
 * padding; use 60 to be safe with any ABI variant).
 * Linux struct winsize: uint16_t ws_row, ws_col, ws_xpixel, ws_ypixel = 8 bytes.
 */
static errno_t tty_ioctl(file_t *f, unsigned long req, uintptr_t arg) {
    (void)f;
    switch (req) {
    case TCGETS:
    case TCSETSW:
    case TCSETSF:
        if (arg) __builtin_memset((void *)arg, 0, 60);
        return EOK;

    case TIOCGWINSZ: {
        if (!arg) return -EFAULT;
        uint16_t *ws = (uint16_t *)arg;
        ws[0] = 24;   /* ws_row */
        ws[1] = 80;   /* ws_col */
        ws[2] = 0;    /* ws_xpixel */
        ws[3] = 0;    /* ws_ypixel */
        return EOK;
    }

    default:
        return -ENOTTY;
    }
}

static const file_ops_t devfs_tty_file_ops = {
    .read  = tty_read,
    .write = tty_write,
    .seek  = NULL,
    .ioctl = tty_ioctl,
    .close = NULL,
};

/* ------------------------------------------------------------------ */
/* devfs_init                                                          */
/* ------------------------------------------------------------------ */

static uint64_t devfs_next_ino = 0x10000;   /* separate from tmpfs inos */

static inode_t *devfs_alloc_dev_inode(const inode_ops_t *ops,
                                       uint32_t major, uint32_t minor) {
    inode_t *inode = malloc(sizeof(inode_t));
    if (!inode) return NULL;
    inode->ino      = __atomic_fetch_add(&devfs_next_ino, 1ULL, __ATOMIC_RELAXED);
    inode->mode     = S_IFCHR | 0666u;
    inode->size     = 0;
    inode->uid      = 0;
    inode->gid      = 0;
    inode->rdev     = makedev(major, minor);
    inode->nlink    = 1;
    inode->refcount = 1;
    inode->ops      = ops;
    inode->lock     = (spinlock_t)SPINLOCK_ZERO;
    inode->private  = NULL;
    return inode;
}

static void devfs_register(const char *name, inode_t *inode) {
    KERNEL_ASSERT(devfs_dir_count < DEVFS_MAX_ENTRIES);
    devfs_dir_entries[devfs_dir_count].name  = name;
    devfs_dir_entries[devfs_dir_count].inode = inode;
    inode_ref(inode);   /* directory entry holds one ref */
    devfs_dir_count++;
}

void devfs_init(void) {
    /* Root directory inode for /dev */
    inode_t *dev_root = malloc(sizeof(inode_t));
    KERNEL_ASSERT(dev_root != NULL);
    dev_root->ino      = __atomic_fetch_add(&devfs_next_ino, 1ULL, __ATOMIC_RELAXED);
    dev_root->mode     = S_IFDIR | 0755u;
    dev_root->size     = 0;
    dev_root->uid      = 0;
    dev_root->gid      = 0;
    dev_root->rdev     = 0;
    dev_root->nlink    = 2;
    dev_root->refcount = 1;
    dev_root->ops      = &devfs_dir_inode_ops;
    dev_root->lock     = (spinlock_t)SPINLOCK_ZERO;
    dev_root->private  = NULL;

    /* Create device inodes */
    inode_t *null_inode = devfs_alloc_dev_inode(&devfs_null_inode_ops, 1, 3);
    inode_t *zero_inode = devfs_alloc_dev_inode(&devfs_zero_inode_ops, 1, 5);
    inode_t *tty_inode  = devfs_alloc_dev_inode(&devfs_tty_inode_ops,  5, 0);
    KERNEL_ASSERT(null_inode && zero_inode && tty_inode);

    /* Register in the directory table (adds one ref each) */
    devfs_register("null", null_inode);
    devfs_register("zero", zero_inode);
    devfs_register("tty",  tty_inode);

    /* Release our allocation refs; dir table holds the live refs */
    inode_unref(null_inode);
    inode_unref(zero_inode);
    inode_unref(tty_inode);

    /* Mount /dev; vfs_mount holds a ref on dev_root */
    errno_t e = vfs_mount("/dev", dev_root);
    KERNEL_ASSERT(e == EOK);
    inode_unref(dev_root);   /* mount table holds the ref now */
}
