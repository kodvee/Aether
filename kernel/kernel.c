#include <stdint.h>
#include <stddef.h>
#include <limine.h>

#include <kernel/kprintf.h>
#include <kernel/version.h>
#include <kernel/cpu.h>
#include <kernel/ports.h>
#include <kernel/cpufeature.h>
#include <kernel/mmu.h>
#include <kernel/int.h>
#include <kernel/elf.h>
#include <kernel/gdt.h>
#include <kernel/smp.h>
#include <kernel/acpi.h>
#include <kernel/hpet.h>
#include <kernel/apic.h>
#include <kernel/panic.h>
#include <kernel/ktest.h>
#include <stdbool.h>

__attribute__((used, section(".requests")))
static volatile LIMINE_BASE_REVISION(2);

__attribute__((used, section(".requests_start_marker")))
static volatile LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".requests_end_marker")))
static volatile LIMINE_REQUESTS_END_MARKER;

/* Initial core */
static core_t* core_bsp = NULL;

/*
 * _start - kernel entry point, called by the Limine bootloader on the BSP.
 *
 * Initializes all subsystems in dependency order: PMM → VMM → GDT → IDT →
 * slab → printf → ACPI → ELF → HPET → SMP. In test builds, runs the ktest
 * suite before halting.
 */
void _start(void) {
	/* Initialize physical memory manager */
    pmm_init();

	/* Initialize virtual memory manager */
	vmm_init();

	/* Initialize gdt */
	gdt_init();

	/* Setup isrs */
    idt_init();

	/* Initialize the slab allocator */
	slab_init();

	core_bsp = malloc(sizeof(*core_bsp));
	core_bsp->bsp = true;
	core_bsp->lapic_id = 0;
	set_gs_register(core_bsp);

	/* Initialize printf */
	printf_init();

	/* Print kernel info */
	kprintf("%s %d.%d.%d-%s running on %s\n",
        __kernel_name,
        __kernel_version_major,
        __kernel_version_minor,
        __kernel_version_lower,
        __kernel_version_suffix,
        __kernel_arch);
	kprintf("Kernel compiled by \"%s\" on \"%s %s\"\n",
		__kernel_compiler_version,
		__kernel_build_date,
		__kernel_build_time);
	
	/* Get available features of CPU */
	cpu_feature_init();

	/* Initialize ACPI */
	acpi_init();

	/* Parse kernel ELF image and build symbol index */
	elf_init();

	/* Load the CPU information */
	cpuinfo_init();

	/* Mask all 8259 PIC IRQ lines before the IOAPIC takes over.
	 * Without this, any PIC-sourced interrupt fires on an unmapped vector
	 * after IDT init and causes a spurious fault. */
	outportb(0xA1, 0xff);
	outportb(0x21, 0xff);

	/* Initialize the HPET timer */
	hpet_init();

	/* Initialize multicore */
	smp_init();

#ifdef KTEST_ENABLED
	/* Run the kernel test suite (discovers tests from .ktest linker section) */
	ktest_run();
#endif

	/* All done */
	asm volatile ("1: hlt; jmp 1b" ::: "memory");
}