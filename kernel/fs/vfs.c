/*
 * vfs.c - Virtual Filesystem Switch core.
 *
 * Responsibilities:
 *   - Mount table: up to MAX_MOUNTS entries, resolved by longest prefix.
 *   - Path resolution: find mount root then walk components via lookup.
 *   - fd management: allocate/release fd slots in process_t.fd_table.
 *   - Reference counting: inode_ref/unref, file_ref/unref.
 *   - Public vfs_open/read/write/close/fstat/stat/ioctl/seek/mkdir.
 *
 * This file knows nothing about tmpfs or devfs internals.  Filesystem
 * implementations are selected entirely through the inode_ops / file_ops
 * vtables.
 */

#include <kernel/vfs.h>
#include <kernel/scheduler.h>
#include <kernel/mmu.h>
#include <kernel/panic.h>
#include <kernel/spinlock.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* string functions provided by libc/string.c */
extern size_t strlen(const char *s);
extern int    strncmp(const char *s1, const char *s2, size_t n);
extern int    strcmp(const char *s1, const char *s2);

/* ------------------------------------------------------------------ */
/* Mount table                                                          */
/* ------------------------------------------------------------------ */

#define MAX_MOUNTS 16
#define VFS_PATH_MAX 512

typedef struct {
    char      path[VFS_PATH_MAX];
    inode_t  *root;
    bool      active;
} mount_entry_t;

static mount_entry_t mount_table[MAX_MOUNTS];
static spinlock_t    mount_lock = SPINLOCK_ZERO;

/* ------------------------------------------------------------------ */
/* Path helpers                                                        */
/* ------------------------------------------------------------------ */

/*
 * path_canon - canonicalize an absolute path by resolving "." and ".."
 * components via string manipulation (no filesystem access).
 *
 * `in` must start with '/'.  Output is written to `out` (may alias `in`
 * only if they are the same pointer -- we copy `in` to a local buffer
 * first).  Returns true on success, false if the result overflows outsz.
 */
static bool path_canon(const char *in, char *out, size_t outsz) {
    if (!in || in[0] != '/') return false;
    size_t inlen = strlen(in);
    if (inlen >= VFS_PATH_MAX) return false;

    char buf[VFS_PATH_MAX];
    __builtin_memcpy(buf, in, inlen + 1);

    const char *segs[128];
    int         nseg = 0;

    char *p = buf + 1;   /* skip leading '/' */
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;

        char *seg = p;
        while (*p && *p != '/') p++;
        char *seg_end = p;
        if (*p == '/') *p++ = '\0';

        size_t slen = (size_t)(seg_end - seg);
        if (slen == 1 && seg[0] == '.') continue;
        if (slen == 2 && seg[0] == '.' && seg[1] == '.') {
            if (nseg > 0) nseg--;
            continue;
        }
        if (nseg >= 128) return false;
        segs[nseg++] = seg;
    }

    size_t pos = 0;
    if (pos + 1 >= outsz) return false;
    out[pos++] = '/';
    for (int i = 0; i < nseg; i++) {
        size_t slen = strlen(segs[i]);
        if (i > 0) {
            if (pos + 1 >= outsz) return false;
            out[pos++] = '/';
        }
        if (pos + slen >= outsz) return false;
        __builtin_memcpy(out + pos, segs[i], slen);
        pos += slen;
    }
    out[pos] = '\0';
    return true;
}

/*
 * path_to_abs - construct an absolute path from a possibly-relative one.
 *
 * If path starts with '/' it is just canonicalized.
 * Otherwise proc->cwd_path is prepended (a snapshot is taken under
 * proc->lock to avoid races with concurrent chdir).  proc may be NULL,
 * in which case "/" is assumed as the CWD.
 *
 * Returns true on success; false if the result would exceed outsz.
 */
static bool path_to_abs(process_t *proc, const char *path,
                         char *out, size_t outsz) {
    if (!path || !out || outsz < 2) return false;

    if (path[0] == '/') return path_canon(path, out, outsz);

    /* Relative path: snapshot cwd under lock */
    char cwd[VFS_PATH_MAX];
    if (proc) {
        bool irq = spinlock_acquire(&proc->lock);
        size_t cwdlen = strlen(proc->cwd_path);
        if (cwdlen >= VFS_PATH_MAX) { spinlock_release(&proc->lock, irq); return false; }
        __builtin_memcpy(cwd, proc->cwd_path, cwdlen + 1);
        spinlock_release(&proc->lock, irq);
    } else {
        cwd[0] = '/'; cwd[1] = '\0';
    }

    size_t cwdlen  = strlen(cwd);
    bool   sep     = (cwd[cwdlen - 1] != '/');
    size_t pathlen = strlen(path);
    if (cwdlen + (sep ? 1u : 0u) + pathlen >= VFS_PATH_MAX) return false;

    char raw[VFS_PATH_MAX];
    __builtin_memcpy(raw, cwd, cwdlen);
    size_t pos = cwdlen;
    if (sep) raw[pos++] = '/';
    __builtin_memcpy(raw + pos, path, pathlen + 1);

    return path_canon(raw, out, outsz);
}

/* ------------------------------------------------------------------ */
/* Reference counting                                                  */
/* ------------------------------------------------------------------ */

void inode_ref(inode_t *inode) {
    KERNEL_ASSERT(inode != NULL);
    __atomic_fetch_add(&inode->refcount, 1u, __ATOMIC_RELAXED);
}

void inode_unref(inode_t *inode) {
    if (!inode) return;
    uint32_t old = __atomic_fetch_sub(&inode->refcount, 1u, __ATOMIC_ACQ_REL);
    KERNEL_ASSERT(old > 0);
    if (old == 1) {
        if (inode->ops && inode->ops->destroy)
            inode->ops->destroy(inode);
        free(inode);
    }
}

void file_ref(file_t *file) {
    KERNEL_ASSERT(file != NULL);
    __atomic_fetch_add(&file->refcount, 1u, __ATOMIC_RELAXED);
}

void file_unref(file_t *file) {
    if (!file) return;
    uint32_t old = __atomic_fetch_sub(&file->refcount, 1u, __ATOMIC_ACQ_REL);
    KERNEL_ASSERT(old > 0);
    if (old == 1) {
        if (file->ops && file->ops->close)
            file->ops->close(file);
        inode_unref(file->inode);
        free(file);
    }
}

/* ------------------------------------------------------------------ */
/* file_alloc helper for filesystem implementations                    */
/* ------------------------------------------------------------------ */

file_t *file_alloc(inode_t *inode, int flags, const file_ops_t *ops) {
    file_t *f = malloc(sizeof(file_t));
    if (!f) return NULL;
    inode_ref(inode);
    f->inode    = inode;
    f->flags    = flags;
    f->pos      = 0;
    f->refcount = 1;
    f->ops      = ops;
    f->lock     = (spinlock_t)SPINLOCK_ZERO;
    f->private  = NULL;
    return f;
}

/* ------------------------------------------------------------------ */
/* fd_table helpers                                                    */
/* ------------------------------------------------------------------ */

void fd_table_init(fd_table_t *fdt) {
    __builtin_memset(fdt, 0, sizeof(fd_table_t));
}

void fd_table_destroy(fd_table_t *fdt) {
    for (int i = 0; i < FD_TABLE_SIZE; i++) {
        if (fdt->files[i]) {
            file_unref(fdt->files[i]);
            fdt->files[i] = NULL;
        }
    }
}

/* Find a free slot >= start; caller must hold proc->lock. */
static int fd_alloc_from(process_t *proc, int start) {
    for (int i = start; i < FD_TABLE_SIZE; i++) {
        if (!proc->fd_table.files[i]) return i;
    }
    return -1;
}

/* Find the lowest free slot; caller must hold proc->lock. */
static int fd_alloc(process_t *proc) {
    return fd_alloc_from(proc, 0);
}

/*
 * fd_get - look up fd in proc, increment the file's refcount, and return it.
 * Caller must NOT hold proc->lock.
 * Returns NULL with *err set to -EBADF if fd is invalid or closed.
 */
static file_t *fd_get(process_t *proc, int fd, errno_t *err) {
    if (fd < 0 || fd >= FD_TABLE_SIZE) { *err = -EBADF; return NULL; }
    bool irq = spinlock_acquire(&proc->lock);
    file_t *f = proc->fd_table.files[fd];
    if (f) file_ref(f);
    spinlock_release(&proc->lock, irq);
    if (!f) { *err = -EBADF; return NULL; }
    return f;
}

/* ------------------------------------------------------------------ */
/* Path helpers                                                        */
/* ------------------------------------------------------------------ */

/*
 * path_split - split an absolute path into parent directory path and basename.
 *
 * Examples:
 *   "/dev/null"  -> parent="/dev",  basename="null"
 *   "/dev"       -> parent="/",     basename="dev"
 *   "/"          -> parent="/",     basename="" (cannot create root)
 *
 * parent_out must be at least VFS_PATH_MAX bytes.
 * Returns true on success, false if path is too long or malformed.
 */
static bool path_split(const char *path, char *parent_out,
                        const char **basename_out) {
    size_t len = strlen(path);
    if (len == 0 || path[0] != '/') return false;

    /* find the last '/' */
    const char *last_slash = path + len - 1;
    while (last_slash > path && *last_slash != '/') last_slash--;

    /* everything before the last '/' is the parent */
    size_t plen;
    if (last_slash == path) {
        /* parent is root "/" */
        parent_out[0] = '/';
        parent_out[1] = '\0';
        plen = 1;
    } else {
        plen = (size_t)(last_slash - path);
        if (plen >= VFS_PATH_MAX) return false;
        __builtin_memcpy(parent_out, path, plen);
        parent_out[plen] = '\0';
    }
    (void)plen;

    *basename_out = last_slash + 1;
    return true;
}

/* ------------------------------------------------------------------ */
/* Path resolution                                                     */
/* ------------------------------------------------------------------ */

/*
 * vfs_resolve - resolve an absolute path to an inode.
 *
 * Returns the inode with one reference held, or NULL with *err_out set.
 * Walks the path component-by-component using inode_ops->lookup.
 */
static inode_t *vfs_resolve(const char *path, errno_t *err_out) {
    if (!path || path[0] != '/') { *err_out = -EINVAL; return NULL; }

    /* Find the longest-prefix mount point. */
    inode_t *root     = NULL;
    size_t   best_len = 0;

    bool irq = spinlock_acquire(&mount_lock);
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (!mount_table[i].active) continue;
        const char *mp   = mount_table[i].path;
        size_t      mlen = strlen(mp);

        bool matches;
        if (mlen == 1 && mp[0] == '/') {
            /* "/" matches any absolute path */
            matches = true;
        } else {
            matches = (mlen <= strlen(path)) &&
                      (strncmp(path, mp, mlen) == 0) &&
                      (path[mlen] == '/' || path[mlen] == '\0');
        }

        if (matches && mlen > best_len) {
            best_len = mlen;
            root = mount_table[i].root;
        }
    }
    if (root) inode_ref(root);
    spinlock_release(&mount_lock, irq);

    if (!root) { *err_out = -ENOENT; return NULL; }

    /* Walk the remaining path components. */
    const char *p = path + best_len;
    while (*p == '/') p++;   /* strip leading slashes after mount prefix */

    inode_t *cur = root;

    while (*p) {
        const char *start = p;
        while (*p && *p != '/') p++;
        size_t clen = (size_t)(p - start);
        while (*p == '/') p++;

        if (clen == 0) continue;
        if (clen >= 256) {
            inode_unref(cur);
            *err_out = -ENAMETOOLONG;
            return NULL;
        }

        char name[256];
        __builtin_memcpy(name, start, clen);
        name[clen] = '\0';

        if (!S_ISDIR(cur->mode)) {
            inode_unref(cur);
            *err_out = -ENOTDIR;
            return NULL;
        }
        if (!cur->ops || !cur->ops->lookup) {
            inode_unref(cur);
            *err_out = -ENOSYS;
            return NULL;
        }

        inode_t *next = NULL;
        errno_t   e   = cur->ops->lookup(cur, name, &next);
        inode_unref(cur);
        if (e < 0) { *err_out = e; return NULL; }
        cur = next;
    }

    *err_out = EOK;
    return cur;
}

/* ------------------------------------------------------------------ */
/* VFS init and mount                                                  */
/* ------------------------------------------------------------------ */

void vfs_init(void) {
    __builtin_memset(mount_table, 0, sizeof(mount_table));
    mount_lock = (spinlock_t)SPINLOCK_ZERO;
}

errno_t vfs_mount(const char *path, inode_t *root) {
    if (!path || !root || path[0] != '/') return -EINVAL;
    size_t plen = strlen(path);
    if (plen >= VFS_PATH_MAX) return -ENAMETOOLONG;

    bool irq = spinlock_acquire(&mount_lock);

    /* reject duplicate mount points */
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (mount_table[i].active && strcmp(mount_table[i].path, path) == 0) {
            spinlock_release(&mount_lock, irq);
            return -EBUSY;
        }
    }

    /* find a free slot */
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (!mount_table[i].active) {
            __builtin_memcpy(mount_table[i].path, path, plen + 1);
            mount_table[i].root   = root;
            mount_table[i].active = true;
            inode_ref(root);
            spinlock_release(&mount_lock, irq);
            return EOK;
        }
    }

    spinlock_release(&mount_lock, irq);
    return -ENOMEM;
}

/* ------------------------------------------------------------------ */
/* vfs_open                                                            */
/* ------------------------------------------------------------------ */

errno_t vfs_open(process_t *proc, const char *path, int flags, uint32_t mode,
                 int *fd_out) {
    if (!proc || !path || !fd_out) return -EINVAL;

    char abspath[VFS_PATH_MAX];
    if (!path_to_abs(proc, path, abspath, sizeof(abspath))) return -ENAMETOOLONG;

    errno_t  err   = 0;
    inode_t *inode = vfs_resolve(abspath, &err);

    if (!inode) {
        if (err != -ENOENT || !(flags & O_CREAT)) return err;

        /* O_CREAT: resolve parent dir and create the file */
        char        parent[VFS_PATH_MAX];
        const char *basename = NULL;
        if (!path_split(abspath, parent, &basename) || !basename || !*basename)
            return -EINVAL;

        errno_t  perr = 0;
        inode_t *dir  = vfs_resolve(parent, &perr);
        if (!dir) return perr;

        if (!S_ISDIR(dir->mode) || !dir->ops || !dir->ops->create) {
            inode_unref(dir);
            return -ENOTDIR;
        }

        uint32_t eff_mode = ((mode & 0777u) & ~proc->umask) | S_IFREG;
        err = dir->ops->create(dir, basename, eff_mode, &inode);
        inode_unref(dir);
        if (err < 0) return err;

    } else if ((flags & O_CREAT) && (flags & O_EXCL)) {
        inode_unref(inode);
        return -EEXIST;
    }

    /* O_DIRECTORY: caller requires a directory */
    if ((flags & O_DIRECTORY) && !S_ISDIR(inode->mode)) {
        inode_unref(inode);
        return -ENOTDIR;
    }

    if (!inode->ops || !inode->ops->open) {
        inode_unref(inode);
        return -ENOSYS;
    }

    file_t *file = NULL;
    err = inode->ops->open(inode, flags, &file);
    inode_unref(inode);   /* open holds its own ref via file_alloc */
    if (err < 0) return err;

    bool irq = spinlock_acquire(&proc->lock);
    int fd = fd_alloc(proc);
    if (fd < 0) {
        spinlock_release(&proc->lock, irq);
        file_unref(file);
        return -EMFILE;
    }
    proc->fd_table.files[fd]  = file;
    proc->fd_table.cloexec[fd] = (flags & O_CLOEXEC) ? 1 : 0;
    spinlock_release(&proc->lock, irq);

    *fd_out = fd;
    return EOK;
}

/* ------------------------------------------------------------------ */
/* vfs_close                                                           */
/* ------------------------------------------------------------------ */

errno_t vfs_close(process_t *proc, int fd) {
    if (!proc || fd < 0 || fd >= FD_TABLE_SIZE) return -EBADF;

    bool irq = spinlock_acquire(&proc->lock);
    file_t *f = proc->fd_table.files[fd];
    if (!f) { spinlock_release(&proc->lock, irq); return -EBADF; }
    proc->fd_table.files[fd] = NULL;
    spinlock_release(&proc->lock, irq);

    file_unref(f);
    return EOK;
}

/* ------------------------------------------------------------------ */
/* vfs_read                                                            */
/* ------------------------------------------------------------------ */

errno_t vfs_read(process_t *proc, int fd, void *buf, size_t count,
                 size_t *bytes_read) {
    if (!proc || !buf || !bytes_read) return -EINVAL;
    *bytes_read = 0;

    errno_t  err = 0;
    file_t  *f   = fd_get(proc, fd, &err);
    if (!f) return err;

    int acc = f->flags & O_ACCMODE;
    if (acc == O_WRONLY) { file_unref(f); return -EBADF; }

    if (S_ISDIR(f->inode->mode)) { file_unref(f); return -EISDIR; }

    if (!f->ops || !f->ops->read) { file_unref(f); return -ENOSYS; }

    err = f->ops->read(f, buf, count, bytes_read);
    file_unref(f);
    return err;
}

/* ------------------------------------------------------------------ */
/* vfs_write                                                           */
/* ------------------------------------------------------------------ */

errno_t vfs_write(process_t *proc, int fd, const void *buf, size_t count,
                  size_t *bytes_written) {
    if (!proc || !buf || !bytes_written) return -EINVAL;
    *bytes_written = 0;

    errno_t  err = 0;
    file_t  *f   = fd_get(proc, fd, &err);
    if (!f) return err;

    int acc = f->flags & O_ACCMODE;
    if (acc == O_RDONLY) { file_unref(f); return -EBADF; }

    if (S_ISDIR(f->inode->mode)) { file_unref(f); return -EISDIR; }

    if (!f->ops || !f->ops->write) { file_unref(f); return -ENOSYS; }

    err = f->ops->write(f, buf, count, bytes_written);
    file_unref(f);
    return err;
}

/* ------------------------------------------------------------------ */
/* vfs_fstat                                                           */
/* ------------------------------------------------------------------ */

errno_t vfs_fstat(process_t *proc, int fd, vfs_stat_t *out) {
    if (!proc || !out) return -EINVAL;

    errno_t err = 0;
    file_t *f   = fd_get(proc, fd, &err);
    if (!f) return err;

    if (!f->inode->ops || !f->inode->ops->stat) {
        file_unref(f);
        return -ENOSYS;
    }

    err = f->inode->ops->stat(f->inode, out);
    file_unref(f);
    return err;
}

/* ------------------------------------------------------------------ */
/* vfs_stat                                                            */
/* ------------------------------------------------------------------ */

errno_t vfs_stat(process_t *proc, const char *path, vfs_stat_t *out) {
    if (!path || !out) return -EINVAL;

    char abspath[VFS_PATH_MAX];
    if (!path_to_abs(proc, path, abspath, sizeof(abspath))) return -ENAMETOOLONG;

    errno_t  err   = 0;
    inode_t *inode = vfs_resolve(abspath, &err);
    if (!inode) return err;

    if (!inode->ops || !inode->ops->stat) {
        inode_unref(inode);
        return -ENOSYS;
    }

    err = inode->ops->stat(inode, out);
    inode_unref(inode);
    return err;
}

errno_t vfs_statat(process_t *proc, int at_fd, const char *path, int flags,
                   vfs_stat_t *out) {
    if (!path || !out) return -EINVAL;
    /* AT_EMPTY_PATH: stat the fd itself */
    if ((flags & AT_EMPTY_PATH) && path[0] == '\0')
        return vfs_fstat(proc, at_fd, out);
    /* Absolute paths and AT_FDCWD both handled by vfs_stat via path_to_abs */
    if (path[0] == '/' || at_fd == AT_FDCWD)
        return vfs_stat(proc, path, out);
    return -ENOSYS;   /* relative path + non-CWD dirfd not supported */
}

/* ------------------------------------------------------------------ */
/* vfs_ioctl                                                           */
/* ------------------------------------------------------------------ */

errno_t vfs_ioctl(process_t *proc, int fd, unsigned long req, uintptr_t arg) {
    if (!proc) return -EINVAL;

    errno_t err = 0;
    file_t *f   = fd_get(proc, fd, &err);
    if (!f) return err;

    if (!f->ops || !f->ops->ioctl) { file_unref(f); return -ENOTTY; }

    err = f->ops->ioctl(f, req, arg);
    file_unref(f);
    return err;
}

/* ------------------------------------------------------------------ */
/* vfs_seek                                                            */
/* ------------------------------------------------------------------ */

errno_t vfs_seek(process_t *proc, int fd, int64_t offset, int whence,
                 int64_t *new_pos) {
    if (!proc || !new_pos) return -EINVAL;

    errno_t err = 0;
    file_t *f   = fd_get(proc, fd, &err);
    if (!f) return err;

    if (!f->ops || !f->ops->seek) {
        /* Directories have no seek op; allow SEEK_SET and SEEK_CUR(0) on
         * the position index so getdents64 can peek and rewind. */
        if (S_ISDIR(f->inode->mode)) {
            bool irq = spinlock_acquire(&f->lock);
            if (whence == SEEK_SET && offset >= 0) {
                f->pos   = offset;
                *new_pos = offset;
                spinlock_release(&f->lock, irq);
                file_unref(f);
                return EOK;
            }
            if (whence == SEEK_CUR && offset == 0) {
                *new_pos = f->pos;
                spinlock_release(&f->lock, irq);
                file_unref(f);
                return EOK;
            }
            spinlock_release(&f->lock, irq);
        }
        file_unref(f);
        return -ENOSYS;
    }

    err = f->ops->seek(f, offset, whence, new_pos);
    file_unref(f);
    return err;
}

/* ------------------------------------------------------------------ */
/* vfs_mkdir                                                           */
/* ------------------------------------------------------------------ */

errno_t vfs_mkdir(process_t *proc, const char *path, uint32_t mode) {
    if (!path) return -EINVAL;

    char abspath[VFS_PATH_MAX];
    if (!path_to_abs(proc, path, abspath, sizeof(abspath))) return -ENAMETOOLONG;

    char        parent[VFS_PATH_MAX];
    const char *basename = NULL;
    if (!path_split(abspath, parent, &basename) || !basename || !*basename)
        return -EINVAL;

    errno_t  err = 0;
    inode_t *dir = vfs_resolve(parent, &err);
    if (!dir) return err;

    if (!S_ISDIR(dir->mode) || !dir->ops || !dir->ops->mkdir) {
        inode_unref(dir);
        return -ENOTDIR;
    }

    uint32_t eff_mode = (mode & 0777u) & (proc ? ~proc->umask : 0777u);
    err = dir->ops->mkdir(dir, basename, eff_mode);
    inode_unref(dir);
    return err;
}

/* ------------------------------------------------------------------ */
/* vfs_setup_std_fds                                                   */
/* ------------------------------------------------------------------ */

void vfs_setup_std_fds(process_t *proc) {
    int fd;
    /* fd 0: stdin  - open /dev/tty read-only */
    if (vfs_open(proc, "/dev/tty", O_RDONLY, 0, &fd) < 0)
        vfs_open(proc, "/dev/null", O_RDONLY, 0, &fd);

    /* fd 1: stdout - open /dev/tty write-only */
    if (vfs_open(proc, "/dev/tty", O_WRONLY, 0, &fd) < 0)
        vfs_open(proc, "/dev/null", O_WRONLY, 0, &fd);

    /* fd 2: stderr - open /dev/tty write-only */
    if (vfs_open(proc, "/dev/tty", O_WRONLY, 0, &fd) < 0)
        vfs_open(proc, "/dev/null", O_WRONLY, 0, &fd);
}

/* ------------------------------------------------------------------ */
/* vfs_readdir                                                         */
/* ------------------------------------------------------------------ */

errno_t vfs_readdir(process_t *proc, int fd,
                    char *name_out, size_t name_max,
                    uint64_t *ino_out, uint32_t *mode_out) {
    if (!proc || !name_out || !ino_out || !mode_out) return -EINVAL;

    errno_t err = 0;
    file_t *f   = fd_get(proc, fd, &err);
    if (!f) return err;

    inode_t *inode = f->inode;
    if (!S_ISDIR(inode->mode)) { file_unref(f); return -ENOTDIR; }
    if (!inode->ops || !inode->ops->readdir) { file_unref(f); return -ENOSYS; }

    bool irq = spinlock_acquire(&f->lock);
    uint64_t idx = (uint64_t)(f->pos < 0 ? 0 : f->pos);
    spinlock_release(&f->lock, irq);

    inode_t *child = NULL;
    err = inode->ops->readdir(inode, idx, name_out, name_max, &child);
    if (err == EOK) {
        *ino_out  = child->ino;
        *mode_out = child->mode;
        inode_unref(child);

        irq = spinlock_acquire(&f->lock);
        f->pos++;
        spinlock_release(&f->lock, irq);
    }

    file_unref(f);
    return err;
}

/* ------------------------------------------------------------------ */
/* vfs_dup / vfs_dup2                                                  */
/* ------------------------------------------------------------------ */

errno_t vfs_dup(process_t *proc, int oldfd, int *newfd_out) {
    if (!proc || !newfd_out) return -EINVAL;
    if (oldfd < 0 || oldfd >= FD_TABLE_SIZE) return -EBADF;

    bool irq = spinlock_acquire(&proc->lock);
    file_t *f = proc->fd_table.files[oldfd];
    if (!f) { spinlock_release(&proc->lock, irq); return -EBADF; }

    int newfd = fd_alloc(proc);
    if (newfd < 0) { spinlock_release(&proc->lock, irq); return -EMFILE; }

    file_ref(f);
    proc->fd_table.files[newfd]  = f;
    proc->fd_table.cloexec[newfd] = 0;   /* dup never inherits FD_CLOEXEC */
    spinlock_release(&proc->lock, irq);

    *newfd_out = newfd;
    return EOK;
}

errno_t vfs_dup2(process_t *proc, int oldfd, int newfd) {
    if (!proc) return -EINVAL;
    if (oldfd < 0 || oldfd >= FD_TABLE_SIZE) return -EBADF;
    if (newfd < 0 || newfd >= FD_TABLE_SIZE) return -EBADF;
    if (oldfd == newfd) return EOK;

    bool irq = spinlock_acquire(&proc->lock);
    file_t *src = proc->fd_table.files[oldfd];
    if (!src) { spinlock_release(&proc->lock, irq); return -EBADF; }

    file_t *old = proc->fd_table.files[newfd];
    file_ref(src);
    proc->fd_table.files[newfd]  = src;
    proc->fd_table.cloexec[newfd] = 0;   /* dup2 never inherits FD_CLOEXEC */
    spinlock_release(&proc->lock, irq);

    if (old) file_unref(old);   /* close previous occupant outside the lock */
    return EOK;
}

/* ------------------------------------------------------------------ */
/* vfs_pipe                                                            */
/* ------------------------------------------------------------------ */

errno_t vfs_pipe(process_t *proc, int fds[2]) {
    if (!proc || !fds) return -EINVAL;

    file_t *rfile = NULL, *wfile = NULL;
    errno_t err = pipe_alloc_files(&rfile, &wfile);
    if (err < 0) return err;

    bool irq = spinlock_acquire(&proc->lock);
    int rfd = fd_alloc(proc);
    if (rfd < 0) {
        spinlock_release(&proc->lock, irq);
        file_unref(rfile);
        file_unref(wfile);
        return -EMFILE;
    }
    proc->fd_table.files[rfd] = rfile;

    int wfd = fd_alloc(proc);
    if (wfd < 0) {
        proc->fd_table.files[rfd] = NULL;
        spinlock_release(&proc->lock, irq);
        file_unref(rfile);
        file_unref(wfile);
        return -EMFILE;
    }
    proc->fd_table.files[wfd] = wfile;
    spinlock_release(&proc->lock, irq);

    fds[0] = rfd;
    fds[1] = wfd;
    return EOK;
}

/* ------------------------------------------------------------------ */
/* vfs_pipe2                                                           */
/* ------------------------------------------------------------------ */

errno_t vfs_pipe2(process_t *proc, int fds[2], int flags) {
    if (!proc || !fds) return -EINVAL;
    if (flags & ~(O_CLOEXEC | O_NONBLOCK)) return -EINVAL;

    file_t *rfile = NULL, *wfile = NULL;
    errno_t err = pipe_alloc_files(&rfile, &wfile);
    if (err < 0) return err;

    if (flags & O_NONBLOCK) {
        rfile->flags |= O_NONBLOCK;
        wfile->flags |= O_NONBLOCK;
    }

    bool irq = spinlock_acquire(&proc->lock);
    int rfd = fd_alloc(proc);
    if (rfd < 0) {
        spinlock_release(&proc->lock, irq);
        file_unref(rfile); file_unref(wfile);
        return -EMFILE;
    }
    proc->fd_table.files[rfd]  = rfile;
    proc->fd_table.cloexec[rfd] = (flags & O_CLOEXEC) ? 1 : 0;

    int wfd = fd_alloc(proc);
    if (wfd < 0) {
        proc->fd_table.files[rfd]  = NULL;
        proc->fd_table.cloexec[rfd] = 0;
        spinlock_release(&proc->lock, irq);
        file_unref(rfile); file_unref(wfile);
        return -EMFILE;
    }
    proc->fd_table.files[wfd]  = wfile;
    proc->fd_table.cloexec[wfd] = (flags & O_CLOEXEC) ? 1 : 0;
    spinlock_release(&proc->lock, irq);

    fds[0] = rfd;
    fds[1] = wfd;
    return EOK;
}

/* ------------------------------------------------------------------ */
/* vfs_dup3                                                            */
/* ------------------------------------------------------------------ */

errno_t vfs_dup3(process_t *proc, int oldfd, int newfd, int flags) {
    if (!proc) return -EINVAL;
    if (oldfd < 0 || oldfd >= FD_TABLE_SIZE) return -EBADF;
    if (newfd < 0 || newfd >= FD_TABLE_SIZE) return -EBADF;
    if (oldfd == newfd) return -EINVAL;   /* dup3 requires oldfd != newfd */
    if (flags & ~O_CLOEXEC) return -EINVAL;

    bool irq = spinlock_acquire(&proc->lock);
    file_t *src = proc->fd_table.files[oldfd];
    if (!src) { spinlock_release(&proc->lock, irq); return -EBADF; }
    file_t *old = proc->fd_table.files[newfd];
    file_ref(src);
    proc->fd_table.files[newfd]  = src;
    proc->fd_table.cloexec[newfd] = (flags & O_CLOEXEC) ? 1 : 0;
    spinlock_release(&proc->lock, irq);

    if (old) file_unref(old);
    return EOK;
}

/* ------------------------------------------------------------------ */
/* vfs_chdir / vfs_getcwd                                              */
/* ------------------------------------------------------------------ */

errno_t vfs_chdir(process_t *proc, const char *path) {
    if (!proc || !path) return -EINVAL;

    char abspath[VFS_PATH_MAX];
    if (!path_to_abs(proc, path, abspath, sizeof(abspath))) return -ENAMETOOLONG;

    errno_t  err   = 0;
    inode_t *inode = vfs_resolve(abspath, &err);
    if (!inode) return err;

    if (!S_ISDIR(inode->mode)) { inode_unref(inode); return -ENOTDIR; }

    bool irq = spinlock_acquire(&proc->lock);
    inode_t *old_cwd = proc->cwd;
    proc->cwd = inode;
    size_t len = strlen(abspath);
    if (len >= VFS_PATH_MAX) len = VFS_PATH_MAX - 1;
    __builtin_memcpy(proc->cwd_path, abspath, len + 1);
    spinlock_release(&proc->lock, irq);

    if (old_cwd) inode_unref(old_cwd);
    return EOK;
}

errno_t vfs_getcwd(process_t *proc, char *buf, size_t size) {
    if (!proc || !buf || size == 0) return -EINVAL;

    bool irq = spinlock_acquire(&proc->lock);
    size_t len = strlen(proc->cwd_path);
    if (len + 1 > size) { spinlock_release(&proc->lock, irq); return -ERANGE; }
    __builtin_memcpy(buf, proc->cwd_path, len + 1);
    spinlock_release(&proc->lock, irq);
    return EOK;
}

/* ------------------------------------------------------------------ */
/* vfs_unlink                                                          */
/* ------------------------------------------------------------------ */

errno_t vfs_unlink(process_t *proc, const char *path) {
    if (!path) return -EINVAL;

    char abspath[VFS_PATH_MAX];
    if (!path_to_abs(proc, path, abspath, sizeof(abspath))) return -ENAMETOOLONG;

    char        parent[VFS_PATH_MAX];
    const char *basename = NULL;
    if (!path_split(abspath, parent, &basename) || !basename || !*basename)
        return -EINVAL;

    errno_t  err = 0;
    inode_t *dir = vfs_resolve(parent, &err);
    if (!dir) return err;

    if (!S_ISDIR(dir->mode) || !dir->ops || !dir->ops->unlink) {
        inode_unref(dir);
        return -ENOTDIR;
    }

    /* Reject attempt to unlink a directory (use rmdir instead) */
    inode_t *target = NULL;
    errno_t e2 = dir->ops->lookup(dir, basename, &target);
    if (e2 == EOK) {
        bool is_dir = S_ISDIR(target->mode);
        inode_unref(target);
        if (is_dir) { inode_unref(dir); return -EISDIR; }
    }

    err = dir->ops->unlink(dir, basename);
    inode_unref(dir);
    return err;
}

/* ------------------------------------------------------------------ */
/* vfs_rmdir                                                           */
/* ------------------------------------------------------------------ */

errno_t vfs_rmdir(process_t *proc, const char *path) {
    if (!path) return -EINVAL;

    char abspath[VFS_PATH_MAX];
    if (!path_to_abs(proc, path, abspath, sizeof(abspath))) return -ENAMETOOLONG;

    char        parent[VFS_PATH_MAX];
    const char *basename = NULL;
    if (!path_split(abspath, parent, &basename) || !basename || !*basename)
        return -EINVAL;

    errno_t  err = 0;
    inode_t *dir = vfs_resolve(parent, &err);
    if (!dir) return err;

    if (!S_ISDIR(dir->mode) || !dir->ops || !dir->ops->lookup) {
        inode_unref(dir); return -ENOTDIR;
    }

    inode_t *target = NULL;
    err = dir->ops->lookup(dir, basename, &target);
    if (err < 0) { inode_unref(dir); return err; }

    if (!S_ISDIR(target->mode)) {
        inode_unref(target); inode_unref(dir); return -ENOTDIR;
    }

    /* Check empty: idx 1 should not exist (idx 0 is ".") */
    if (target->ops && target->ops->readdir) {
        char       tmp_name[2];
        inode_t   *tmp_child = NULL;
        errno_t    empty     = target->ops->readdir(target, 1, tmp_name,
                                                     sizeof(tmp_name), &tmp_child);
        if (empty == EOK) {
            inode_unref(tmp_child);
            inode_unref(target); inode_unref(dir);
            return -ENOTEMPTY;
        }
    }
    inode_unref(target);

    if (!dir->ops->unlink) { inode_unref(dir); return -ENOSYS; }
    err = dir->ops->unlink(dir, basename);
    inode_unref(dir);
    return err;
}

/* ------------------------------------------------------------------ */
/* vfs_rename                                                          */
/* ------------------------------------------------------------------ */

errno_t vfs_rename(process_t *proc, const char *oldpath, const char *newpath) {
    if (!oldpath || !newpath) return -EINVAL;

    char absold[VFS_PATH_MAX], absnew[VFS_PATH_MAX];
    if (!path_to_abs(proc, oldpath, absold, sizeof(absold))) return -ENAMETOOLONG;
    if (!path_to_abs(proc, newpath, absnew, sizeof(absnew))) return -ENAMETOOLONG;

    char        old_parent[VFS_PATH_MAX], new_parent[VFS_PATH_MAX];
    const char *old_base = NULL, *new_base = NULL;
    if (!path_split(absold, old_parent, &old_base) || !old_base || !*old_base)
        return -EINVAL;
    if (!path_split(absnew, new_parent, &new_base) || !new_base || !*new_base)
        return -EINVAL;

    errno_t  err    = 0;
    inode_t *od = vfs_resolve(old_parent, &err);
    if (!od) return err;

    inode_t *nd = NULL;
    if (strcmp(old_parent, new_parent) == 0) {
        nd = od; inode_ref(nd);
    } else {
        nd = vfs_resolve(new_parent, &err);
        if (!nd) { inode_unref(od); return err; }
    }

    if (!S_ISDIR(od->mode) || !S_ISDIR(nd->mode)) {
        inode_unref(od); inode_unref(nd); return -ENOTDIR;
    }
    if (!od->ops || !od->ops->rename) {
        inode_unref(od); inode_unref(nd); return -ENOSYS;
    }

    err = od->ops->rename(od, old_base, nd, new_base);
    inode_unref(od);
    inode_unref(nd);
    return err;
}

/* ------------------------------------------------------------------ */
/* vfs_truncate / vfs_ftruncate                                        */
/* ------------------------------------------------------------------ */

errno_t vfs_truncate(process_t *proc, const char *path, uint64_t size) {
    if (!path) return -EINVAL;

    char abspath[VFS_PATH_MAX];
    if (!path_to_abs(proc, path, abspath, sizeof(abspath))) return -ENAMETOOLONG;

    errno_t  err   = 0;
    inode_t *inode = vfs_resolve(abspath, &err);
    if (!inode) return err;

    if (S_ISDIR(inode->mode)) { inode_unref(inode); return -EISDIR; }
    if (!inode->ops || !inode->ops->truncate) {
        inode_unref(inode); return -ENOSYS;
    }

    err = inode->ops->truncate(inode, size);
    inode_unref(inode);
    return err;
}

errno_t vfs_ftruncate(process_t *proc, int fd, uint64_t size) {
    if (!proc) return -EINVAL;

    errno_t err = 0;
    file_t *f   = fd_get(proc, fd, &err);
    if (!f) return err;

    inode_t *inode = f->inode;
    if (S_ISDIR(inode->mode)) { file_unref(f); return -EISDIR; }
    if (!inode->ops || !inode->ops->truncate) { file_unref(f); return -ENOSYS; }

    err = inode->ops->truncate(inode, size);
    file_unref(f);
    return err;
}

/* ------------------------------------------------------------------ */
/* vfs_access                                                          */
/* ------------------------------------------------------------------ */

errno_t vfs_access(process_t *proc, const char *path, int amode) {
    (void)amode;   /* no permission model yet; just check existence */
    if (!path) return -EINVAL;

    char abspath[VFS_PATH_MAX];
    if (!path_to_abs(proc, path, abspath, sizeof(abspath))) return -ENAMETOOLONG;

    errno_t  err   = 0;
    inode_t *inode = vfs_resolve(abspath, &err);
    if (!inode) return err;
    inode_unref(inode);
    return EOK;
}

/* ------------------------------------------------------------------ */
/* vfs_fcntl                                                           */
/* ------------------------------------------------------------------ */

errno_t vfs_fcntl(process_t *proc, int fd, int cmd, uintptr_t arg, int *result) {
    if (!proc || !result) return -EINVAL;
    if (fd < 0 || fd >= FD_TABLE_SIZE) return -EBADF;

    switch (cmd) {
    case F_GETFD: {
        bool irq = spinlock_acquire(&proc->lock);
        if (!proc->fd_table.files[fd]) { spinlock_release(&proc->lock, irq); return -EBADF; }
        *result = proc->fd_table.cloexec[fd] ? FD_CLOEXEC : 0;
        spinlock_release(&proc->lock, irq);
        return EOK;
    }
    case F_SETFD: {
        bool irq = spinlock_acquire(&proc->lock);
        if (!proc->fd_table.files[fd]) { spinlock_release(&proc->lock, irq); return -EBADF; }
        proc->fd_table.cloexec[fd] = ((int)arg & FD_CLOEXEC) ? 1 : 0;
        *result = 0;
        spinlock_release(&proc->lock, irq);
        return EOK;
    }
    case F_GETFL: {
        errno_t err = 0;
        file_t *f   = fd_get(proc, fd, &err);
        if (!f) return err;
        *result = f->flags;
        file_unref(f);
        return EOK;
    }
    case F_SETFL: {
        errno_t err = 0;
        file_t *f   = fd_get(proc, fd, &err);
        if (!f) return err;
        /* Only O_APPEND and O_NONBLOCK are mutable */
        bool irq = spinlock_acquire(&f->lock);
        f->flags = (f->flags & ~(O_APPEND | O_NONBLOCK)) |
                   ((int)arg & (O_APPEND | O_NONBLOCK));
        spinlock_release(&f->lock, irq);
        *result = 0;
        file_unref(f);
        return EOK;
    }
    case F_DUPFD:
    case F_DUPFD_CLOEXEC: {
        int start = (arg < (uintptr_t)FD_TABLE_SIZE) ? (int)arg : FD_TABLE_SIZE;
        bool irq = spinlock_acquire(&proc->lock);
        if (!proc->fd_table.files[fd]) { spinlock_release(&proc->lock, irq); return -EBADF; }
        file_t *src = proc->fd_table.files[fd];
        int newfd = fd_alloc_from(proc, start);
        if (newfd < 0) { spinlock_release(&proc->lock, irq); return -EMFILE; }
        file_ref(src);
        proc->fd_table.files[newfd]  = src;
        proc->fd_table.cloexec[newfd] = (cmd == F_DUPFD_CLOEXEC) ? 1 : 0;
        spinlock_release(&proc->lock, irq);
        *result = newfd;
        return EOK;
    }
    default:
        return -EINVAL;
    }
}

/* ------------------------------------------------------------------ */
/* vfs_openat                                                          */
/* ------------------------------------------------------------------ */

errno_t vfs_openat(process_t *proc, int at_fd, const char *path,
                   int flags, uint32_t mode, int *fd_out) {
    if (!proc || !path || !fd_out) return -EINVAL;
    /* Absolute paths and AT_FDCWD are both handled by vfs_open via path_to_abs */
    if (path[0] == '/' || at_fd == AT_FDCWD)
        return vfs_open(proc, path, flags, mode, fd_out);
    return -ENOSYS;   /* relative + non-CWD dirfd not supported */
}
