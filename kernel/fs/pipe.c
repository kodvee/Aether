/*
 * pipe.c - anonymous pipe filesystem object.
 *
 * A pipe is a unidirectional byte stream shared between a read-end file_t
 * (O_RDONLY) and a write-end file_t (O_WRONLY) that point to the same
 * underlying inode.  The inode's private pointer holds a pipe_t which
 * contains the ring buffer, open-end counters, and the pipe lock.
 *
 * Blocking behaviour:
 *   pipe_read  - if the buffer is empty and write_open > 0, yields via
 *                schedule() until data arrives (cooperative spin-yield).
 *   pipe_write - if the buffer is full and read_open > 0, yields similarly.
 *   Both return immediately on EOF / EPIPE when the opposite end is closed.
 *
 * Concurrency: pipe_t.lock protects head/tail/count.  read_open and
 * write_open are updated atomically with ACQ_REL; they are only decremented
 * by pipe_close which is called exactly once per file_t (when refcount
 * reaches zero).
 */

#include <kernel/vfs.h>
#include <kernel/scheduler.h>
#include <kernel/mmu.h>
#include <kernel/panic.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define PIPE_BUF 4096

typedef struct {
    uint8_t    data[PIPE_BUF];
    size_t     head;         /* index of next byte to consume          */
    size_t     tail;         /* index of next byte to produce          */
    size_t     count;        /* bytes currently in the buffer          */
    spinlock_t lock;
    uint32_t   read_open;    /* atomic: # live read-end file_t objects  */
    uint32_t   write_open;   /* atomic: # live write-end file_t objects */
} pipe_t;

static _Atomic uint64_t pipe_next_ino = 0x80000000ULL;

/* ------------------------------------------------------------------ */
/* file_ops                                                            */
/* ------------------------------------------------------------------ */

static errno_t pipe_read(file_t *f, void *buf, size_t n, size_t *out) {
    pipe_t *p = (pipe_t *)f->inode->private;
    *out = 0;
    if (!n) return EOK;

    for (;;) {
        bool irq = spinlock_acquire(&p->lock);
        if (p->count > 0) {
            size_t take  = p->count < n ? p->count : n;
            size_t first = PIPE_BUF - p->head;
            if (first > take) first = take;
            __builtin_memcpy(buf, p->data + p->head, first);
            if (first < take)
                __builtin_memcpy((uint8_t *)buf + first, p->data, take - first);
            p->head   = (p->head + take) % PIPE_BUF;
            p->count -= take;
            spinlock_release(&p->lock, irq);
            *out = take;
            return EOK;
        }
        spinlock_release(&p->lock, irq);

        if (__atomic_load_n(&p->write_open, __ATOMIC_ACQUIRE) == 0) {
            *out = 0;   /* EOF: all write ends closed */
            return EOK;
        }

        /* Buffer empty but write end still open; yield and retry. */
        schedule();
    }
}

static errno_t pipe_write(file_t *f, const void *buf, size_t n, size_t *out) {
    pipe_t *p = (pipe_t *)f->inode->private;
    *out = 0;
    if (!n) return EOK;

    if (__atomic_load_n(&p->read_open, __ATOMIC_ACQUIRE) == 0)
        return -EPIPE;

    size_t total = 0;
    while (total < n) {
        bool irq = spinlock_acquire(&p->lock);
        size_t space = PIPE_BUF - p->count;
        if (space > 0) {
            size_t put   = (n - total) < space ? (n - total) : space;
            size_t first = PIPE_BUF - p->tail;
            if (first > put) first = put;
            __builtin_memcpy(p->data + p->tail,
                             (const uint8_t *)buf + total, first);
            if (first < put)
                __builtin_memcpy(p->data,
                                 (const uint8_t *)buf + total + first,
                                 put - first);
            p->tail   = (p->tail + put) % PIPE_BUF;
            p->count += put;
            total    += put;
            spinlock_release(&p->lock, irq);
            continue;
        }
        spinlock_release(&p->lock, irq);

        if (__atomic_load_n(&p->read_open, __ATOMIC_ACQUIRE) == 0) {
            *out = total;
            return total ? EOK : -EPIPE;
        }

        /* Buffer full but read end still open; yield and retry. */
        schedule();
    }
    *out = total;
    return EOK;
}

static void pipe_close(file_t *f) {
    pipe_t *p   = (pipe_t *)f->inode->private;
    int     acc = f->flags & O_ACCMODE;
    if (acc == O_RDONLY)
        __atomic_fetch_sub(&p->read_open,  1u, __ATOMIC_ACQ_REL);
    else
        __atomic_fetch_sub(&p->write_open, 1u, __ATOMIC_ACQ_REL);
    /* Readers / writers polling via schedule() detect the change on their
     * next iteration without any explicit wakeup. */
}

static const file_ops_t pipe_file_ops = {
    .read  = pipe_read,
    .write = pipe_write,
    .seek  = NULL,   /* pipes are not seekable */
    .ioctl = NULL,
    .close = pipe_close,
};

/* ------------------------------------------------------------------ */
/* inode_ops                                                           */
/* ------------------------------------------------------------------ */

static errno_t pipe_inode_open(inode_t *inode, int flags, file_t **out) {
    file_t *f = file_alloc(inode, flags, &pipe_file_ops);
    if (!f) return -ENOMEM;
    *out = f;
    return EOK;
}

static errno_t pipe_inode_stat(inode_t *inode, vfs_stat_t *out) {
    out->ino     = inode->ino;
    out->mode    = inode->mode;
    out->nlink   = 1;
    out->uid     = 0;
    out->gid     = 0;
    out->size    = 0;
    out->blksize = PIPE_BUF;
    out->blocks  = 0;
    out->rdev    = 0;
    return EOK;
}

static void pipe_inode_destroy(inode_t *inode) {
    free(inode->private);   /* free pipe_t */
}

static const inode_ops_t pipe_inode_ops = {
    .lookup  = NULL,
    .create  = NULL,
    .mkdir   = NULL,
    .unlink  = NULL,
    .readdir = NULL,
    .open    = pipe_inode_open,
    .stat    = pipe_inode_stat,
    .destroy = pipe_inode_destroy,
};

/* ------------------------------------------------------------------ */
/* pipe_alloc_files                                                    */
/* ------------------------------------------------------------------ */

/*
 * pipe_alloc_files - allocate a (read, write) file_t pair for a new pipe.
 *
 * Allocates the pipe_t ring buffer and a shared inode.  Calls file_alloc
 * twice (which calls inode_ref each time), then drops the initial inode
 * ref so the inode's lifetime is exactly tied to the two file_t objects.
 *
 * On success, both *rfile_out and *wfile_out have refcount=1.
 * The caller (vfs_pipe) installs them into the fd table.
 *
 * On any allocation failure the routine cleans up completely and returns
 * -ENOMEM; no partial state is left behind.
 */
errno_t pipe_alloc_files(file_t **rfile_out, file_t **wfile_out) {
    pipe_t *p = malloc(sizeof(pipe_t));
    if (!p) return -ENOMEM;
    __builtin_memset(p, 0, sizeof(pipe_t));
    p->lock = (spinlock_t)SPINLOCK_ZERO;
    __atomic_store_n(&p->read_open,  1u, __ATOMIC_RELEASE);
    __atomic_store_n(&p->write_open, 1u, __ATOMIC_RELEASE);

    inode_t *inode = malloc(sizeof(inode_t));
    if (!inode) { free(p); return -ENOMEM; }
    inode->ino      = __atomic_fetch_add(&pipe_next_ino, 1ULL, __ATOMIC_RELAXED);
    inode->mode     = S_IFIFO | 0600u;
    inode->size     = 0;
    inode->uid      = 0;
    inode->gid      = 0;
    inode->rdev     = 0;
    inode->nlink    = 1;
    inode->refcount = 1;   /* initial ref; dropped below after two file_allocs */
    inode->ops      = &pipe_inode_ops;
    inode->lock     = (spinlock_t)SPINLOCK_ZERO;
    inode->private  = p;

    /* file_alloc calls inode_ref, so after each call refcount increments. */
    file_t *rfile = file_alloc(inode, O_RDONLY, &pipe_file_ops);
    if (!rfile) {
        free(inode);
        free(p);
        return -ENOMEM;
    }
    /* inode refcount == 2 */

    file_t *wfile = file_alloc(inode, O_WRONLY, &pipe_file_ops);
    if (!wfile) {
        file_unref(rfile);   /* refcount 1->0: close cb, inode_unref (2->1) */
        inode_unref(inode);  /* refcount 1->0: destroy (frees p), free(inode) */
        return -ENOMEM;
    }
    /* inode refcount == 3 */

    inode_unref(inode);  /* drop initial ref; steady state refcount == 2 */

    *rfile_out = rfile;
    *wfile_out = wfile;
    return EOK;
}
