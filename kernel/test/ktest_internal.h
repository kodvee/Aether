#pragma once
#ifdef KTEST_ENABLED

#include <kernel/ktest.h>
#include <stdint.h>
#include <stdbool.h>

/* ------------------------------------------------------------------ */
/* Runtime configuration (populated from kernel cmdline at boot)        */
/* ------------------------------------------------------------------ */

typedef enum {
    KTEST_MODE_ALL = 0,
    KTEST_MODE_CRITICAL,
    KTEST_MODE_STRESS,
    KTEST_MODE_PANIC,
    KTEST_MODE_SMP,
} ktest_mode_t;

typedef struct {
    bool         enabled;
    ktest_mode_t mode;
    bool         headless;        /* serial-only; skip flanterm writes */
    bool         ci_exit;         /* emit QEMU isa-debug-exit on completion */
    char         subsystem[32];   /* empty = no subsystem filter */
    char         filter[64];      /* empty = no name filter */
} ktest_config_t;

/* Populated by ktest_config_init(), read by runner */
const ktest_config_t *ktest_config_get(void);
void ktest_config_init(void);

/* Linker section bounds (defined in linker.ld) */
extern ktest_entry_t __ktest_start[];
extern ktest_entry_t __ktest_end[];

#endif /* KTEST_ENABLED */
