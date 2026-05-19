#pragma once

#include <stdint.h>
#include <stddef.h>
#include <elf.h>

/*
 * Elf64_Phdr - 64-bit program header (defined here; not relied on from libc elf.h).
 *
 * NOTE: in the 64-bit layout, p_flags immediately follows p_type -- unlike
 * the 32-bit struct where p_flags comes last.
 */
typedef struct {
    Elf64_Word  p_type;     /* segment type                */
    Elf64_Word  p_flags;    /* segment permissions         */
    Elf64_Off   p_offset;   /* byte offset into the file   */
    Elf64_Addr  p_vaddr;    /* virtual address in memory   */
    Elf64_Addr  p_paddr;    /* physical address (ignored)  */
    Elf64_Xword p_filesz;   /* bytes in the file image     */
    Elf64_Xword p_memsz;    /* bytes in the memory image   */
    Elf64_Xword p_align;    /* alignment (power of two)    */
} Elf64_Phdr;

/* p_type values */
#define PT_NULL    0u
#define PT_LOAD    1u
#define PT_DYNAMIC 2u
#define PT_INTERP  3u
#define PT_PHDR    6u

/* p_flags permission bits */
#define PF_X  0x1u   /* execute */
#define PF_W  0x2u   /* write   */
#define PF_R  0x4u   /* read    */

/* Forward declaration so the header is self-contained */
struct thread;

/*
 * elf_load_user - load a static ELF64 executable into a fresh user process.
 *
 * Validates the header, maps all PT_LOAD segments into a new address space,
 * allocates a user stack, and creates a user thread (THREAD_CREATED state).
 *
 * The caller must call thread_ready_on() or thread_ready() to schedule it.
 * Returns NULL on any error; all resources are freed on failure.
 */
struct thread *elf_load_user(const void *elf_data, size_t elf_size,
                              const char *name);

/*
 * init_spawn - load the embedded init binary and enqueue it on core 0.
 * Called once from the BSP's first kernel thread after the scheduler is live.
 * Prints a notice and returns silently if no binary is embedded.
 */
void init_spawn(void);
