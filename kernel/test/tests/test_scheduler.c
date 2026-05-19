#ifdef KTEST_ENABLED

#include <kernel/ktest.h>
#include <kernel/scheduler.h>
#include <kernel/cpu.h>
#include <kernel/list.h>
#include <kernel/spinlock.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Trampoline defined in sys/switch.S; used as the initial rip for every new
 * thread.  We only need its address here for context-register verification. */
extern void thread_entry_trampoline(void);

/* coreCount is set by smp_init before tests run */
extern uint64_t coreCount;

/* Dummy entry functions used to populate thread context */
static void _entry_a(void) {}
static void _entry_b(void) {}
static void _entry_c(void) {}

/* ================================================================== */
/* Group 1: context_regs_t layout                                      */
/*                                                                      */
/* The offsets here must exactly match the byte offsets used by the    */
/* context_switch() assembly stub in sys/switch.S.  If they drift,     */
/* every context switch silently corrupts registers.                    */
/* ================================================================== */

static void test_ctx_size(ktest_ctx_t *ctx) {
    KT_CHECK_EQ(sizeof(context_regs_t), (size_t)64);
}

KTEST("sched-ctx-size", "scheduler",
      "context_regs_t is exactly 64 bytes (8 x 8-byte fields)",
      KT_FLAG_CRITICAL, test_ctx_size);

/* ------------------------------------------------------------------ */

static void test_ctx_offsets(ktest_ctx_t *ctx) {
    /* switch.S encodes these byte offsets directly in its mov instructions.
     * Any change to the struct layout must be mirrored in the asm stub. */
    KT_CHECK_EQ(offsetof(context_regs_t, rip),  (size_t) 0);
    KT_CHECK_EQ(offsetof(context_regs_t, rsp),  (size_t) 8);
    KT_CHECK_EQ(offsetof(context_regs_t, rbx),  (size_t)16);
    KT_CHECK_EQ(offsetof(context_regs_t, rbp),  (size_t)24);
    KT_CHECK_EQ(offsetof(context_regs_t, r12),  (size_t)32);
    KT_CHECK_EQ(offsetof(context_regs_t, r13),  (size_t)40);
    KT_CHECK_EQ(offsetof(context_regs_t, r14),  (size_t)48);
    KT_CHECK_EQ(offsetof(context_regs_t, r15),  (size_t)56);
}

KTEST("sched-ctx-offsets", "scheduler",
      "context_regs_t field offsets match the switch.S byte encoding",
      KT_FLAG_CRITICAL, test_ctx_offsets);

/* ================================================================== */
/* Group 2: thread_state_t enum stability                              */
/* ================================================================== */

static void test_thread_state_values(ktest_ctx_t *ctx) {
    /* These values are used as integer tags in context switch paths and
     * assertions.  A silent renumbering would be a correctness bug. */
    KT_CHECK_EQ((int)THREAD_CREATED, 0);
    KT_CHECK_EQ((int)THREAD_READY,   1);
    KT_CHECK_EQ((int)THREAD_RUNNING, 2);
    KT_CHECK_EQ((int)THREAD_BLOCKED, 3);
    KT_CHECK_EQ((int)THREAD_DEAD,    4);
}

KTEST("sched-thread-state-enum", "scheduler",
      "thread_state_t enum values are stable (CREATED=0 .. DEAD=4)",
      KT_FLAG_CRITICAL, test_thread_state_values);

/* ================================================================== */
/* Group 3: kernel stack allocation                                    */
/* ================================================================== */

static void test_kstack_alloc_basic(ktest_ctx_t *ctx) {
    uintptr_t top = kstack_alloc();
    KT_ASSERT(top != 0);
    KT_CHECK_EQ(top & (PAGE_SIZE - 1), (uintptr_t)0);  /* page-aligned */
    kstack_free(top);
}

KTEST("sched-kstack-alloc-basic", "scheduler",
      "kstack_alloc returns a non-zero, page-aligned address",
      KT_FLAG_CRITICAL, test_kstack_alloc_basic);

/* ------------------------------------------------------------------ */

static void test_kstack_size(ktest_ctx_t *ctx) {
    uintptr_t top = kstack_alloc();
    KT_ASSERT(top != 0);
    uintptr_t bottom = top - KSTACK_SIZE;
    /* Bottom must also be page-aligned */
    KT_CHECK_EQ(bottom & (PAGE_SIZE - 1), (uintptr_t)0);
    /* The region must be exactly KSTACK_SIZE bytes */
    KT_CHECK_EQ(top - bottom, (uintptr_t)KSTACK_SIZE);
    kstack_free(top);
}

KTEST("sched-kstack-size", "scheduler",
      "kstack region is exactly KSTACK_SIZE bytes, bottom is page-aligned",
      KT_FLAG_NONE, test_kstack_size);

/* ------------------------------------------------------------------ */

static void test_kstack_distinct(ktest_ctx_t *ctx) {
    uintptr_t t1 = kstack_alloc();
    uintptr_t t2 = kstack_alloc();
    KT_ASSERT(t1 != 0);
    KT_ASSERT(t2 != 0);
    KT_CHECK(t1 != t2);
    kstack_free(t1);
    kstack_free(t2);
}

KTEST("sched-kstack-distinct", "scheduler",
      "two kstack_alloc calls return non-overlapping stacks",
      KT_FLAG_NONE, test_kstack_distinct);

/* ------------------------------------------------------------------ */

static void test_kstack_writable(ktest_ctx_t *ctx) {
    uintptr_t top = kstack_alloc();
    KT_ASSERT(top != 0);

    /* Write to the first and last byte of the allocated region */
    volatile uint8_t *base = (volatile uint8_t *)(top - KSTACK_SIZE);
    volatile uint8_t *last = (volatile uint8_t *)(top - 1);
    *base = 0xAB;
    *last = 0xCD;
    KT_CHECK_EQ((int)*base, 0xAB);
    KT_CHECK_EQ((int)*last, 0xCD);

    kstack_free(top);
}

KTEST("sched-kstack-writable", "scheduler",
      "entire kstack region is mapped writable",
      KT_FLAG_NONE, test_kstack_writable);

/* ------------------------------------------------------------------ */

static void test_kstack_free_null(ktest_ctx_t *ctx) {
    /* kstack_free(0) must be a safe no-op */
    kstack_free(0);
    KT_CHECK(true);
}

KTEST("sched-kstack-free-null", "scheduler",
      "kstack_free(0) is a safe no-op",
      KT_FLAG_NONE, test_kstack_free_null);

/* ------------------------------------------------------------------ */

static void test_kstack_multi_alloc_free(ktest_ctx_t *ctx) {
    #define NK 8
    uintptr_t tops[NK];
    for (int i = 0; i < NK; i++) {
        tops[i] = kstack_alloc();
        KT_ASSERT(tops[i] != 0);
    }
    /* All addresses must be distinct */
    for (int i = 0; i < NK; i++) {
        for (int j = i + 1; j < NK; j++) {
            KT_CHECK(tops[i] != tops[j]);
        }
    }
    for (int i = 0; i < NK; i++)
        kstack_free(tops[i]);
    #undef NK
}

KTEST("sched-kstack-multi", "scheduler",
      "8 concurrent kstack allocations produce distinct addresses",
      KT_FLAG_NONE, test_kstack_multi_alloc_free);

/* ================================================================== */
/* Group 4: wait_queue_t initialization                                */
/* ================================================================== */

static void test_waitq_init_dynamic(ktest_ctx_t *ctx) {
    wait_queue_t wq;
    wait_queue_init(&wq);
    KT_CHECK(list_empty(&wq.waiters));
    KT_CHECK(!spinlock_is_held(&wq.lock));
}

KTEST("sched-waitq-init-dynamic", "scheduler",
      "wait_queue_init produces an empty queue with a free lock",
      KT_FLAG_CRITICAL, test_waitq_init_dynamic);

/* ------------------------------------------------------------------ */

static void test_waitq_init_static(ktest_ctx_t *ctx) {
    wait_queue_t wq = WAIT_QUEUE_INIT(wq);
    KT_CHECK(list_empty(&wq.waiters));
    KT_CHECK(!spinlock_is_held(&wq.lock));
}

KTEST("sched-waitq-init-static", "scheduler",
      "WAIT_QUEUE_INIT macro produces an empty queue with a free lock",
      KT_FLAG_NONE, test_waitq_init_static);

/* ------------------------------------------------------------------ */

static void test_waitq_wake_one_empty(ktest_ctx_t *ctx) {
    wait_queue_t wq;
    wait_queue_init(&wq);
    /* No thread is waiting; this must be a safe no-op */
    wait_queue_wake_one(&wq);
    KT_CHECK(list_empty(&wq.waiters));
}

KTEST("sched-waitq-wake-one-empty", "scheduler",
      "wait_queue_wake_one on an empty queue is a safe no-op",
      KT_FLAG_NONE, test_waitq_wake_one_empty);

/* ------------------------------------------------------------------ */

static void test_waitq_wake_all_empty(ktest_ctx_t *ctx) {
    wait_queue_t wq;
    wait_queue_init(&wq);
    wait_queue_wake_all(&wq);
    KT_CHECK(list_empty(&wq.waiters));
}

KTEST("sched-waitq-wake-all-empty", "scheduler",
      "wait_queue_wake_all on an empty queue is a safe no-op",
      KT_FLAG_NONE, test_waitq_wake_all_empty);

/* ================================================================== */
/* Group 5: thread_create                                              */
/*                                                                     */
/* thread_create() allocates a thread_t + kstack and sets the initial  */
/* context to run thread_entry_trampoline which calls the entry fn.    */
/* These tests create threads but NEVER enqueue or execute them.       */
/* ================================================================== */

static void test_thread_create_nonnull(ktest_ctx_t *ctx) {
    thread_t *t = thread_create(&kernel_process, _entry_a);
    KT_ASSERT_NONNULL(t);
    /* leak is acceptable in test mode */
}

KTEST("sched-thread-create-nonnull", "scheduler",
      "thread_create returns a non-NULL thread pointer",
      KT_FLAG_CRITICAL, test_thread_create_nonnull);

/* ------------------------------------------------------------------ */

static void test_thread_create_state(ktest_ctx_t *ctx) {
    thread_t *t = thread_create(&kernel_process, _entry_a);
    KT_ASSERT_NONNULL(t);
    KT_CHECK_EQ((int)t->state, (int)THREAD_CREATED);
}

KTEST("sched-thread-create-state", "scheduler",
      "newly created thread has THREAD_CREATED state",
      KT_FLAG_CRITICAL, test_thread_create_state);

/* ------------------------------------------------------------------ */

static void test_thread_create_context_rip(ktest_ctx_t *ctx) {
    thread_t *t = thread_create(&kernel_process, _entry_a);
    KT_ASSERT_NONNULL(t);
    /* rip must be the trampoline, not the entry function directly */
    KT_CHECK_EQ(t->context.rip, (uintptr_t)thread_entry_trampoline);
}

KTEST("sched-thread-create-rip", "scheduler",
      "new thread's context.rip points to thread_entry_trampoline",
      KT_FLAG_CRITICAL, test_thread_create_context_rip);

/* ------------------------------------------------------------------ */

static void test_thread_create_context_r12(ktest_ctx_t *ctx) {
    thread_t *t = thread_create(&kernel_process, _entry_a);
    KT_ASSERT_NONNULL(t);
    /* r12 carries the real entry function, read by the trampoline */
    KT_CHECK_EQ(t->context.r12, (uintptr_t)_entry_a);
}

KTEST("sched-thread-create-r12", "scheduler",
      "new thread's context.r12 holds the entry function address",
      KT_FLAG_CRITICAL, test_thread_create_context_r12);

/* ------------------------------------------------------------------ */

static void test_thread_create_context_callee_zeros(ktest_ctx_t *ctx) {
    thread_t *t = thread_create(&kernel_process, _entry_a);
    KT_ASSERT_NONNULL(t);
    /* All callee-saved regs other than r12 must be zero on a fresh thread */
    KT_CHECK_EQ(t->context.rbx, (uintptr_t)0);
    KT_CHECK_EQ(t->context.rbp, (uintptr_t)0);
    KT_CHECK_EQ(t->context.r13, (uintptr_t)0);
    KT_CHECK_EQ(t->context.r14, (uintptr_t)0);
    KT_CHECK_EQ(t->context.r15, (uintptr_t)0);
}

KTEST("sched-thread-create-callee-zeros", "scheduler",
      "new thread's callee-saved regs (except r12) are zeroed",
      KT_FLAG_NONE, test_thread_create_context_callee_zeros);

/* ------------------------------------------------------------------ */

static void test_thread_create_rsp(ktest_ctx_t *ctx) {
    thread_t *t = thread_create(&kernel_process, _entry_a);
    KT_ASSERT_NONNULL(t);
    /* rsp must equal kstack_top (stack starts empty, grows down) */
    KT_CHECK_EQ(t->context.rsp, t->kstack_top);
}

KTEST("sched-thread-create-rsp", "scheduler",
      "new thread's context.rsp equals kstack_top (empty stack)",
      KT_FLAG_NONE, test_thread_create_rsp);

/* ------------------------------------------------------------------ */

static void test_thread_create_kstack(ktest_ctx_t *ctx) {
    thread_t *t = thread_create(&kernel_process, _entry_a);
    KT_ASSERT_NONNULL(t);
    KT_CHECK(t->kstack_top != 0);
    KT_CHECK_EQ(t->kstack_top - t->kstack_bottom, (uintptr_t)KSTACK_SIZE);
    KT_CHECK_EQ(t->kstack_top & (PAGE_SIZE - 1), (uintptr_t)0);
}

KTEST("sched-thread-create-kstack", "scheduler",
      "new thread has a valid kstack (non-zero, correctly sized, aligned)",
      KT_FLAG_CRITICAL, test_thread_create_kstack);

/* ------------------------------------------------------------------ */

static void test_thread_create_tid(ktest_ctx_t *ctx) {
    thread_t *t = thread_create(&kernel_process, _entry_a);
    KT_ASSERT_NONNULL(t);
    KT_CHECK(t->tid >= 1);
}

KTEST("sched-thread-create-tid", "scheduler",
      "new thread receives a positive TID",
      KT_FLAG_CRITICAL, test_thread_create_tid);

/* ------------------------------------------------------------------ */

static void test_thread_create_parent(ktest_ctx_t *ctx) {
    process_t proc;
    proc.pid  = 99;
    proc.name = "test-proc";
    proc.pagemap = NULL;
    proc.lock = (spinlock_t)SPINLOCK_ZERO;
    list_head_init(&proc.threads);
    proc.thread_count = 0;

    thread_t *t = thread_create(&proc, _entry_a);
    KT_ASSERT_NONNULL(t);
    KT_CHECK(t->parent == &proc);
}

KTEST("sched-thread-create-parent", "scheduler",
      "new thread's parent pointer matches the supplied process",
      KT_FLAG_NONE, test_thread_create_parent);

/* ------------------------------------------------------------------ */

static void test_thread_create_lock_free(ktest_ctx_t *ctx) {
    thread_t *t = thread_create(&kernel_process, _entry_a);
    KT_ASSERT_NONNULL(t);
    KT_CHECK(!spinlock_is_held(&t->lock));
}

KTEST("sched-thread-create-lock-free", "scheduler",
      "new thread's lock is in the unheld (zero) state",
      KT_FLAG_NONE, test_thread_create_lock_free);

/* ------------------------------------------------------------------ */

static void test_thread_create_unique_tids(ktest_ctx_t *ctx) {
    #define NT 16
    thread_t *threads[NT];
    for (int i = 0; i < NT; i++) {
        threads[i] = thread_create(&kernel_process, _entry_a);
        KT_ASSERT_NONNULL(threads[i]);
    }
    /* Every TID must be distinct */
    for (int i = 0; i < NT; i++) {
        for (int j = i + 1; j < NT; j++) {
            KT_CHECK(threads[i]->tid != threads[j]->tid);
        }
    }
    #undef NT
}

KTEST("sched-thread-create-unique-tids", "scheduler",
      "16 thread_create calls produce 16 distinct TIDs",
      KT_FLAG_NONE, test_thread_create_unique_tids);

/* ------------------------------------------------------------------ */

static void test_thread_create_tid_monotone(ktest_ctx_t *ctx) {
    thread_t *a = thread_create(&kernel_process, _entry_a);
    thread_t *b = thread_create(&kernel_process, _entry_b);
    thread_t *c = thread_create(&kernel_process, _entry_c);
    KT_ASSERT_NONNULL(a);
    KT_ASSERT_NONNULL(b);
    KT_ASSERT_NONNULL(c);
    KT_CHECK(a->tid < b->tid);
    KT_CHECK(b->tid < c->tid);
}

KTEST("sched-thread-create-tid-monotone", "scheduler",
      "thread TIDs are strictly increasing across consecutive creates",
      KT_FLAG_NONE, test_thread_create_tid_monotone);

/* ------------------------------------------------------------------ */

static void test_thread_create_list_nodes_detached(ktest_ctx_t *ctx) {
    thread_t *t = thread_create(&kernel_process, _entry_a);
    KT_ASSERT_NONNULL(t);
    /* list_node_init leaves the node pointing to itself -- not on any list */
    KT_CHECK(t->list_node.next == &t->list_node);
    KT_CHECK(t->list_node.prev == &t->list_node);
    KT_CHECK(t->wq_node.next   == &t->wq_node);
    KT_CHECK(t->wq_node.prev   == &t->wq_node);
}

KTEST("sched-thread-create-nodes-detached", "scheduler",
      "new thread's list_node and wq_node are detached (not on any list)",
      KT_FLAG_NONE, test_thread_create_list_nodes_detached);

/* ================================================================== */
/* Group 6: process_t initialization                                   */
/* ================================================================== */

static void test_process_local_init(ktest_ctx_t *ctx) {
    process_t p;
    p.pid          = 42;
    p.name         = "myproc";
    p.pagemap      = NULL;
    p.lock         = (spinlock_t)SPINLOCK_ZERO;
    p.thread_count = 0;
    list_head_init(&p.threads);

    KT_CHECK_EQ((int)p.pid, 42);
    KT_CHECK(p.name != NULL);
    KT_CHECK_EQ((uintptr_t)p.pagemap, (uintptr_t)NULL);
    KT_CHECK(list_empty(&p.threads));
    KT_CHECK_EQ((int)p.thread_count, 0);
    KT_CHECK(!spinlock_is_held(&p.lock));
}

KTEST("sched-process-local-init", "scheduler",
      "manually initialised process_t has correct field values",
      KT_FLAG_NONE, test_process_local_init);

/* ================================================================== */
/* Group 7: scheduler_init + kernel_process                            */
/*                                                                     */
/* Each test below calls scheduler_init() at its start to guarantee a  */
/* clean global state.  Threads created inside these tests are not     */
/* executed; their stacks are intentionally leaked (bounded quantity). */
/* ================================================================== */

static void test_sched_init_kernel_proc_name(ktest_ctx_t *ctx) {
    scheduler_init();
    KT_ASSERT_NONNULL(kernel_process.name);
    KT_CHECK(kernel_process.name[0] != '\0');
}

KTEST("sched-init-kproc-name", "scheduler",
      "kernel_process.name is set to a non-empty string after scheduler_init",
      KT_FLAG_NONE, test_sched_init_kernel_proc_name);

/* ------------------------------------------------------------------ */

static void test_sched_init_kernel_proc_pid(ktest_ctx_t *ctx) {
    scheduler_init();
    KT_CHECK(kernel_process.pid >= 1);
}

KTEST("sched-init-kproc-pid", "scheduler",
      "kernel_process.pid is >= 1 after scheduler_init",
      KT_FLAG_NONE, test_sched_init_kernel_proc_pid);

/* ------------------------------------------------------------------ */

static void test_sched_init_kernel_proc_pagemap(ktest_ctx_t *ctx) {
    scheduler_init();
    KT_CHECK_EQ((uintptr_t)kernel_process.pagemap, (uintptr_t)NULL);
}

KTEST("sched-init-kproc-pagemap", "scheduler",
      "kernel_process.pagemap is NULL (kernel threads share the kernel map)",
      KT_FLAG_NONE, test_sched_init_kernel_proc_pagemap);

/* ------------------------------------------------------------------ */

static void test_sched_init_run_queues_empty(ktest_ctx_t *ctx) {
    scheduler_init();
    for (uint64_t i = 0; i < coreCount; i++) {
        KT_CHECK(list_empty(&cpu_core_local[i].run_queue));
    }
}

KTEST("sched-init-run-queues-empty", "scheduler",
      "all per-core run queues are empty immediately after scheduler_init",
      KT_FLAG_NONE, test_sched_init_run_queues_empty);

/* ------------------------------------------------------------------ */

static void test_sched_init_run_queue_locks_free(ktest_ctx_t *ctx) {
    scheduler_init();
    for (uint64_t i = 0; i < coreCount; i++) {
        KT_CHECK(!spinlock_is_held(&cpu_core_local[i].run_queue_lock));
    }
}

KTEST("sched-init-run-queue-locks-free", "scheduler",
      "all per-core run_queue_locks are unheld after scheduler_init",
      KT_FLAG_NONE, test_sched_init_run_queue_locks_free);

/* ------------------------------------------------------------------ */

static void test_sched_init_idempotent(ktest_ctx_t *ctx) {
    /* scheduler_init() must be safe to call twice (kernel.c calls it after
     * ktest_run returns).  The second call should produce a valid state. */
    scheduler_init();
    scheduler_init();

    KT_CHECK(kernel_process.pid >= 1);
    for (uint64_t i = 0; i < coreCount; i++) {
        KT_CHECK(list_empty(&cpu_core_local[i].run_queue));
    }
}

KTEST("sched-init-idempotent", "scheduler",
      "scheduler_init() called twice leaves the system in a valid state",
      KT_FLAG_NONE, test_sched_init_idempotent);

/* ================================================================== */
/* Group 8: thread_ready_on -- enqueue without execution                */
/* ================================================================== */

static void test_thread_ready_on_state(ktest_ctx_t *ctx) {
    scheduler_init();
    thread_t *t = thread_create(&kernel_process, _entry_a);
    KT_ASSERT_NONNULL(t);

    KT_CHECK_EQ((int)t->state, (int)THREAD_CREATED);
    thread_ready_on(t, 0);
    KT_CHECK_EQ((int)t->state, (int)THREAD_READY);
}

KTEST("sched-ready-on-state", "scheduler",
      "thread_ready_on transitions thread state CREATED -> READY",
      KT_FLAG_NONE, test_thread_ready_on_state);

/* ------------------------------------------------------------------ */

static void test_thread_ready_on_queue_nonempty(ktest_ctx_t *ctx) {
    scheduler_init();
    thread_t *t = thread_create(&kernel_process, _entry_a);
    KT_ASSERT_NONNULL(t);

    KT_CHECK(list_empty(&cpu_core_local[0].run_queue));
    thread_ready_on(t, 0);
    KT_CHECK(!list_empty(&cpu_core_local[0].run_queue));
}

KTEST("sched-ready-on-queue-nonempty", "scheduler",
      "after thread_ready_on(t, 0), core 0 run queue is non-empty",
      KT_FLAG_NONE, test_thread_ready_on_queue_nonempty);

/* ------------------------------------------------------------------ */

static void test_thread_ready_on_fifo_order(ktest_ctx_t *ctx) {
    scheduler_init();
    thread_t *a = thread_create(&kernel_process, _entry_a);
    thread_t *b = thread_create(&kernel_process, _entry_b);
    thread_t *c = thread_create(&kernel_process, _entry_c);
    KT_ASSERT_NONNULL(a);
    KT_ASSERT_NONNULL(b);
    KT_ASSERT_NONNULL(c);

    thread_ready_on(a, 0);
    thread_ready_on(b, 0);
    thread_ready_on(c, 0);

    /* The run queue is FIFO (push_back / pop_front).  First in = first out. */
    bool irq = spinlock_acquire(&cpu_core_local[0].run_queue_lock);
    list_node_t *n1 = list_pop_front(&cpu_core_local[0].run_queue);
    list_node_t *n2 = list_pop_front(&cpu_core_local[0].run_queue);
    list_node_t *n3 = list_pop_front(&cpu_core_local[0].run_queue);
    spinlock_release(&cpu_core_local[0].run_queue_lock, irq);

    KT_ASSERT(n1 != NULL);
    KT_ASSERT(n2 != NULL);
    KT_ASSERT(n3 != NULL);

    thread_t *t1 = list_entry(n1, thread_t, list_node);
    thread_t *t2 = list_entry(n2, thread_t, list_node);
    thread_t *t3 = list_entry(n3, thread_t, list_node);

    KT_CHECK(t1 == a);
    KT_CHECK(t2 == b);
    KT_CHECK(t3 == c);
}

KTEST("sched-ready-on-fifo", "scheduler",
      "threads dequeue in FIFO order matching the enqueue sequence",
      KT_FLAG_NONE, test_thread_ready_on_fifo_order);

/* ------------------------------------------------------------------ */

static void test_thread_ready_on_per_core_isolation(ktest_ctx_t *ctx) {
    if (coreCount < 2) { ctx->result = KT_SKIP; return; }

    scheduler_init();
    thread_t *ta = thread_create(&kernel_process, _entry_a);
    thread_t *tb = thread_create(&kernel_process, _entry_b);
    KT_ASSERT_NONNULL(ta);
    KT_ASSERT_NONNULL(tb);

    thread_ready_on(ta, 0);
    thread_ready_on(tb, 1);

    KT_CHECK(!list_empty(&cpu_core_local[0].run_queue));
    KT_CHECK(!list_empty(&cpu_core_local[1].run_queue));

    /* Core 0 must only see ta; core 1 must only see tb */
    bool irq0 = spinlock_acquire(&cpu_core_local[0].run_queue_lock);
    list_node_t *n0 = list_pop_front(&cpu_core_local[0].run_queue);
    spinlock_release(&cpu_core_local[0].run_queue_lock, irq0);

    bool irq1 = spinlock_acquire(&cpu_core_local[1].run_queue_lock);
    list_node_t *n1 = list_pop_front(&cpu_core_local[1].run_queue);
    spinlock_release(&cpu_core_local[1].run_queue_lock, irq1);

    KT_ASSERT(n0 != NULL);
    KT_ASSERT(n1 != NULL);
    KT_CHECK(list_entry(n0, thread_t, list_node) == ta);
    KT_CHECK(list_entry(n1, thread_t, list_node) == tb);
}

KTEST("sched-ready-on-core-isolation", "scheduler",
      "threads enqueued on different cores stay isolated (requires >= 2 cores)",
      KT_FLAG_SMP, test_thread_ready_on_per_core_isolation);

/* ------------------------------------------------------------------ */

static void test_thread_ready_on_count(ktest_ctx_t *ctx) {
    #define NTH 10
    scheduler_init();

    for (int i = 0; i < NTH; i++) {
        thread_t *t = thread_create(&kernel_process, _entry_a);
        KT_ASSERT_NONNULL(t);
        thread_ready_on(t, 0);
    }

    /* Count nodes in core 0's run queue */
    int count = 0;
    bool irq = spinlock_acquire(&cpu_core_local[0].run_queue_lock);
    list_node_t *pos;
    list_for_each(pos, &cpu_core_local[0].run_queue) {
        count++;
    }
    spinlock_release(&cpu_core_local[0].run_queue_lock, irq);

    KT_CHECK_EQ(count, NTH);
    #undef NTH
}

KTEST("sched-ready-on-count", "scheduler",
      "10 threads ready'd on core 0 produce exactly 10 nodes in the run queue",
      KT_FLAG_NONE, test_thread_ready_on_count);

/* ------------------------------------------------------------------ */

static void test_thread_ready_from_blocked(ktest_ctx_t *ctx) {
    scheduler_init();
    thread_t *t = thread_create(&kernel_process, _entry_a);
    KT_ASSERT_NONNULL(t);

    /* Manually force BLOCKED state (as if the thread had been waiting) */
    bool tl = spinlock_acquire(&t->lock);
    t->state = THREAD_BLOCKED;
    spinlock_release(&t->lock, tl);

    /* thread_ready_on accepts CREATED or BLOCKED - BLOCKED must work too */
    thread_ready_on(t, 0);
    KT_CHECK_EQ((int)t->state, (int)THREAD_READY);
    KT_CHECK(!list_empty(&cpu_core_local[0].run_queue));
}

KTEST("sched-ready-from-blocked", "scheduler",
      "thread_ready_on transitions a BLOCKED thread to READY",
      KT_FLAG_NONE, test_thread_ready_from_blocked);

/* ================================================================== */
/* Group 9: wait queue waiter management                               */
/* ================================================================== */

static void test_waitq_add_waiter(ktest_ctx_t *ctx) {
    wait_queue_t wq;
    wait_queue_init(&wq);

    thread_t *t = thread_create(&kernel_process, _entry_a);
    KT_ASSERT_NONNULL(t);

    /* Manually enqueue thread onto the wait queue (simulating thread_block) */
    bool irq = spinlock_acquire(&wq.lock);
    list_push_back(&wq.waiters, &t->wq_node);
    spinlock_release(&wq.lock, irq);

    KT_CHECK(!list_empty(&wq.waiters));
}

KTEST("sched-waitq-add-waiter", "scheduler",
      "a thread's wq_node can be appended to a wait queue's waiters list",
      KT_FLAG_NONE, test_waitq_add_waiter);

/* ------------------------------------------------------------------ */

static void test_waitq_wake_one_dequeues(ktest_ctx_t *ctx) {
    scheduler_init();

    wait_queue_t wq;
    wait_queue_init(&wq);

    thread_t *t = thread_create(&kernel_process, _entry_a);
    KT_ASSERT_NONNULL(t);

    /* Block the thread: force BLOCKED state and add to wait queue */
    bool tl = spinlock_acquire(&t->lock);
    t->state = THREAD_BLOCKED;
    spinlock_release(&t->lock, tl);

    bool ql = spinlock_acquire(&wq.lock);
    list_push_back(&wq.waiters, &t->wq_node);
    spinlock_release(&wq.lock, ql);

    KT_CHECK(!list_empty(&wq.waiters));

    /* Wake one -- must move the thread to the run queue */
    wait_queue_wake_one(&wq);

    /* Wait queue must now be empty */
    KT_CHECK(list_empty(&wq.waiters));

    /* Thread must now be READY and on core 0's run queue */
    KT_CHECK_EQ((int)t->state, (int)THREAD_READY);
    KT_CHECK(!list_empty(&cpu_core_local[0].run_queue));
}

KTEST("sched-waitq-wake-one-dequeues", "scheduler",
      "wait_queue_wake_one moves one thread from wait queue to run queue",
      KT_FLAG_NONE, test_waitq_wake_one_dequeues);

/* ------------------------------------------------------------------ */

static void test_waitq_wake_all_dequeues(ktest_ctx_t *ctx) {
    #define NW 4
    scheduler_init();

    wait_queue_t wq;
    wait_queue_init(&wq);

    thread_t *threads[NW];
    for (int i = 0; i < NW; i++) {
        threads[i] = thread_create(&kernel_process, _entry_a);
        KT_ASSERT_NONNULL(threads[i]);

        bool tl = spinlock_acquire(&threads[i]->lock);
        threads[i]->state = THREAD_BLOCKED;
        spinlock_release(&threads[i]->lock, tl);

        bool ql = spinlock_acquire(&wq.lock);
        list_push_back(&wq.waiters, &threads[i]->wq_node);
        spinlock_release(&wq.lock, ql);
    }

    /* All 4 threads are waiting */
    wait_queue_wake_all(&wq);

    /* Wait queue must now be drained */
    KT_CHECK(list_empty(&wq.waiters));

    /* Count threads now on core 0's run queue */
    int ready_count = 0;
    bool irq = spinlock_acquire(&cpu_core_local[0].run_queue_lock);
    list_node_t *pos;
    list_for_each(pos, &cpu_core_local[0].run_queue) {
        ready_count++;
    }
    spinlock_release(&cpu_core_local[0].run_queue_lock, irq);

    KT_CHECK_EQ(ready_count, NW);
    #undef NW
}

KTEST("sched-waitq-wake-all-dequeues", "scheduler",
      "wait_queue_wake_all moves all 4 waiters to the run queue",
      KT_FLAG_NONE, test_waitq_wake_all_dequeues);

/* ------------------------------------------------------------------ */

static void test_waitq_wake_one_leaves_rest(ktest_ctx_t *ctx) {
    scheduler_init();

    wait_queue_t wq;
    wait_queue_init(&wq);

    thread_t *t1 = thread_create(&kernel_process, _entry_a);
    thread_t *t2 = thread_create(&kernel_process, _entry_b);
    KT_ASSERT_NONNULL(t1);
    KT_ASSERT_NONNULL(t2);

    /* Block both */
    bool tl;
    tl = spinlock_acquire(&t1->lock); t1->state = THREAD_BLOCKED; spinlock_release(&t1->lock, tl);
    tl = spinlock_acquire(&t2->lock); t2->state = THREAD_BLOCKED; spinlock_release(&t2->lock, tl);

    bool ql = spinlock_acquire(&wq.lock);
    list_push_back(&wq.waiters, &t1->wq_node);
    list_push_back(&wq.waiters, &t2->wq_node);
    spinlock_release(&wq.lock, ql);

    /* Wake only the first one */
    wait_queue_wake_one(&wq);

    /* One waiter must remain */
    KT_CHECK(!list_empty(&wq.waiters));

    bool irq = spinlock_acquire(&wq.lock);
    list_node_t *remaining = list_pop_front(&wq.waiters);
    spinlock_release(&wq.lock, irq);

    KT_ASSERT(remaining != NULL);
    thread_t *leftover = list_entry(remaining, thread_t, wq_node);
    KT_CHECK(leftover == t2);          /* t1 was woken; t2 remains */
    KT_CHECK_EQ((int)t1->state, (int)THREAD_READY);
    KT_CHECK_EQ((int)t2->state, (int)THREAD_BLOCKED);
}

KTEST("sched-waitq-wake-one-leaves-rest", "scheduler",
      "wake_one wakes t1 and leaves t2 blocked in the wait queue",
      KT_FLAG_NONE, test_waitq_wake_one_leaves_rest);

/* ================================================================== */
/* Group 10: kernel_process used by thread_create                      */
/* ================================================================== */

static void test_thread_create_uses_kernel_process(ktest_ctx_t *ctx) {
    scheduler_init();
    thread_t *t = thread_create(&kernel_process, _entry_a);
    KT_ASSERT_NONNULL(t);
    KT_CHECK(t->parent == &kernel_process);
}

KTEST("sched-thread-kproc-parent", "scheduler",
      "thread created with &kernel_process stores the correct parent pointer",
      KT_FLAG_NONE, test_thread_create_uses_kernel_process);

#endif /* KTEST_ENABLED */
