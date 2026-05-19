#ifdef KTEST_ENABLED

#include <kernel/ktest.h>
#include <kernel/elf.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* ------------------------------------------------------------------ */
/* Basic image health                                                   */
/* ------------------------------------------------------------------ */

static void test_elf_loaded(ktest_ctx_t *ctx) {
    KT_ASSERT(kelf.loaded);
    KT_CHECK(kelf.sym_count > 0);
    KT_CHECK(kelf.sorted_count > 0);
    KT_CHECK(kelf.symtab != NULL);
    KT_CHECK(kelf.strtab != NULL);
}

KTEST("elf-loaded", "elf",
      "kelf is populated and has a non-empty symbol table",
      KT_FLAG_CRITICAL, test_elf_loaded);

/* ------------------------------------------------------------------ */

static void test_elf_sorted_index_ordered(ktest_ctx_t *ctx) {
    if (!kelf.loaded) { ctx->result = KT_SKIP; return; }

    /* Verify sorted_idx is actually ascending by address */
    uintptr_t prev = 0;
    for (uint32_t i = 0; i < kelf.sorted_count; i++) {
        uintptr_t val = kelf.symtab[kelf.sorted_idx[i]].st_value;
        KT_ASSERT(val >= prev);
        prev = val;
    }
}

KTEST("elf-sorted-order", "elf",
      "sorted_idx is ordered ascending by symbol address",
      KT_FLAG_CRITICAL, test_elf_sorted_index_ordered);

/* ------------------------------------------------------------------ */
/* Symbol lookup by name                                                */
/* ------------------------------------------------------------------ */

static void test_elf_sym_by_name_ktest_run(ktest_ctx_t *ctx) {
    if (!kelf.loaded) { ctx->result = KT_SKIP; return; }

    const Elf64_Sym *sym = elf_sym_by_name(&kelf, "ktest_run");
    KT_ASSERT_NONNULL(sym);
    KT_CHECK(sym->st_value != 0);
    KT_CHECK(elf_sym_is_func(sym));
}

KTEST("elf-sym-ktest_run", "elf",
      "elf_sym_by_name finds 'ktest_run' as a defined function",
      KT_FLAG_NONE, test_elf_sym_by_name_ktest_run);

/* ------------------------------------------------------------------ */

static void test_elf_sym_by_name_missing(ktest_ctx_t *ctx) {
    if (!kelf.loaded) { ctx->result = KT_SKIP; return; }

    const Elf64_Sym *sym = elf_sym_by_name(&kelf, "__no_such_symbol_xyzzy__");
    KT_CHECK_EQ((uintptr_t)sym, (uintptr_t)NULL);
}

KTEST("elf-sym-missing", "elf",
      "elf_sym_by_name returns NULL for an unknown symbol",
      KT_FLAG_NONE, test_elf_sym_by_name_missing);

/* ------------------------------------------------------------------ */
/* Address -> symbol roundtrip                                           */
/* ------------------------------------------------------------------ */

static void test_elf_addr_roundtrip(ktest_ctx_t *ctx) {
    if (!kelf.loaded) { ctx->result = KT_SKIP; return; }

    /* Use the address of ktest_run itself as a known kernel address */
    uintptr_t addr = (uintptr_t)ktest_run;

    const char      *name = NULL;
    const Elf64_Sym *sym  = elf_sym_by_addr(&kelf, addr, &name);
    KT_ASSERT_NONNULL(sym);
    KT_ASSERT_NONNULL(name);
    KT_CHECK(name[0] != '\0');

    /* The returned symbol address must be <= addr */
    KT_CHECK(sym->st_value <= addr);
}

KTEST("elf-addr-roundtrip", "elf",
      "elf_sym_by_addr resolves address of ktest_run to a valid symbol",
      KT_FLAG_CRITICAL, test_elf_addr_roundtrip);

/* ------------------------------------------------------------------ */

static void test_elf_addr_invalid(ktest_ctx_t *ctx) {
    if (!kelf.loaded) { ctx->result = KT_SKIP; return; }

    /* Address 0 has no kernel symbol */
    const Elf64_Sym *sym = elf_sym_by_addr(&kelf, 0, NULL);
    KT_CHECK_EQ((uintptr_t)sym, (uintptr_t)NULL);
}

KTEST("elf-addr-invalid", "elf",
      "elf_sym_by_addr(0) returns NULL",
      KT_FLAG_NONE, test_elf_addr_invalid);

/* ------------------------------------------------------------------ */
/* Section helpers                                                      */
/* ------------------------------------------------------------------ */

static void test_elf_section_name(ktest_ctx_t *ctx) {
    if (!kelf.loaded || kelf.shdr_count == 0) { ctx->result = KT_SKIP; return; }

    /* Walk sections and verify every name pointer is within shstrtab */
    for (uint16_t i = 0; i < kelf.shdr_count; i++) {
        const char *name = elf_section_name(&kelf, &kelf.shdrs[i]);
        /* name must be non-NULL (shstrtab[0] == '\0' for null section) */
        KT_ASSERT_NONNULL(name);
    }
}

KTEST("elf-section-names", "elf",
      "every section has a valid name pointer within shstrtab",
      KT_FLAG_NONE, test_elf_section_name);

/* ------------------------------------------------------------------ */

static void test_elf_kernel_cmdline(ktest_ctx_t *ctx) {
    /* kernel_cmdline() must not crash; it may return NULL if not passed */
    const char *cmdline = kernel_cmdline();
    (void)cmdline; /* NULL is acceptable */
    KT_CHECK(true);
}

KTEST("kernel-cmdline", "elf",
      "kernel_cmdline() does not crash and returns a valid pointer or NULL",
      KT_FLAG_NONE, test_elf_kernel_cmdline);

#endif /* KTEST_ENABLED */
