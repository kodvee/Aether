#pragma once

#include <stdint.h>
#include <stddef.h>

struct flanterm_context;

void printf_init(void);
void kprintf(const char* fmt, ...);

/*
 * kwrite - write raw bytes to all output sinks (serial + framebuffer).
 * Used by sys_write to forward user output without sprintf overhead.
 */
void kwrite(const char *buf, size_t len);

/* Returns the active flanterm context, or NULL before printf_init(). */
struct flanterm_context *printf_get_context(void);

static inline void clear_screen(void) {
	kprintf("\033[2J");
}

static inline void reset_cursor(void) {
	kprintf("\x1b[0;0H");
}