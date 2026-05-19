/*
 * tmpfs.c - in-memory filesystem (RAM filesystem).
 *
 * tmpfs provides a fully functional read/write filesystem backed entirely
 * by kernel heap memory.  It is the natural choice for "/" before any
 * disk driver exists.
 *
 * Supported operations:
 *   Directories: lookup, create, mkdir, unlink, readdir, stat
 *   Regular files: open (with O_TRUNC), read, write, seek, stat
 *
 * All data is lost on reboot.  No persistence, no journalling.
 *
 * Concurrency: each inode has its own spinlock protecting the private data
 * (directory entries list or file buffer).  Inode metadata (size, nlink)
 * is also protected by the inode lock.
 */

#include <kernel/vfs.h>
#include <kernel/mmu.h>
#include <kernel/panic.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

extern size_t strlen(const char *s);
extern int    strcmp(const char *s1, const char *s2);
extern char  *strdup(const char *s);

/* ------------------------------------------------------------------ */
/* Inode number allocator                                              */
/* ------------------------------------------------------------------ */

static uint64_t tmpfs_next_ino = 1;

static uint64_t alloc_ino(void) {
    return __atomic_fetch_add(&tmpfs_next_ino, 1ULL, __ATOMIC_RELAXED);
}

/* ------------------------------------------------------------------ */
/* tmpfs private data structures                                       */
/* ------------------------------------------------------------------ */

/*
 * tmpfs_file_t - private data for a regular-file inode.
 * Protected by the owning inode's lock.
 */
typedef struct {
    uint8_t *buf;       /* heap buffer holding file content */
    size_t   size;      /* number of valid bytes in buf */
    size_t   capacity;  /* allocated size of buf */
} tmpfs_file_t;

/*
 * tmpfs_dentry_t - one entry in a directory's child list.
 */
typedef struct {
    char        *name;   /* heap-allocated NUL-terminated name */
    inode_t     *inode;  /* held ref on the child inode */
    list_node_t  node;   /* linkage in tmpfs_dir_t.entries */
} tmpfs_dentry_t;

/*
 * tmpfs_dir_t - private data for a directory inode.
 * Protected by the owning inode's lock.
 */
typedef struct {
    list_head_t entries; /* tmpfs_dentry_t nodes */
    uint32_t    count;   /* number of entries (not counting . and ..) */
} tmpfs_dir_t;

/* ------------------------------------------------------------------ */
/* Forward declarations                                                */
/* ------------------------------------------------------------------ */

static const inode_ops_t tmpfs_dir_inode_ops;
static const inode_ops_t tmpfs_file_inode_ops;
static const file_ops_t  tmpfs_file_file_ops;

/* ------------------------------------------------------------------ */
/* Inode allocation                                                    */
/* ------------------------------------------------------------------ */

static inode_t *tmpfs_alloc_inode(uint32_t mode) {
    inode_t *inode = malloc(sizeof(inode_t));
    if (!inode) return NULL;

    inode->ino      = alloc_ino();
    inode->mode     = mode;
    inode->size     = 0;
    inode->uid      = 0;
    inode->gid      = 0;
    inode->rdev     = 0;
    inode->nlink    = 1;
    inode->refcount = 1;
    inode->lock     = (spinlock_t)SPINLOCK_ZERO;
    inode->private  = NULL;

    if (S_ISDIR(mode)) {
        inode->ops = &tmpfs_dir_inode_ops;
        tmpfs_dir_t *d = malloc(sizeof(tmpfs_dir_t));
        if (!d) { free(inode); return NULL; }
        list_head_init(&d->entries);
        d->count    = 0;
        inode->private = d;
    } else {
        inode->ops = &tmpfs_file_inode_ops;
        tmpfs_file_t *f = malloc(sizeof(tmpfs_file_t));
        if (!f) { free(inode); return NULL; }
        f->buf      = NULL;
        f->size     = 0;
        f->capacity = 0;
        inode->private = f;
    }

    return inode;
}

/* ------------------------------------------------------------------ */
/* File buffer growth                                                   */
/* ------------------------------------------------------------------ */

static errno_t tmpfs_file_reserve(tmpfs_file_t *f, size_t needed) {
    if (needed <= f->capacity) return EOK;
    size_t cap = f->capacity ? f->capacity : 64;
    while (cap < needed) cap *= 2;

    uint8_t *nb = malloc(cap);
    if (!nb) return -ENOSPC;

    if (f->buf) {
        __builtin_memcpy(nb, f->buf, f->size);
        free(f->buf);
    }
    /* zero the new region */
    if (cap > f->size)
        __builtin_memset(nb + f->size, 0, cap - f->size);
    f->buf      = nb;
    f->capacity = cap;
    return EOK;
}

/* ------------------------------------------------------------------ */
/* Directory helpers                                                   */
/* ------------------------------------------------------------------ */

/*
 * dir_find - find a dentry by name; caller must hold inode->lock.
 * Returns the dentry or NULL.
 */
static tmpfs_dentry_t *dir_find(tmpfs_dir_t *d, const char *name) {
    list_node_t *n;
    list_for_each(n, &d->entries) {
        tmpfs_dentry_t *de = list_entry(n, tmpfs_dentry_t, node);
        if (strcmp(de->name, name) == 0) return de;
    }
    return NULL;
}

/*
 * dir_add - add a new dentry pointing to child; caller must hold inode->lock.
 * Takes one additional reference on child (directory entry owns a ref).
 * Returns 0 on success, -ENOSPC on OOM.
 */
static errno_t dir_add(tmpfs_dir_t *d, const char *name, inode_t *child) {
    tmpfs_dentry_t *de = malloc(sizeof(tmpfs_dentry_t));
    if (!de) return -ENOSPC;

    de->name = strdup(name);
    if (!de->name) { free(de); return -ENOSPC; }

    inode_ref(child);
    de->inode = child;
    list_node_init(&de->node);
    list_push_back(&d->entries, &de->node);
    d->count++;
    return EOK;
}

/* ------------------------------------------------------------------ */
/* Directory inode operations                                          */
/* ------------------------------------------------------------------ */

static errno_t tmpfs_dir_lookup(inode_t *dir, const char *name,
                                 inode_t **out) {
    tmpfs_dir_t *d = (tmpfs_dir_t *)dir->private;
    bool irq = spinlock_acquire(&dir->lock);
    tmpfs_dentry_t *de = dir_find(d, name);
    if (de) { inode_ref(de->inode); *out = de->inode; }
    spinlock_release(&dir->lock, irq);
    return de ? EOK : -ENOENT;
}

static errno_t tmpfs_dir_create(inode_t *dir, const char *name, uint32_t mode,
                                 inode_t **out) {
    tmpfs_dir_t *d = (tmpfs_dir_t *)dir->private;

    bool irq = spinlock_acquire(&dir->lock);
    if (dir_find(d, name)) {
        spinlock_release(&dir->lock, irq);
        return -EEXIST;
    }
    spinlock_release(&dir->lock, irq);

    inode_t *child = tmpfs_alloc_inode(mode);
    if (!child) return -ENOSPC;

    irq = spinlock_acquire(&dir->lock);
    errno_t e = dir_add(d, name, child);
    spinlock_release(&dir->lock, irq);

    if (e < 0) { inode_unref(child); return e; }

    /* Return with one ref (the caller's ref; dir entry holds another) */
    *out = child;
    return EOK;
}

static errno_t tmpfs_dir_mkdir(inode_t *dir, const char *name, uint32_t mode) {
    inode_t *child = NULL;
    errno_t e = tmpfs_dir_create(dir, name,
                                  (mode & 0777u) | S_IFDIR, &child);
    if (e < 0) return e;
    inode_unref(child);   /* caller doesn't need it; dir holds ref */
    return EOK;
}

static errno_t tmpfs_dir_unlink(inode_t *dir, const char *name) {
    tmpfs_dir_t *d = (tmpfs_dir_t *)dir->private;
    bool irq = spinlock_acquire(&dir->lock);

    tmpfs_dentry_t *de = dir_find(d, name);
    if (!de) { spinlock_release(&dir->lock, irq); return -ENOENT; }

    list_remove(&de->node);
    d->count--;
    spinlock_release(&dir->lock, irq);

    inode_unref(de->inode);
    free(de->name);
    free(de);
    return EOK;
}

static errno_t tmpfs_dir_readdir(inode_t *dir, uint64_t idx,
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

    tmpfs_dir_t *d = (tmpfs_dir_t *)dir->private;
    bool irq = spinlock_acquire(&dir->lock);

    uint64_t i = 0;
    list_node_t *n;
    list_for_each(n, &d->entries) {
        if (i == idx) {
            tmpfs_dentry_t *de = list_entry(n, tmpfs_dentry_t, node);
            size_t nlen = strlen(de->name);
            if (nlen >= name_max) nlen = name_max - 1;
            __builtin_memcpy(name_out, de->name, nlen);
            name_out[nlen] = '\0';
            inode_ref(de->inode);
            *inode_out = de->inode;
            spinlock_release(&dir->lock, irq);
            return EOK;
        }
        i++;
    }
    spinlock_release(&dir->lock, irq);
    return -ENOENT;
}

static errno_t tmpfs_dir_open(inode_t *inode, int flags, file_t **out) {
    (void)flags;
    /* Opening a directory returns a file_t for readdir use; no file ops needed */
    file_t *f = file_alloc(inode, flags, NULL);
    if (!f) return -ENOSPC;
    *out = f;
    return EOK;
}

static errno_t tmpfs_dir_stat(inode_t *inode, vfs_stat_t *out) {
    bool irq = spinlock_acquire(&inode->lock);
    out->ino     = inode->ino;
    out->mode    = inode->mode;
    out->nlink   = inode->nlink + 2;  /* . and .. */
    out->uid     = inode->uid;
    out->gid     = inode->gid;
    out->size    = ((tmpfs_dir_t *)inode->private)->count * 32ULL;
    out->blksize = 4096;
    out->blocks  = 0;
    out->rdev    = 0;
    spinlock_release(&inode->lock, irq);
    return EOK;
}

static void tmpfs_dir_destroy(inode_t *inode) {
    tmpfs_dir_t *d = (tmpfs_dir_t *)inode->private;
    if (!d) return;
    list_node_t *n, *tmp;
    list_for_each_safe(n, tmp, &d->entries) {
        tmpfs_dentry_t *de = list_entry(n, tmpfs_dentry_t, node);
        inode_unref(de->inode);
        free(de->name);
        free(de);
    }
    free(d);
    inode->private = NULL;
}

static errno_t tmpfs_dir_rename(inode_t *old_dir, const char *old_name,
                                 inode_t *new_dir, const char *new_name) {
    tmpfs_dir_t *od = (tmpfs_dir_t *)old_dir->private;
    tmpfs_dir_t *nd = (tmpfs_dir_t *)new_dir->private;

    if (old_dir == new_dir) {
        /* Same-directory rename: just update the dentry name */
        bool irq = spinlock_acquire(&old_dir->lock);
        tmpfs_dentry_t *de = dir_find(od, old_name);
        if (!de) { spinlock_release(&old_dir->lock, irq); return -ENOENT; }
        /* Remove any existing entry with new_name */
        tmpfs_dentry_t *existing = dir_find(od, new_name);
        inode_t *old_inode = NULL;
        if (existing) {
            list_remove(&existing->node);
            od->count--;
            old_inode = existing->inode;
            free(existing->name);
            free(existing);
        }
        char *new_copy = strdup(new_name);
        if (!new_copy) {
            /* Restore existing if we removed it */
            spinlock_release(&old_dir->lock, irq);
            if (old_inode) inode_unref(old_inode);
            return -ENOSPC;
        }
        free(de->name);
        de->name = new_copy;
        spinlock_release(&old_dir->lock, irq);
        if (old_inode) inode_unref(old_inode);
        return EOK;
    }

    /* Cross-directory rename */
    /* Step 1: detach from old_dir */
    bool irq = spinlock_acquire(&old_dir->lock);
    tmpfs_dentry_t *de = dir_find(od, old_name);
    if (!de) { spinlock_release(&old_dir->lock, irq); return -ENOENT; }
    list_remove(&de->node);
    od->count--;
    spinlock_release(&old_dir->lock, irq);

    char *new_copy = strdup(new_name);
    if (!new_copy) {
        /* Roll back */
        irq = spinlock_acquire(&old_dir->lock);
        list_push_back(&od->entries, &de->node);
        od->count++;
        spinlock_release(&old_dir->lock, irq);
        return -ENOSPC;
    }

    /* Step 2: insert into new_dir (displacing any existing entry) */
    irq = spinlock_acquire(&new_dir->lock);
    tmpfs_dentry_t *existing = dir_find(nd, new_name);
    inode_t *old_inode = NULL;
    if (existing) {
        list_remove(&existing->node);
        nd->count--;
        old_inode = existing->inode;
        free(existing->name);
        free(existing);
    }
    free(de->name);
    de->name = new_copy;
    list_node_init(&de->node);
    list_push_back(&nd->entries, &de->node);
    nd->count++;
    spinlock_release(&new_dir->lock, irq);

    if (old_inode) inode_unref(old_inode);
    return EOK;
}

static const inode_ops_t tmpfs_dir_inode_ops = {
    .lookup  = tmpfs_dir_lookup,
    .create  = tmpfs_dir_create,
    .mkdir   = tmpfs_dir_mkdir,
    .unlink  = tmpfs_dir_unlink,
    .readdir = tmpfs_dir_readdir,
    .open    = tmpfs_dir_open,
    .stat    = tmpfs_dir_stat,
    .destroy = tmpfs_dir_destroy,
    .rename  = tmpfs_dir_rename,
};

/* ------------------------------------------------------------------ */
/* File inode operations                                               */
/* ------------------------------------------------------------------ */

static errno_t tmpfs_file_open(inode_t *inode, int flags, file_t **out) {
    if (flags & O_TRUNC) {
        tmpfs_file_t *fd = (tmpfs_file_t *)inode->private;
        bool irq = spinlock_acquire(&inode->lock);
        fd->size  = 0;
        inode->size = 0;
        spinlock_release(&inode->lock, irq);
    }
    file_t *f = file_alloc(inode, flags, &tmpfs_file_file_ops);
    if (!f) return -ENOSPC;
    *out = f;
    return EOK;
}

static errno_t tmpfs_file_stat(inode_t *inode, vfs_stat_t *out) {
    bool irq = spinlock_acquire(&inode->lock);
    out->ino     = inode->ino;
    out->mode    = inode->mode;
    out->nlink   = inode->nlink;
    out->uid     = inode->uid;
    out->gid     = inode->gid;
    out->size    = inode->size;
    out->blksize = 4096;
    out->blocks  = (int64_t)((inode->size + 511) / 512);
    out->rdev    = 0;
    spinlock_release(&inode->lock, irq);
    return EOK;
}

static void tmpfs_file_destroy(inode_t *inode) {
    tmpfs_file_t *f = (tmpfs_file_t *)inode->private;
    if (!f) return;
    if (f->buf) free(f->buf);
    free(f);
    inode->private = NULL;
}

static errno_t tmpfs_file_truncate(inode_t *inode, uint64_t new_size) {
    tmpfs_file_t *tf = (tmpfs_file_t *)inode->private;
    bool irq = spinlock_acquire(&inode->lock);

    if (new_size > tf->size) {
        /* Extend: reserve and zero the gap */
        errno_t e = tmpfs_file_reserve(tf, (size_t)new_size);
        if (e < 0) { spinlock_release(&inode->lock, irq); return e; }
        if (new_size > tf->size)
            __builtin_memset(tf->buf + tf->size, 0,
                             (size_t)(new_size - tf->size));
        tf->size    = (size_t)new_size;
        inode->size = new_size;
    } else if (new_size < tf->size) {
        /* Shrink: just update the length; don't free memory */
        tf->size    = (size_t)new_size;
        inode->size = new_size;
    }

    spinlock_release(&inode->lock, irq);
    return EOK;
}

static const inode_ops_t tmpfs_file_inode_ops = {
    .lookup   = NULL,
    .create   = NULL,
    .mkdir    = NULL,
    .unlink   = NULL,
    .readdir  = NULL,
    .open     = tmpfs_file_open,
    .stat     = tmpfs_file_stat,
    .destroy  = tmpfs_file_destroy,
    .truncate = tmpfs_file_truncate,
};

/* ------------------------------------------------------------------ */
/* File operations (for open file_t)                                   */
/* ------------------------------------------------------------------ */

static errno_t tmpfs_fops_read(file_t *f, void *buf, size_t n,
                                size_t *out) {
    inode_t      *inode = f->inode;
    tmpfs_file_t *tf    = (tmpfs_file_t *)inode->private;
    *out = 0;

    bool f_irq = spinlock_acquire(&f->lock);
    bool i_irq = spinlock_acquire(&inode->lock);

    int64_t pos = f->pos;
    if (pos < 0 || (uint64_t)pos >= tf->size) {
        spinlock_release(&inode->lock, i_irq);
        spinlock_release(&f->lock, f_irq);
        return EOK;   /* EOF */
    }

    size_t avail = tf->size - (size_t)pos;
    size_t chunk = n < avail ? n : avail;
    __builtin_memcpy(buf, tf->buf + pos, chunk);
    f->pos += (int64_t)chunk;
    *out = chunk;

    spinlock_release(&inode->lock, i_irq);
    spinlock_release(&f->lock, f_irq);
    return EOK;
}

static errno_t tmpfs_fops_write(file_t *f, const void *buf, size_t n,
                                 size_t *out) {
    inode_t      *inode = f->inode;
    tmpfs_file_t *tf    = (tmpfs_file_t *)inode->private;
    *out = 0;
    if (n == 0) return EOK;

    bool f_irq = spinlock_acquire(&f->lock);
    bool i_irq = spinlock_acquire(&inode->lock);

    int64_t pos = f->pos;
    if (f->flags & O_APPEND) pos = (int64_t)tf->size;

    size_t  new_end = (size_t)pos + n;
    errno_t e       = tmpfs_file_reserve(tf, new_end);
    if (e < 0) {
        spinlock_release(&inode->lock, i_irq);
        spinlock_release(&f->lock, f_irq);
        return e;
    }

    __builtin_memcpy(tf->buf + pos, buf, n);
    if (new_end > tf->size) {
        tf->size    = new_end;
        inode->size = new_end;
    }
    f->pos = (int64_t)new_end;
    *out   = n;

    spinlock_release(&inode->lock, i_irq);
    spinlock_release(&f->lock, f_irq);
    return EOK;
}

static errno_t tmpfs_fops_seek(file_t *f, int64_t offset, int whence,
                                int64_t *new_pos) {
    inode_t      *inode = f->inode;
    tmpfs_file_t *tf    = (tmpfs_file_t *)inode->private;

    bool f_irq = spinlock_acquire(&f->lock);
    bool i_irq = spinlock_acquire(&inode->lock);

    int64_t base;
    switch (whence) {
    case SEEK_SET: base = 0;                       break;
    case SEEK_CUR: base = f->pos;                  break;
    case SEEK_END: base = (int64_t)tf->size;       break;
    default:
        spinlock_release(&inode->lock, i_irq);
        spinlock_release(&f->lock, f_irq);
        return -EINVAL;
    }

    int64_t np = base + offset;
    if (np < 0) {
        spinlock_release(&inode->lock, i_irq);
        spinlock_release(&f->lock, f_irq);
        return -EINVAL;
    }

    f->pos    = np;
    *new_pos  = np;

    spinlock_release(&inode->lock, i_irq);
    spinlock_release(&f->lock, f_irq);
    return EOK;
}

static const file_ops_t tmpfs_file_file_ops = {
    .read  = tmpfs_fops_read,
    .write = tmpfs_fops_write,
    .seek  = tmpfs_fops_seek,
    .ioctl = NULL,
    .close = NULL,   /* no extra cleanup; file_unref handles inode_unref + free */
};

/* ------------------------------------------------------------------ */
/* Public entry point                                                  */
/* ------------------------------------------------------------------ */

inode_t *tmpfs_create_root(void) {
    return tmpfs_alloc_inode(S_IFDIR | 0755u);
}
