#ifdef KTEST_ENABLED

#include <kernel/ktest.h>
#include <kernel/types.h>
#include <kernel/syscall.h>
#include <stdint.h>
#include <stddef.h>

/* ================================================================== */
/* Group 1: identity type sizes                                         */
/*                                                                      */
/* These types are referenced across the ABI boundary (e.g. in         */
/* syscall return values and thread identifiers).  A silent size change */
/* breaks binary compatibility.                                         */
/* ================================================================== */

static void test_pid_t_size(ktest_ctx_t *ctx) {
    KT_CHECK_EQ(sizeof(pid_t), (size_t)4);
}

KTEST("types-pid-size", "types",
      "pid_t is exactly 4 bytes (int32_t)",
      KT_FLAG_CRITICAL, test_pid_t_size);

/* ------------------------------------------------------------------ */

static void test_tid_t_size(ktest_ctx_t *ctx) {
    KT_CHECK_EQ(sizeof(tid_t), (size_t)4);
}

KTEST("types-tid-size", "types",
      "tid_t is exactly 4 bytes (int32_t)",
      KT_FLAG_CRITICAL, test_tid_t_size);

/* ------------------------------------------------------------------ */

static void test_cpu_id_t_size(ktest_ctx_t *ctx) {
    KT_CHECK_EQ(sizeof(cpu_id_t), (size_t)4);
}

KTEST("types-cpu-id-size", "types",
      "cpu_id_t is exactly 4 bytes (uint32_t)",
      KT_FLAG_CRITICAL, test_cpu_id_t_size);

/* ------------------------------------------------------------------ */

static void test_errno_t_size(ktest_ctx_t *ctx) {
    KT_CHECK_EQ(sizeof(errno_t), sizeof(int));
}

KTEST("types-errno-size", "types",
      "errno_t is the same size as int",
      KT_FLAG_CRITICAL, test_errno_t_size);

/* ------------------------------------------------------------------ */

static void test_vaddr_paddr_size(ktest_ctx_t *ctx) {
    KT_CHECK_EQ(sizeof(vaddr_t), (size_t)8);
    KT_CHECK_EQ(sizeof(paddr_t), (size_t)8);
}

KTEST("types-addr-size", "types",
      "vaddr_t and paddr_t are 8 bytes on x86-64",
      KT_FLAG_CRITICAL, test_vaddr_paddr_size);

/* ================================================================== */
/* Group 2: pid_t / tid_t signedness                                    */
/* ================================================================== */

static void test_pid_t_signed(ktest_ctx_t *ctx) {
    pid_t p = -1;
    KT_CHECK(p < 0);
}

KTEST("types-pid-signed", "types",
      "pid_t is a signed type (-1 is representable and negative)",
      KT_FLAG_CRITICAL, test_pid_t_signed);

/* ------------------------------------------------------------------ */

static void test_tid_t_signed(ktest_ctx_t *ctx) {
    tid_t t = -1;
    KT_CHECK(t < 0);
}

KTEST("types-tid-signed", "types",
      "tid_t is a signed type (-1 is representable and negative)",
      KT_FLAG_CRITICAL, test_tid_t_signed);

/* ------------------------------------------------------------------ */

static void test_cpu_id_t_unsigned(ktest_ctx_t *ctx) {
    cpu_id_t c = (cpu_id_t)-1;
    KT_CHECK(c > 0); /* wraps to UINT32_MAX, not -1 */
}

KTEST("types-cpu-id-unsigned", "types",
      "cpu_id_t is unsigned (no negative values)",
      KT_FLAG_CRITICAL, test_cpu_id_t_unsigned);

/* ================================================================== */
/* Group 3: error constant values                                        */
/*                                                                      */
/* These values are returned via the syscall ABI and must match the     */
/* POSIX/Linux errno numbering exactly.                                  */
/* ================================================================== */

static void test_errno_constants(ktest_ctx_t *ctx) {
    KT_CHECK_EQ(EOK,     0);
    KT_CHECK_EQ(EPERM,   1);
    KT_CHECK_EQ(ENOENT,  2);
    KT_CHECK_EQ(ESRCH,   3);
    KT_CHECK_EQ(EINTR,   4);
    KT_CHECK_EQ(EBADF,   9);
    KT_CHECK_EQ(ENOMEM, 12);
    KT_CHECK_EQ(EFAULT, 14);
    KT_CHECK_EQ(EBUSY,  16);
    KT_CHECK_EQ(EINVAL, 22);
    KT_CHECK_EQ(ENOTTY, 25);
    KT_CHECK_EQ(EDEADLK,35);
    KT_CHECK_EQ(ENOSYS, 38);
    KT_CHECK_EQ(ENOTSUP,95);
}

KTEST("types-errno-constants", "types",
      "all errno constants match POSIX/Linux numbering",
      KT_FLAG_CRITICAL, test_errno_constants);

/* ================================================================== */
/* Group 4: syscall_frame_t layout                                      */
/*                                                                      */
/* syscall.S hardcodes byte offsets for every field in syscall_frame_t. */
/* A layout mismatch silently corrupts every syscall argument.          */
/* ================================================================== */

static void test_syscall_frame_size(ktest_ctx_t *ctx) {
    /* 16 fields × 8 bytes = 128 bytes */
    KT_CHECK_EQ(sizeof(syscall_frame_t), (size_t)128);
}

KTEST("types-syscall-frame-size", "types",
      "syscall_frame_t is exactly 128 bytes (16 x uint64_t fields)",
      KT_FLAG_CRITICAL, test_syscall_frame_size);

/* ------------------------------------------------------------------ */

static void test_syscall_frame_offsets(ktest_ctx_t *ctx) {
    KT_CHECK_EQ(offsetof(syscall_frame_t, rax),      (size_t)  0);
    KT_CHECK_EQ(offsetof(syscall_frame_t, rbx),      (size_t)  8);
    KT_CHECK_EQ(offsetof(syscall_frame_t, rcx),      (size_t) 16);
    KT_CHECK_EQ(offsetof(syscall_frame_t, rdx),      (size_t) 24);
    KT_CHECK_EQ(offsetof(syscall_frame_t, rsi),      (size_t) 32);
    KT_CHECK_EQ(offsetof(syscall_frame_t, rdi),      (size_t) 40);
    KT_CHECK_EQ(offsetof(syscall_frame_t, rbp),      (size_t) 48);
    KT_CHECK_EQ(offsetof(syscall_frame_t, r8),       (size_t) 56);
    KT_CHECK_EQ(offsetof(syscall_frame_t, r9),       (size_t) 64);
    KT_CHECK_EQ(offsetof(syscall_frame_t, r10),      (size_t) 72);
    KT_CHECK_EQ(offsetof(syscall_frame_t, r11),      (size_t) 80);
    KT_CHECK_EQ(offsetof(syscall_frame_t, r12),      (size_t) 88);
    KT_CHECK_EQ(offsetof(syscall_frame_t, r13),      (size_t) 96);
    KT_CHECK_EQ(offsetof(syscall_frame_t, r14),      (size_t)104);
    KT_CHECK_EQ(offsetof(syscall_frame_t, r15),      (size_t)112);
    KT_CHECK_EQ(offsetof(syscall_frame_t, user_rsp), (size_t)120);
}

KTEST("types-syscall-frame-offsets", "types",
      "syscall_frame_t field offsets match the syscall.S byte encoding",
      KT_FLAG_CRITICAL, test_syscall_frame_offsets);

/* ================================================================== */
/* Group 5: irq_state_t                                                 */
/* ================================================================== */

static void test_irq_state_t_size(ktest_ctx_t *ctx) {
    /* irq_state_t is bool */
    KT_CHECK_EQ(sizeof(irq_state_t), sizeof(bool));
}

KTEST("types-irq-state-size", "types",
      "irq_state_t is the same size as bool",
      KT_FLAG_NONE, test_irq_state_t_size);

#endif /* KTEST_ENABLED */
