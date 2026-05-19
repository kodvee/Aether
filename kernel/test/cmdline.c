#ifdef KTEST_ENABLED

#include "ktest_internal.h"
#include <kernel/elf.h>
#include <string.h>
#include <stddef.h>
#include <memory.h>

static ktest_config_t g_config;

/*
 * Find a token in the cmdline.  Returns a pointer to the character
 * immediately after `key` if the token matches, or NULL if not found.
 * A token matches if it equals key exactly, or starts with key followed
 * by '=' or '.'.  Tokens are whitespace-delimited.
 */
static const char *cmdline_tok(const char *cmdline, const char *key) {
    if (!cmdline) return NULL;
    size_t klen = strlen(key);
    const char *p = cmdline;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (strncmp(p, key, klen) == 0) {
            char next = p[klen];
            if (next == '\0' || next == ' ' || next == '=' || next == '.')
                return p + klen;
        }
        while (*p && *p != ' ') p++;
    }
    return NULL;
}

/* Return the value part of "key=value", or NULL if bare token / absent. */
static const char *cmdline_val(const char *cmdline, const char *key) {
    const char *tok = cmdline_tok(cmdline, key);
    if (!tok) return NULL;
    if (*tok == '=') return tok + 1;
    return NULL;
}

/* Copy at most dst_size-1 chars from src up to (but not including) stop_char. */
static void copy_until(char *dst, const char *src, char stop_char, size_t dst_size) {
    size_t i = 0;
    while (src[i] && src[i] != stop_char && src[i] != ' ' && i + 1 < dst_size) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

void ktest_config_init(void) {
    const char *cmdline = kernel_cmdline();

    /* Is testing even requested? */
    if (!cmdline_tok(cmdline, "ktest")) {
        g_config.enabled = false;
        return;
    }
    g_config.enabled = true;

    /* Mode: ktest=<mode> */
    const char *mode_val = cmdline_val(cmdline, "ktest");
    if (!mode_val || *mode_val == '\0') {
        g_config.mode = KTEST_MODE_ALL;
    } else if (strncmp(mode_val, "critical", 8) == 0) {
        g_config.mode = KTEST_MODE_CRITICAL;
    } else if (strncmp(mode_val, "stress", 6) == 0) {
        g_config.mode = KTEST_MODE_STRESS;
    } else if (strncmp(mode_val, "panic", 5) == 0) {
        g_config.mode = KTEST_MODE_PANIC;
    } else if (strncmp(mode_val, "smp", 3) == 0) {
        g_config.mode = KTEST_MODE_SMP;
    } else {
        g_config.mode = KTEST_MODE_ALL;
    }

    /* Subsystem filter: ktest.sub=<subsystem> */
    const char *sub = cmdline_val(cmdline, "ktest.sub");
    if (sub) copy_until(g_config.subsystem, sub, '\0', sizeof(g_config.subsystem));

    /* Name filter: ktest.filter=<substring> */
    const char *filt = cmdline_val(cmdline, "ktest.filter");
    if (filt) copy_until(g_config.filter, filt, '\0', sizeof(g_config.filter));

    /* Flags */
    g_config.headless = !!cmdline_tok(cmdline, "ktest.headless");
    g_config.ci_exit  = !!cmdline_tok(cmdline, "ktest.ci");
}

const ktest_config_t *ktest_config_get(void) {
    return &g_config;
}

#endif /* KTEST_ENABLED */
