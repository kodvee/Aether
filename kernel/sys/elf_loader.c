/*
 * elf_loader.c - ELF64 user-process loader.
 *
 * Loads a static ELF64 executable from a byte blob (the embedded init image
 * or any future kernel-provided binary) into a fresh process address space:
 *
 *   1. Parse and validate ELF header.
 *   2. For each PT_LOAD segment: call process_mmap (which zeroes the pages),
 *      then copy the file image in via the HHDM (the user pagemap is NOT
 *      loaded into CR3 during the copy -- we walk it with mmu_virt_to_phys).
 *   3. Set proc->brk to the page-ceiling of the highest loaded address.
 *   4. Allocate user stack; carve back 64 bytes (already zeroed) for the
 *      initial ABI stack frame: argc=0, argv/envp/auxv terminators = NULL.
 *   5. Create a user thread via thread_create_user(); caller schedules it.
 */

#include <kernel/elf_loader.h>
#include <kernel/scheduler.h>
#include <kernel/vmm.h>
#include <kernel/pmm.h>
#include <kernel/mmu.h>
#include <kernel/kprintf.h>
#include <kernel/panic.h>
#include <stdint.h>
#include <stddef.h>

#define PAGE_ALIGN_UP(x)   (((uintptr_t)(x) + PAGE_SIZE - 1) & ~(uintptr_t)(PAGE_SIZE - 1))
#define PAGE_ALIGN_DOWN(x) ((uintptr_t)(x) & ~(uintptr_t)(PAGE_SIZE - 1))

/* Provided by kernel/init/embed.S */
extern uint8_t __init_elf_start[];
extern uint8_t __init_elf_end[];

/* ------------------------------------------------------------------ */
/* Internal helpers                                                     */
/* ------------------------------------------------------------------ */

static bool validate_header(const Elf64_Ehdr *hdr, size_t size) {
    if (size < sizeof(Elf64_Ehdr)) {
        KERROR("elf_loader", "blob too small");
        return false;
    }
    if (hdr->e_ident[EI_MAG0] != ELFMAG0 || hdr->e_ident[EI_MAG1] != ELFMAG1 ||
        hdr->e_ident[EI_MAG2] != ELFMAG2 || hdr->e_ident[EI_MAG3] != ELFMAG3) {
        KERROR("elf_loader", "bad ELF magic");
        return false;
    }
    if (hdr->e_ident[EI_CLASS] != ELFCLASS64) {
        KERROR("elf_loader", "not ELF64");
        return false;
    }
    if (hdr->e_ident[EI_DATA] != ELFDATA2LSB) {
        KERROR("elf_loader", "not little-endian");
        return false;
    }
    if (hdr->e_machine != EM_X86_64) {
        KERROR("elf_loader", "not x86-64");
        return false;
    }
    if (hdr->e_type != ET_EXEC) {
        KERROR("elf_loader", "not a static executable (ET_EXEC)");
        return false;
    }
    if (!hdr->e_phnum || hdr->e_phentsize < sizeof(Elf64_Phdr)) {
        KERROR("elf_loader", "no usable program headers");
        return false;
    }
    if (hdr->e_phoff + (uint64_t)hdr->e_phnum * hdr->e_phentsize > (uint64_t)size) {
        KERROR("elf_loader", "program header table out of bounds");
        return false;
    }
    return true;
}

/*
 * copy_segment - copy [src, src+filesz) into user virtual memory at vaddr.
 *
 * The user pagemap is not the current CR3.  process_ensure_page allocates
 * a frame on demand and returns the physical address; we write via the HHDM.
 */
static void copy_segment(process_t *proc, uintptr_t vaddr,
                          const uint8_t *src, size_t filesz) {
    size_t remaining = filesz;
    while (remaining > 0) {
        uintptr_t page_base = PAGE_ALIGN_DOWN(vaddr);
        uintptr_t page_off  = vaddr - page_base;
        uintptr_t phys      = process_ensure_page(proc, page_base);
        KERNEL_ASSERT(phys != 0);

        uint8_t *dst  = (uint8_t *)(phys + HHDM_HIGHER_HALF) + page_off;
        size_t chunk  = PAGE_SIZE - page_off;
        if (chunk > remaining) chunk = remaining;

        __builtin_memcpy(dst, src, chunk);

        src       += chunk;
        vaddr     += chunk;
        remaining -= chunk;
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

struct thread *elf_load_user(const void *elf_data, size_t elf_size,
                              const char *name) {
    if (!elf_data || !elf_size) return NULL;

    const Elf64_Ehdr *hdr = (const Elf64_Ehdr *)elf_data;
    if (!validate_header(hdr, elf_size)) return NULL;

    process_t *proc = process_create(name);
    if (!proc) {
        KERROR("elf_loader", "process_create failed");
        return NULL;
    }

    const Elf64_Phdr *phdrs = (const Elf64_Phdr *)
                               ((uintptr_t)elf_data + hdr->e_phoff);
    uintptr_t max_vaddr = 0;

    for (uint16_t i = 0; i < hdr->e_phnum; i++) {
        const Elf64_Phdr *ph = &phdrs[i];
        if (ph->p_type != PT_LOAD || !ph->p_memsz) continue;

        /* Translate ELF PF_* -> kernel PROT_* (bit values differ) */
        uint32_t prot = 0;
        if (ph->p_flags & PF_R) prot |= PROT_READ;
        if (ph->p_flags & PF_W) prot |= PROT_WRITE;
        if (ph->p_flags & PF_X) prot |= PROT_EXEC;

        uintptr_t seg_base = PAGE_ALIGN_DOWN(ph->p_vaddr);
        uintptr_t seg_end  = PAGE_ALIGN_UP(ph->p_vaddr + ph->p_memsz);

        uintptr_t mapped = process_mmap(proc, seg_base, seg_end - seg_base, prot);
        if (!mapped) {
            KERROR("elf_loader", "failed to map segment %u at %#lx", i, seg_base);
            process_destroy(proc);
            return NULL;
        }

        /* Copy file image; BSS tail is already zeroed by process_mmap */
        if (ph->p_filesz > 0) {
            if (ph->p_offset + ph->p_filesz > (uint64_t)elf_size) {
                KERROR("elf_loader", "segment %u file data exceeds blob", i);
                process_destroy(proc);
                return NULL;
            }
            const uint8_t *src = (const uint8_t *)elf_data + ph->p_offset;
            copy_segment(proc, ph->p_vaddr, src, (size_t)ph->p_filesz);
        }

        uintptr_t seg_top = ph->p_vaddr + ph->p_memsz;
        if (seg_top > max_vaddr) max_vaddr = seg_top;
    }

    /* Program break starts just above the last loaded segment */
    proc->brk = PAGE_ALIGN_UP(max_vaddr);

    /*
     * User stack.  process_alloc_ustack returns the TOP of the mapped region
     * (= initial RSP for a fully empty stack).  We subtract 64 bytes to
     * provide an ABI-compliant initial frame; all bytes are pre-zeroed:
     *
     *   [rsp + 0]   argc  = 0
     *   [rsp + 8]   argv[0] = NULL  (terminator)
     *   [rsp + 16]  envp[0] = NULL  (terminator)
     *   [rsp + 24]  auxv.a_type = AT_NULL = 0
     *   [rsp + 32]  auxv.a_val  = 0
     *   [rsp + 40..56] padding
     */
    uintptr_t usp = process_alloc_ustack(proc) - 64;

    thread_t *t = thread_create_user(proc, hdr->e_entry, usp);
    if (!t) {
        KERROR("elf_loader", "thread_create_user failed");
        process_destroy(proc);
        return NULL;
    }

    KINFO("elf_loader", "loaded '%s': entry=%#lx usp=%#lx brk=%#lx pid=%d",
          name, (uintptr_t)hdr->e_entry, usp, proc->brk, proc->pid);
    return t;
}

void init_spawn(void) {
    size_t size = (size_t)(__init_elf_end - __init_elf_start);
    if (!size) {
        kprintf("init: no embedded binary -- placeholder build\n");
        return;
    }

    thread_t *t = elf_load_user(__init_elf_start, size, "init");
    if (!t) {
        KERROR("elf_loader", "init load failed");
        return;
    }

    thread_ready_on(t, 0);
    KINFO("elf_loader", "init (PID %d, tid %d) enqueued on core 0",
          t->parent->pid, t->tid);
}
