/**
 * elf.c — ELF64 parsing and symbol resolution subsystem.
 *
 * Parses the kernel's own ELF image (supplied by Limine via the kernel-file
 * request) at boot time, copies all symbol/section data to the heap so it
 * survives clean_reclaimable_memory(), and provides O(log n) address-to-symbol
 * and O(n) name-to-symbol lookup.
 *
 * Do NOT call elf_init / elf_image_load before the slab allocator is ready.
 */

#include <kernel/elf.h>
#include <kernel/kprintf.h>
#include <kernel/mmu.h>      /* malloc */
#include <kernel/macros.h>
#include <limine.h>
#include <string.h>
#include <memory.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <elf.h>

/* Limine kernel-file request — filled before _start is called */
__attribute__((used, section(".requests")))
static volatile struct limine_kernel_file_request kfile_request = {
    .id = LIMINE_KERNEL_FILE_REQUEST,
    .revision = 0,
};

/* Global kernel ELF image populated by elf_image_load */
elf_image_t kelf = { .loaded = false };

/* ------------------------------------------------------------------ */
/* Internal helpers                                                     */
/* ------------------------------------------------------------------ */

static bool elf_validate(const Elf64_Ehdr *hdr) {
    if (hdr->e_ident[0] != ELFMAG0 ||
        hdr->e_ident[1] != ELFMAG1 ||
        hdr->e_ident[2] != ELFMAG2 ||
        hdr->e_ident[3] != ELFMAG3) {
        kprintf("elf: bad magic\n");
        return false;
    }
    if (hdr->e_ident[4] != ELFCLASS64) {
        kprintf("elf: not a 64-bit ELF\n");
        return false;
    }
    if (hdr->e_ident[5] != ELFDATA2LSB) {
        kprintf("elf: not little-endian\n");
        return false;
    }
    if (hdr->e_machine != EM_X86_64) {
        kprintf("elf: not x86-64\n");
        return false;
    }
    return true;
}

/*
 * Build sorted_idx: indices into symtab, sorted ascending by st_value,
 * restricted to defined symbols with a non-zero address.  Uses insertion
 * sort — acceptable because kernel symbol counts are typically < 10 000.
 */
static void build_sorted_index(elf_image_t *img) {
    /* Count qualifying symbols */
    uint32_t count = 0;
    for (uint32_t i = 0; i < img->sym_count; i++) {
        const Elf64_Sym *s = &img->symtab[i];
        if (s->st_shndx != SHN_UNDEF && s->st_value != 0)
            count++;
    }

    img->sorted_idx   = malloc(sizeof(uint32_t) * count);
    img->sorted_count = 0;

    if (!img->sorted_idx) {
        kprintf("elf: out of memory for sorted index\n");
        return;
    }

    /* Populate */
    for (uint32_t i = 0; i < img->sym_count; i++) {
        const Elf64_Sym *s = &img->symtab[i];
        if (s->st_shndx == SHN_UNDEF || s->st_value == 0) continue;
        img->sorted_idx[img->sorted_count++] = i;
    }

    /* Insertion sort by st_value */
    for (uint32_t i = 1; i < img->sorted_count; i++) {
        uint32_t key = img->sorted_idx[i];
        uintptr_t key_val = img->symtab[key].st_value;
        int32_t j = (int32_t)i - 1;
        while (j >= 0 && img->symtab[img->sorted_idx[j]].st_value > key_val) {
            img->sorted_idx[j + 1] = img->sorted_idx[j];
            j--;
        }
        img->sorted_idx[j + 1] = key;
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

bool elf_image_load(elf_image_t *img, const void *elf_data) {
    img->loaded = false;

    const Elf64_Ehdr *hdr = (const Elf64_Ehdr *)elf_data;
    if (!elf_validate(hdr)) return false;

    if (hdr->e_shentsize < sizeof(Elf64_Shdr)) {
        kprintf("elf: section header entry too small (%u)\n", hdr->e_shentsize);
        return false;
    }

    /* Section header table in the original (Limine) mapping */
    const Elf64_Shdr *raw_shdrs =
        (const Elf64_Shdr *)((uintptr_t)elf_data + hdr->e_shoff);
    uint16_t shnum = hdr->e_shnum;

    /*
     * ELF extended section count: if e_shnum == 0 the real count is in
     * section 0's sh_size field.
     */
    if (shnum == 0) {
        if (hdr->e_shoff == 0) {
            kprintf("elf: no section headers\n");
            return false;
        }
        shnum = (uint16_t)raw_shdrs[0].sh_size;
    }

    /* Resolve shstrndx (handles SHN_XINDEX) */
    uint32_t shstrndx = hdr->e_shstrndx;
    if (shstrndx == SHN_XINDEX)
        shstrndx = raw_shdrs[0].sh_link;

    if (shstrndx >= shnum) {
        kprintf("elf: shstrndx %u out of range (%u sections)\n", shstrndx, shnum);
        return false;
    }

    /* Copy the section header table to the heap */
    size_t shdrs_bytes = (size_t)shnum * sizeof(Elf64_Shdr);
    Elf64_Shdr *shdrs_copy = malloc(shdrs_bytes);
    if (!shdrs_copy) {
        kprintf("elf: out of memory for shdrs\n");
        return false;
    }
    memcpy(shdrs_copy, raw_shdrs, shdrs_bytes);

    /* Copy shstrtab */
    const Elf64_Shdr *shstr_shdr = &shdrs_copy[shstrndx];
    char *shstrtab_copy = malloc(shstr_shdr->sh_size);
    if (!shstrtab_copy) {
        kprintf("elf: out of memory for shstrtab\n");
        free(shdrs_copy);
        return false;
    }
    memcpy(shstrtab_copy,
           (const void *)((uintptr_t)elf_data + shstr_shdr->sh_offset),
           shstr_shdr->sh_size);

    /* Find SHT_SYMTAB section (search by type, not name) */
    const Elf64_Shdr *symtab_shdr  = NULL;
    const Elf64_Shdr *strtab_shdr  = NULL;

    for (uint32_t i = 0; i < shnum; i++) {
        if (shdrs_copy[i].sh_type == SHT_SYMTAB) {
            symtab_shdr = &shdrs_copy[i];
            /* sh_link points to the associated string table section */
            uint32_t link = symtab_shdr->sh_link;
            if (link < shnum && shdrs_copy[link].sh_type == SHT_STRTAB)
                strtab_shdr = &shdrs_copy[link];
            break;
        }
    }

    if (!symtab_shdr) {
        kprintf("elf: no SHT_SYMTAB section found\n");
        free(shstrtab_copy);
        free(shdrs_copy);
        return false;
    }
    if (!strtab_shdr) {
        kprintf("elf: symtab sh_link does not point to a valid strtab\n");
        free(shstrtab_copy);
        free(shdrs_copy);
        return false;
    }

    /* Copy symtab */
    uint32_t sym_count = (uint32_t)(symtab_shdr->sh_size / sizeof(Elf64_Sym));
    Elf64_Sym *symtab_copy = malloc(symtab_shdr->sh_size);
    if (!symtab_copy) {
        kprintf("elf: out of memory for symtab\n");
        free(shstrtab_copy);
        free(shdrs_copy);
        return false;
    }
    memcpy(symtab_copy,
           (const void *)((uintptr_t)elf_data + symtab_shdr->sh_offset),
           symtab_shdr->sh_size);

    /* Copy strtab */
    char *strtab_copy = malloc(strtab_shdr->sh_size);
    if (!strtab_copy) {
        kprintf("elf: out of memory for strtab\n");
        free(symtab_copy);
        free(shstrtab_copy);
        free(shdrs_copy);
        return false;
    }
    memcpy(strtab_copy,
           (const void *)((uintptr_t)elf_data + strtab_shdr->sh_offset),
           strtab_shdr->sh_size);

    /* Populate the image struct */
    img->symtab       = symtab_copy;
    img->sym_count    = sym_count;
    img->strtab       = strtab_copy;
    img->strtab_size  = strtab_shdr->sh_size;
    img->shdrs        = shdrs_copy;
    img->shdr_count   = shnum;
    img->shstrtab     = shstrtab_copy;
    img->sorted_idx   = NULL;
    img->sorted_count = 0;
    img->loaded       = true;

    build_sorted_index(img);

    kprintf("elf: loaded %u symbols, %u sections, %u indexed\n",
            sym_count, shnum, img->sorted_count);
    return true;
}

/*
 * Binary search for the symbol whose st_value is the largest value <= addr.
 * Among equally-close candidates, prefer one that strictly contains addr
 * (st_size > 0 and addr < st_value + st_size).
 */
const Elf64_Sym *elf_sym_by_addr(const elf_image_t *img, uintptr_t addr,
                                  const char **name_out) {
    if (!img->loaded || img->sorted_count == 0) return NULL;

    uint32_t lo = 0, hi = img->sorted_count;
    while (lo + 1 < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (img->symtab[img->sorted_idx[mid]].st_value <= addr)
            lo = mid;
        else
            hi = mid;
    }

    /* lo is the rightmost entry with st_value <= addr */
    if (img->symtab[img->sorted_idx[lo]].st_value > addr) return NULL;

    const Elf64_Sym *best = &img->symtab[img->sorted_idx[lo]];

    /*
     * Walk backwards to handle duplicate st_value entries and find any
     * symbol that strictly contains addr (has a declared size).
     */
    for (int32_t i = (int32_t)lo; i >= 0; i--) {
        const Elf64_Sym *s = &img->symtab[img->sorted_idx[i]];
        if (s->st_value > addr) continue;
        if (s->st_value < best->st_value) break; /* went past a closer symbol */
        if (elf_sym_contains(s, addr)) {
            best = s;
            break;
        }
    }

    if (name_out) *name_out = img->strtab + best->st_name;
    return best;
}

const Elf64_Sym *elf_sym_by_name(const elf_image_t *img, const char *name) {
    if (!img->loaded) return NULL;
    for (uint32_t i = 0; i < img->sym_count; i++) {
        const char *sname = img->strtab + img->symtab[i].st_name;
        if (strcmp(sname, name) == 0)
            return &img->symtab[i];
    }
    return NULL;
}

const Elf64_Shdr *elf_sym_section(const elf_image_t *img,
                                   const Elf64_Sym *sym) {
    if (!img->loaded) return NULL;
    uint32_t idx = sym->st_shndx;
    /* SHN_XINDEX: real index is in symtab[0].st_value — not supported here */
    if (idx == SHN_UNDEF || idx >= SHN_LORESERVE) return NULL;
    if (idx >= img->shdr_count) return NULL;
    return &img->shdrs[idx];
}

/* Parse and copy the kernel ELF image into kelf. Must be called after slab_init. */
void __init elf_init(void) {
    if (kfile_request.response == NULL ||
        kfile_request.response->kernel_file == NULL) {
        kprintf("elf: kernel file not provided by bootloader\n");
        return;
    }
    elf_image_load(&kelf, kfile_request.response->kernel_file->address);
}
