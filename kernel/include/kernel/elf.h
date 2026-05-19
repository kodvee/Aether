#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <elf.h>

/* ELF section type constants (not in our elf.h) */
#define SHT_NULL     0
#define SHT_PROGBITS 1
#define SHT_SYMTAB   2
#define SHT_STRTAB   3
#define SHT_DYNSYM  11

/*
 * elf_image_t - in-memory snapshot of a loaded ELF kernel image.
 *
 * All pointer members point into heap copies made at load time; they remain
 * valid after clean_reclaimable_memory() reclaims the original Limine pages.
 *
 * sorted_idx is an array of symtab indices sorted ascending by st_value,
 * used exclusively for O(log n) address lookup in elf_sym_by_addr.
 */
typedef struct {
    const Elf64_Sym  *symtab;       /* heap copy of .symtab */
    uint32_t          sym_count;    /* number of entries in symtab */
    const char       *strtab;       /* heap copy of symbol strtab */
    size_t            strtab_size;
    const Elf64_Shdr *shdrs;        /* heap copy of section header table */
    uint16_t          shdr_count;
    const char       *shstrtab;     /* heap copy of section-name strtab */
    uint32_t         *sorted_idx;   /* heap array: symtab indices sorted by addr */
    uint32_t          sorted_count; /* entries in sorted_idx (defined, non-zero addr) */
    bool              loaded;
} elf_image_t;

/*
 * Global kernel ELF image. Populated by elf_init() during early boot.
 * Read-only after elf_init() returns; must not be written by any caller.
 *
 * ELF invariants:
 *   - elf_image_load must not be called before slab_init.
 *   - elf_image_load must be called before clean_reclaimable_memory.
 *   - All elf_* query functions (elf_sym_by_addr, elf_sym_by_name, etc.)
 *     are safe when img->loaded == false — they return NULL/false immediately.
 *   - elf_sym_by_addr and elf_sym_by_name make no allocations and are safe
 *     in interrupt and panic context.
 *   - The kernel must be compiled with -fno-omit-frame-pointer for stack
 *     traces to be meaningful.
 */
extern const elf_image_t kelf;

/*
 * elf_init - load the kernel's own ELF image into kelf.
 * Requires the slab allocator to be ready. Marked __init; must be called
 * before clean_reclaimable_memory() reclaims Limine pages.
 */
void elf_init(void);

/*
 * elf_image_free - release all heap allocations made by elf_image_load.
 * Sets img->loaded = false. Safe to call on an already-freed or
 * never-loaded image. Do not call on &kelf (kernel image is permanent).
 */
void elf_image_free(elf_image_t *img);

/* Returns the Limine-provided kernel cmdline, or NULL if unavailable. */
const char *kernel_cmdline(void);

/* ---------- primary API ---------- */

/*
 * elf_image_load - parse elf_data and populate img.
 * Allocates heap copies of all needed sections; safe after reclaim.
 * Returns true on success. img->loaded is set accordingly.
 */
bool elf_image_load(elf_image_t *img, const void *elf_data);

/*
 * elf_sym_by_addr - find the best-matching symbol for a virtual address.
 *
 * "Best matching" means the symbol whose st_value is the largest value
 * that is still <= addr.  Prefers symbols that contain addr (st_size > 0
 * and addr < st_value + st_size) over mere "closest lower" matches.
 *
 * If name_out is non-NULL, *name_out is set to the symbol's name string
 * (points into img->strtab - no allocation).
 *
 * Returns NULL if img is not loaded or no symbol is at or below addr.
 * No dynamic allocation; safe in panic context.
 */
const Elf64_Sym *elf_sym_by_addr(const elf_image_t *img, uintptr_t addr,
                                  const char **name_out);

/*
 * elf_sym_by_name - linear scan for exact name match.
 * Returns NULL if not found.
 * No dynamic allocation; safe in panic context.
 */
const Elf64_Sym *elf_sym_by_name(const elf_image_t *img, const char *name);

/*
 * elf_sym_section - return the section header that contains sym.
 * Returns NULL if the symbol's st_shndx is undefined, absolute, or
 * out of range.
 */
const Elf64_Shdr *elf_sym_section(const elf_image_t *img, const Elf64_Sym *sym);

/* ---------- inline helpers ---------- */

static inline const char *elf_sym_name(const elf_image_t *img,
                                        const Elf64_Sym *sym) {
    return img->strtab + sym->st_name;
}

static inline const char *elf_section_name(const elf_image_t *img,
                                             const Elf64_Shdr *s) {
    return img->shstrtab + s->sh_name;
}

static inline bool elf_sym_is_func(const Elf64_Sym *sym) {
    return ELF64_ST_TYPE(sym->st_info) == STT_FUNC;
}

static inline bool elf_sym_is_object(const Elf64_Sym *sym) {
    return ELF64_ST_TYPE(sym->st_info) == STT_OBJECT;
}

static inline bool elf_sym_defined(const Elf64_Sym *sym) {
    return sym->st_shndx != SHN_UNDEF;
}

/* True if addr falls within the symbol's declared extent */
static inline bool elf_sym_contains(const Elf64_Sym *sym, uintptr_t addr) {
    return sym->st_size > 0
        && addr >= sym->st_value
        && addr <  sym->st_value + sym->st_size;
}

static inline uint64_t elf_sym_offset(const Elf64_Sym *sym, uintptr_t addr) {
    return addr - sym->st_value;
}
