#include <stdint.h>
#include <stddef.h>
#include <limine.h>

#include <kernel/kprintf.h>
#include <kernel/version.h>
#include <kernel/cpu.h>
#include <kernel/ports.h>
#include <kernel/cpufeature.h>
#include <kernel/mmu.h>
#include <stdbool.h>
#include <kernel/hpet.h>
#include <kernel/apic.h>

extern void debug_printf_init(void);
extern void gdt_init(void);
extern void pmm_init(void);
extern void vmm_init(void);
extern void idt_init(void);
extern void elf_init(void);
extern void printf_init(void);
extern void smp_init(void);
extern void cpuinfo_init(void);
extern void acpi_init(void);
extern void hpet_init(void);
extern void cpu_feature_init(void);

__attribute__((used, section(".requests")))
static volatile LIMINE_BASE_REVISION(2);

__attribute__((used, section(".requests_start_marker")))
static volatile LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".requests_end_marker")))
static volatile LIMINE_REQUESTS_END_MARKER;

void kinit_func(void) {
	kprintf("Reclaimed a total of %lu bytes\n", clean_reclaimable_memory());
	for(;;) {
		kprintf("x");
	}
}

/* Initial core */
static core_t* core_bsp = NULL;

/**
 * The kernel start function. The kernel begins executing from
 * this function, this is called by the limine bootloader.
 * 
 * It prints the kernel information and initializes the kernel uptil the scheduler
 * when a kinit thread is started which finished the initialization
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

	core_bsp = malloc(sizeof(core_bsp));
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

	/* Disable the PIC because we are using the ioapic */
	outportb(0xA1, 0xff);
	outportb(0x21, 0xff);

	/* Initialize the HPET timer */
	hpet_init();

	/* Initialize multicore */
	smp_init();

	/* All done */
	asm volatile ("1: hlt; jmp 1b" ::: "memory");
}