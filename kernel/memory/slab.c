/*
 * slab.c - kernel slab allocator
 *
 * Small allocations (≤ SLAB_MAX_SIZE) come from per-size-class caches.
 * Each cache manages a linked list of 4 KiB pages; objects within a page
 * form an embedded free-list.  When a page is exhausted a new one is
 * allocated from the PMM and prepended to the cache's list.
 *
 * Large allocations (> SLAB_MAX_SIZE) are served directly from the PMM;
 * a small header at the start of the first page stores the allocation
 * metadata so free() can release the right number of frames.
 *
 * free() identifies the allocation type by a magic value stored at the
 * start of every managed page.
 */

#include <kernel/mmu.h>
#include <kernel/spinlock.h>
#include <kernel/macros.h>
#include <kernel/panic.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* -- Magic numbers ------------------------------------------------------ */

#define SLAB_MAGIC  ((uint32_t)0x514B3A00)
#define LARGE_MAGIC ((uint32_t)0xB16A7E00)

/* -- Per-page header for slab pages ------------------------------------ */

struct slab_page {
    uint32_t           magic;     /* SLAB_MAGIC                       */
    uint16_t           inuse;     /* objects currently allocated       */
    uint16_t           capacity;  /* total objects this page can hold  */
    struct slab_cache *cache;     /* owning cache                      */
    struct slab_page  *next;      /* next page in cache list           */
    void             **freelist;  /* head of embedded free-object list */
};
/* sizeof == 4+2+2+8+8+8 = 32 bytes; objects start at offset 32 */

/* -- Per-size-class cache ----------------------------------------------- */

struct slab_cache {
    spinlock_t        lock;
    struct slab_page *pages;     /* list of pages, newest at head     */
    size_t            obj_size;
};

/* -- Header for large (non-slab) allocations --------------------------- */

struct large_hdr {
    uint32_t magic;   /* LARGE_MAGIC                                   */
    uint32_t _pad;
    size_t   pages;   /* number of physical frames backing this alloc  */
    size_t   size;    /* original requested size                        */
};
/* sizeof == 4+4+8+8 = 24 bytes; user data starts at offset 24 */

/* -- Size classes ------------------------------------------------------- */

#define SLAB_MAX_SIZE 2048u

static struct slab_cache caches[] = {
    { SPINLOCK_ZERO, NULL,    8 },
    { SPINLOCK_ZERO, NULL,   16 },
    { SPINLOCK_ZERO, NULL,   32 },
    { SPINLOCK_ZERO, NULL,   64 },
    { SPINLOCK_ZERO, NULL,  128 },
    { SPINLOCK_ZERO, NULL,  256 },
    { SPINLOCK_ZERO, NULL,  512 },
    { SPINLOCK_ZERO, NULL, 1024 },
    { SPINLOCK_ZERO, NULL, 2048 },
};

#define NCACHES (sizeof(caches) / sizeof(caches[0]))

static struct slab_cache *cache_for(size_t size) {
    for (size_t i = 0; i < NCACHES; i++)
        if (caches[i].obj_size >= size)
            return &caches[i];
    return NULL;
}

/* -- Slab page management ----------------------------------------------- */

static struct slab_page *slab_new_page(struct slab_cache *cache) {
    KERNEL_ASSERT(spinlock_is_held(&cache->lock));
    uintptr_t frame = mmu_request_frame();
    struct slab_page *pg = (struct slab_page *)(frame + HHDM_HIGHER_HALF);

    size_t hdr      = sizeof(struct slab_page);           /* 32 */
    size_t avail    = PAGE_SIZE - hdr;
    size_t cap      = avail / cache->obj_size;

    pg->magic    = SLAB_MAGIC;
    pg->inuse    = 0;
    pg->capacity = (uint16_t)cap;
    pg->cache    = cache;
    pg->next     = cache->pages;
    cache->pages = pg;

    /* Build the embedded free-list: each free object's first word points
     * to the next free object; the last points to NULL.               */
    uintptr_t base = (uintptr_t)pg + hdr;
    pg->freelist   = (void **)base;

    for (size_t i = 0; i < cap - 1; i++) {
        void **cur  = (void **)(base + i * cache->obj_size);
        void **next = (void **)(base + (i + 1) * cache->obj_size);
        *cur = next;
    }
    *(void **)(base + (cap - 1) * cache->obj_size) = NULL;

    return pg;
}

/* -- Slab alloc / free -------------------------------------------------- */

static void *slab_alloc(struct slab_cache *cache) {
    bool s = spinlock_acquire(&cache->lock);

    /* Find a page with free slots; pages list is short (usually 1). */
    struct slab_page *pg = cache->pages;
    while (pg && !pg->freelist)
        pg = pg->next;

    if (!pg)
        pg = slab_new_page(cache);

    void **obj   = pg->freelist;
    pg->freelist = *obj;
    pg->inuse++;

    spinlock_release(&cache->lock, s);

    __builtin_memset(obj, 0, cache->obj_size);
    return obj;
}

static void slab_do_free(struct slab_page *pg, void *addr) {
    struct slab_cache *cache = pg->cache;
    bool s = spinlock_acquire(&cache->lock);

    void **obj   = addr;
    *obj         = pg->freelist;
    pg->freelist = obj;
    pg->inuse--;

    spinlock_release(&cache->lock, s);
}

/* -- Large allocation helpers ------------------------------------------- */

static void *large_alloc(size_t size) {
    size_t page_count = (sizeof(struct large_hdr) + size + PAGE_SIZE - 1) / PAGE_SIZE;
    uintptr_t phys    = mmu_request_frames((uint64_t)page_count);
    struct large_hdr *hdr = (struct large_hdr *)(phys + HHDM_HIGHER_HALF);

    hdr->magic = LARGE_MAGIC;
    hdr->_pad  = 0;
    hdr->pages = page_count;
    hdr->size  = size;

    void *data = hdr + 1;
    __builtin_memset(data, 0, size);
    return data;
}

/* -- Public allocator interface ----------------------------------------- */

void *malloc(size_t size) {
    if (size == 0) size = 1;

    struct slab_cache *cache = cache_for(size);
    if (cache)
        return slab_alloc(cache);

    return large_alloc(size);
}

void free(void *addr) {
    if (!addr) return;

    uintptr_t page_base = (uintptr_t)addr & ~(uintptr_t)(PAGE_SIZE - 1);
    uint32_t  magic     = *(uint32_t *)page_base;

    if (magic == SLAB_MAGIC) {
        slab_do_free((struct slab_page *)page_base, addr);
        return;
    }

    if (magic == LARGE_MAGIC) {
        struct large_hdr *hdr = (struct large_hdr *)page_base;
        mmu_free_frames((void *)(page_base - HHDM_HIGHER_HALF), (uint64_t)hdr->pages);
        return;
    }

    SUBSYS_PANIC("slab", "free: corrupt page magic - double-free or bad pointer");
}

void *realloc(void *addr, size_t new_size) {
    if (!addr)     return malloc(new_size);
    if (!new_size) { free(addr); return NULL; }

    uintptr_t page_base = (uintptr_t)addr & ~(uintptr_t)(PAGE_SIZE - 1);
    uint32_t  magic     = *(uint32_t *)page_base;

    if (magic == SLAB_MAGIC) {
        struct slab_page *pg = (struct slab_page *)page_base;
        /* Still fits in the same slab slot - nothing to do. */
        if (new_size <= pg->cache->obj_size)
            return addr;
        /* Needs a larger slot or a large alloc. */
        void *newp = malloc(new_size);
        if (!newp) return NULL;
        __builtin_memcpy(newp, addr, pg->cache->obj_size);
        slab_do_free(pg, addr);
        return newp;
    }

    if (magic == LARGE_MAGIC) {
        struct large_hdr *hdr = (struct large_hdr *)page_base;
        size_t new_pages =
            (sizeof(struct large_hdr) + new_size + PAGE_SIZE - 1) / PAGE_SIZE;
        /* Same page count - update the size field in-place. */
        if (new_pages == hdr->pages) {
            hdr->size = new_size;
            return addr;
        }
        void *newp = malloc(new_size);
        if (!newp) return NULL;
        size_t copy = hdr->size < new_size ? hdr->size : new_size;
        __builtin_memcpy(newp, addr, copy);
        mmu_free_frames((void *)(page_base - HHDM_HIGHER_HALF),
                        (uint64_t)hdr->pages);
        return newp;
    }

    SUBSYS_PANIC("slab", "realloc: corrupt page magic - double-free or bad pointer");
}

/* -- Initialise all caches ---------------------------------------------- */

void __init slab_init(void) {
    /*
     * Pre-warm each cache with one page so the very first malloc in each
     * class doesn't race with page allocation in hot paths.
     */
    for (size_t i = 0; i < NCACHES; i++) {
        bool s = spinlock_acquire(&caches[i].lock);
        slab_new_page(&caches[i]);
        spinlock_release(&caches[i].lock, s);
        KINFO("slab", "cache[%zu] obj_size=%zu cap=%u",
              i, caches[i].obj_size, caches[i].pages->capacity);
    }
}
