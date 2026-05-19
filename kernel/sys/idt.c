/**
 * idt.c: Interrupt Descriptor table
 *
 * Handles the loading, initializing of idt
 * Handles setting ISRs, Directing interrupts to be handled as exceptions or IRQs
 */

#include <kernel/cpu.h>
#include <kernel/int.h>
#include <stdint.h>
#include <stddef.h>
#include <kernel/kprintf.h>
#include <kernel/elf.h>
#include <kernel/spinlock.h>
#include <kernel/mmu.h>
#include <kernel/cpu.h>
#include <kernel/macros.h>
#include <kernel/panic.h>

static struct idt_pointer idtp;
static idt_entry_t idt[256];

static uint8_t free_vector = 32;

void idt_set_gate(uint8_t num, void* handler, uint16_t selector, uint8_t flags, int userspace) {
	uintptr_t base = (uintptr_t)handler;
	idt[num].base_low  = (base) & 0xFFFF;
	idt[num].base_mid  = (base >> 16) & 0xFFFF;
	idt[num].base_high = (base >> 32) & 0xFFFFFFFF;
	idt[num].selector = selector;
	idt[num].zero = 0;
	idt[num].pad = 0;
	idt[num].flags = flags | (userspace ? 0x60 : 0);
}

uint8_t idt_allocate(void) {
	static spinlock_t lock = SPINLOCK_ZERO;
	bool int_state = spinlock_acquire(&lock);

	if (free_vector == 255)
		SUBSYS_PANIC("idt", "IDT vectors exhausted");

	uint8_t ret = free_vector++;
	spinlock_release(&lock, int_state);
	return ret;
}

/* All cores share the same idt */
irq_t *irqs = NULL;

/* isrs are defined in the int.S file */
extern void *isrs[];

void __init idt_init(void) {
	idtp.limit = sizeof(idt);
	idtp.base  = (uintptr_t)&idt;

	for (uint64_t i = 0; i < 256; i++)
		idt_set_gate(i, isrs[i], 0x8, 0x8e, 0);

	idt_reload();
	irqs = malloc(sizeof(irq_t) * IRQ_COUNT);
	/* Zero the array so uninstalled handlers are NULL */
	for (int i = 0; i < IRQ_COUNT; i++)
		irqs[i] = NULL;
}

void idt_reload(void) {
	asm volatile ("lidt %0" : : "m"(idtp));
}

void irq_install(irq_t irq, int index) {
	irqs[index - 32] = irq;
	const char *irq_sym = NULL;
	elf_sym_by_addr(&kelf, (uintptr_t)irqs[index - 32], &irq_sym);
	kprintf("irq: Install IRQ %d at %p [%s]\n",
	        index - 32, (void *)(uintptr_t)irqs[index - 32],
	        irq_sym ? irq_sym : "???");
}

static void _exception(struct regs *r, const char *description) {
	if ((r->cs & 0x3) == 0)
		EXCEPTION_PANIC("cpu", description, r);
}

#define EXC(i, n) case i: _exception(r, n); break;

static core_t __seg_gs const* core_local = 0;

struct regs* isr_handler(struct regs* r) {
	switch (r->int_no) {
		/* CPU exceptions */
		EXC(0,  "divide-by-zero")
		EXC(3,  "breakpoint")
		EXC(4,  "overflow")
		EXC(5,  "bound range exceeded")
		EXC(6,  "invalid opcode")
		EXC(7,  "device not available")
		case 8:  DOUBLE_FAULT_PANIC(r); break;
		EXC(10, "invalid TSS")
		EXC(11, "segment not present")
		EXC(12, "stack-segment fault")
		case 13: EXCEPTION_PANIC("cpu", "general protection fault", r); break;
		case 14: PAGE_FAULT_PANIC(r); break;
		EXC(16, "x87 floating-point exception")
		EXC(17, "alignment check")
		EXC(18, "machine check")
		EXC(19, "SIMD floating-point exception")
		EXC(20, "virtualization exception")
		EXC(21, "control protection exception")
		EXC(28, "hypervisor injection exception")
		EXC(29, "VMM communication exception")
		EXC(30, "security exception")

		/* HALT signal - secondary cores halt during SMP panic freeze */
		case 255: {
			asm volatile ("cli; 1: hlt; jmp 1b" ::: "memory");
		} break;

		default: {
			uint64_t vec = r->int_no;
			/* Dynamic IRQ dispatch: all installed vectors 32–254 */
			if (vec >= 32 && vec < 255) {
				irq_t h = irqs[vec - 32];
				if (h) return h(r);
			}
			kprintf("int: unexpected interrupt on core %lu, vector=%lu\n",
			        core_local->lapic_id, vec);
		} break;
	}

	return r;
}
