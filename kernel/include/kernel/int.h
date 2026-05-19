#pragma once

#include <kernel/cpu.h>
#include <stdint.h>
#include <stddef.h>

/**
 * Interrupt descriptor table
*/
typedef struct {
    uint16_t base_low;
    uint16_t selector;

    uint8_t zero;
    uint8_t flags;

    uint16_t base_mid;
    uint32_t base_high;
    uint32_t pad;
} __attribute__((packed)) idt_entry_t;

struct idt_pointer {
	uint16_t  limit;
	uintptr_t base;
} __attribute__((packed));

/*
 * IDT/IRQ invariants:
 *   - idt_init() must be called before irq_install/irq_get/irq_uninstall.
 *   - Dynamic vectors are allocated via idt_allocate() (range 32-254).
 *   - Vector 255 is the SMP halt IPI and must not be installed as an IRQ.
 *   - irq_install/irq_get/irq_uninstall are not interrupt-safe; call from
 *     normal context only (interrupts may be enabled).
 */

typedef struct regs* (*irq_t)(struct regs* r);

#define IRQ_COUNT 224   /* vectors 32-255 */

void    idt_init(void);
void    idt_reload(void);
uint8_t idt_allocate(void);

void  irq_install(irq_t irq, int vector);
irq_t irq_get(int vector);
void  irq_uninstall(int vector);