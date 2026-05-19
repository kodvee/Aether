/*
 * embed.S - embed the init binary directly into the kernel image.
 *
 * The .incbin directive includes the raw bytes of init/init.elf at link time.
 * If the file is empty (placeholder build), __init_elf_start == __init_elf_end
 * and init_spawn() will print a notice and return without loading anything.
 *
 * To replace the placeholder with a real binary:
 *   cp /path/to/init.elf kernel/init/init.elf
 *   make
 */

.section .rodata
.align 4096

.global __init_elf_start
.global __init_elf_end

__init_elf_start:
    .incbin "init/init.elf"
__init_elf_end:
