#ifdef KTEST_ENABLED

#include "ktest_internal.h"
#include <kernel/kprintf.h>
#include <kernel/ports.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <memory.h>

/* ------------------------------------------------------------------ */
/* Global: the currently executing test ctx (read by panic hook)      */
/* ------------------------------------------------------------------ */

volatile ktest_ctx_t *g_ktest_active_ctx = NULL;

/* ------------------------------------------------------------------ */
/* QEMU isa-debug-exit                                                */
/*   Write V to port 0xF4 -> QEMU exits with code (V<<1)|1             */
/*   0 -> exit 1 (success), 1 -> exit 3 (failure)                       */
/* ------------------------------------------------------------------ */

#define QEMU_EXIT_PORT 0xF4u

static void qemu_exit(bool success) {
    outportb(QEMU_EXIT_PORT, success ? 0u : 1u);
    /* Should not return, but halt in case the device isn't present */
    asm volatile ("cli; 1: hlt; jmp 1b" ::: "memory");
}

/* ------------------------------------------------------------------ */
/* Output                                                               */
/* kprintf always writes to serial + framebuffer (if available), so    */
/* headless mode works without any special handling here.               */
/* ------------------------------------------------------------------ */

static bool g_headless;

#define TOUT(...) kprintf(__VA_ARGS__)

/* ------------------------------------------------------------------ */
/* Result string                                                        */
/* ------------------------------------------------------------------ */

static const char *result_str(ktest_result_t r) {
    switch (r) {
        case KT_PASS:             return "PASS";
        case KT_FAIL:             return "FAIL";
        case KT_SKIP:             return "SKIP";
        case KT_PANIC_EXPECTED:   return "PANIC-OK";
        case KT_PANIC_UNEXPECTED: return "PANIC!";
        case KT_TIMEOUT:          return "TIMEOUT";
        default:                  return "???";
    }
}

/* ------------------------------------------------------------------ */
/* _kt_record: record one assertion result                              */
/* ------------------------------------------------------------------ */

void _kt_record(ktest_ctx_t *ctx, bool ok, const char *expr,
                const char *file, int line) {
    if (ok) {
        ctx->checks_passed++;
    } else {
        ctx->checks_failed++;
        TOUT("    FAIL  %s:%d  %s\n", file, line, expr);
    }
}

/* ------------------------------------------------------------------ */
/* Filter logic                                                         */
/* ------------------------------------------------------------------ */

static bool should_run(const ktest_entry_t *e, const ktest_config_t *cfg) {
    if (e->flags & KT_FLAG_SKIP) return false;

    /* Mode filter */
    switch (cfg->mode) {
        case KTEST_MODE_CRITICAL:
            if (!(e->flags & KT_FLAG_CRITICAL)) return false;
            break;
        case KTEST_MODE_STRESS:
            if (!(e->flags & KT_FLAG_STRESS)) return false;
            break;
        case KTEST_MODE_PANIC:
            if (!(e->flags & KT_FLAG_PANIC)) return false;
            break;
        case KTEST_MODE_SMP:
            if (!(e->flags & KT_FLAG_SMP)) return false;
            break;
        case KTEST_MODE_ALL:
        default:
            /* stress and destructive are opt-in even in ALL mode */
            if ((e->flags & KT_FLAG_STRESS) || (e->flags & KT_FLAG_DESTRUCTIVE))
                return false;
            break;
    }

    /* Subsystem filter */
    if (cfg->subsystem[0] != '\0') {
        if (strcmp(e->subsystem, cfg->subsystem) != 0)
            return false;
    }

    /* Name substring filter */
    if (cfg->filter[0] != '\0') {
        if (!strstr(e->name, cfg->filter))
            return false;
    }

    return true;
}

/* ------------------------------------------------------------------ */
/* Execute one test                                                     */
/* ------------------------------------------------------------------ */

static ktest_result_t run_one(const ktest_entry_t *e) {
    ktest_ctx_t ctx = {
        .name             = e->name,
        .subsystem        = e->subsystem,
        .flags            = e->flags,
        .expect_panic     = false,
        .expected_panic_msg = NULL,
        .panic_caught     = false,
        .checks_passed    = 0,
        .checks_failed    = 0,
        .result           = KT_PASS,
    };

    g_ktest_active_ctx = &ctx;

    e->fn(&ctx);

    g_ktest_active_ctx = NULL;

    /* If the test result is still PASS but checks failed, mark it failed */
    if (ctx.result == KT_PASS && ctx.checks_failed > 0)
        ctx.result = KT_FAIL;

    return ctx.result;
}

/* ------------------------------------------------------------------ */
/* ktest_run: main entry point                                          */
/* ------------------------------------------------------------------ */

int ktest_run(void) {
    ktest_config_init();

    const ktest_config_t *cfg = ktest_config_get();
    if (!cfg->enabled) return 0;

    g_headless = cfg->headless;

    ptrdiff_t total_registered = __ktest_end - __ktest_start;

    TOUT("\n");
    TOUT("=========================================\n");
    TOUT("  Aether kernel test suite\n");
    TOUT("=========================================\n");
    TOUT("  registered: %ld\n", (long)total_registered);

    uint32_t n_run  = 0;
    uint32_t n_pass = 0;
    uint32_t n_fail = 0;
    uint32_t n_skip = 0;

    for (ktest_entry_t *e = __ktest_start; e < __ktest_end; e++) {
        if (!should_run(e, cfg)) {
            n_skip++;
            continue;
        }

        TOUT("\n[ RUN  ] %s::%s\n", e->subsystem, e->name);
        if (e->description && e->description[0])
            TOUT("         %s\n", e->description);

        ktest_result_t res = run_one(e);
        n_run++;

        const char *tag = result_str(res);
        TOUT("[ %-6s] %s::%s\n", tag, e->subsystem, e->name);

        if (res == KT_PASS || res == KT_PANIC_EXPECTED)
            n_pass++;
        else
            n_fail++;
    }

    TOUT("\n=========================================\n");
    TOUT("  Results: %u run, %u passed, %u failed, %u skipped\n",
         n_run, n_pass, n_fail, n_skip);
    TOUT("=========================================\n\n");

    bool success = (n_fail == 0);

    if (cfg->ci_exit)
        qemu_exit(success);

    return (int)n_fail;
}

#endif /* KTEST_ENABLED */
