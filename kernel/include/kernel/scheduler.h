#pragma once

/*
 * scheduler.h - execution context and scheduling foundation
 *
 * This header defines the structures that the scheduler will operate on.
 * It does NOT implement scheduling logic; it establishes the data model
 * that the scheduler implementation will consume.
 *
 * -----------------------------------------------------------------------
 * Design contract
 * -----------------------------------------------------------------------
 *
 * thread_t is the unit of scheduling.  Every schedulable execution
 * context - whether a kernel thread or a user thread - maps to one
 * thread_t.
 *
 * process_t is the unit of isolation.  It owns a virtual address space
 * (pagemap) and a set of threads.  A kernel-only process can exist with
 * a NULL pagemap if it runs entirely in the shared kernel address space.
 *
 * wait_queue_t is the blocking primitive.  A thread blocks by placing
 * itself on a wait queue; another thread or interrupt handler wakes it
 * by removing it and marking it runnable.
 *
 * -----------------------------------------------------------------------
 * What is NOT here yet
 * -----------------------------------------------------------------------
 *
 * - Scheduler algorithm (run queue policy, preemption logic)
 * - Context switch assembly stub
 * - Thread creation / teardown API
 * - Process creation / teardown API
 * - Signal delivery
 * - User-mode address space management beyond the pagemap pointer
 *
 * -----------------------------------------------------------------------
 * Concurrency rules
 * -----------------------------------------------------------------------
 *
 * thread_t.state must only be changed while holding thread_t.lock.
 * thread_t.list_node must only be manipulated while holding the lock
 *   of the containing list (run queue lock or wait queue lock).
 * process_t.threads is protected by process_t.lock.
 * wait_queue_t.waiters is protected by wait_queue_t.lock.
 *
 * Spinlocks in this subsystem are IRQ-safe: always acquired via
 * spinlock_acquire() which disables interrupts on entry.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <kernel/types.h>
#include <kernel/list.h>
#include <kernel/spinlock.h>
#include <kernel/vmm.h>

/* ------------------------------------------------------------------ */
/* Thread state machine                                                 */
/* ------------------------------------------------------------------ */

/*
 * Thread lifecycle:
 *
 *   THREAD_CREATED -> THREAD_READY -> THREAD_RUNNING
 *                            ^                |
 *                            |          THREAD_BLOCKED
 *                            -----------------
 *                       (wakeup)         (sleep/wait)
 *
 *   Any state -> THREAD_DEAD (on exit or termination)
 *
 * THREAD_CREATED: allocated and initialised, not yet on any run queue.
 * THREAD_READY:   on a run queue, eligible to be scheduled.
 * THREAD_RUNNING: currently executing on a CPU.
 * THREAD_BLOCKED: waiting on a resource; on a wait queue, not a run queue.
 * THREAD_DEAD:    finished; resources not yet reclaimed.
 */
typedef enum {
    THREAD_CREATED = 0,
    THREAD_READY,
    THREAD_RUNNING,
    THREAD_BLOCKED,
    THREAD_DEAD,
} thread_state_t;

/* ------------------------------------------------------------------ */
/* Callee-saved register context (for cooperative context switch)       */
/* ------------------------------------------------------------------ */

/*
 * context_regs_t: the register state saved across a voluntary context
 * switch.  Only callee-saved registers need to be preserved; the C ABI
 * guarantees all other registers are caller-saved.
 *
 * The context switch stub (to be implemented in asm) will:
 *   1. Save rip, rsp, rbx, rbp, r12-r15 of the outgoing thread here.
 *   2. Load the same fields from the incoming thread's context_regs_t.
 *
 * rip is the address to return to after the switch (the instruction
 * after the switch call, or the thread entry point for a new thread).
 * rsp points to the top of the thread's kernel stack frame.
 */
typedef struct {
    uintptr_t rip;   /* return / resume address                         */
    uintptr_t rsp;   /* kernel stack pointer at point of switch         */
    uintptr_t rbx;
    uintptr_t rbp;
    uintptr_t r12;
    uintptr_t r13;
    uintptr_t r14;
    uintptr_t r15;
} context_regs_t;

/* ------------------------------------------------------------------ */
/* Kernel stack                                                         */
/* ------------------------------------------------------------------ */

#define KSTACK_SIZE (16u * 1024u)  /* 16 KiB per kernel thread stack    */

/*
 * kstack_alloc - allocate a kernel stack and return the virtual address
 * of its TOP (the address the stack pointer should start at).
 *
 * The bottom of the stack is at (top - KSTACK_SIZE).  The region is
 * backed by PMM frames mapped into mmu_kernel_pagemap.
 *
 * Returns 0 on allocation failure.
 * Caller must not free individual pages; use kstack_free().
 */
uintptr_t kstack_alloc(void);

/*
 * kstack_free - release a kernel stack previously obtained from
 * kstack_alloc().  Pass the TOP address returned by kstack_alloc().
 */
void kstack_free(uintptr_t stack_top);

/* ------------------------------------------------------------------ */
/* Forward declarations                                                 */
/* ------------------------------------------------------------------ */

struct thread;
struct process;

/* ------------------------------------------------------------------ */
/* Wait queue                                                           */
/* ------------------------------------------------------------------ */

/*
 * wait_queue_t - minimal blocking primitive.
 *
 * A thread blocks by:
 *   1. Setting its state to THREAD_BLOCKED.
 *   2. Adding its wq_node to a wait_queue_t under that queue's lock.
 *   3. Yielding to the scheduler.
 *
 * A waking entity:
 *   1. Acquires the wait_queue_t lock.
 *   2. Pops threads from the waiters list.
 *   3. Sets each thread's state back to THREAD_READY.
 *   4. Enqueues it on the appropriate run queue.
 *   5. Releases the lock.
 *
 * wait_queue_wake_one() and wait_queue_wake_all() are declared here as
 * forward declarations; the scheduler implements them once it exists.
 */
typedef struct {
    spinlock_t  lock;
    list_head_t waiters;   /* list of thread_t via thread_t.wq_node       */
} wait_queue_t;

#define WAIT_QUEUE_INIT(wq) \
    { .lock = SPINLOCK_ZERO, .waiters = LIST_HEAD_INIT((wq).waiters) }

static inline void wait_queue_init(wait_queue_t *wq) {
    wq->lock = (spinlock_t)SPINLOCK_ZERO;
    list_head_init(&wq->waiters);
}

/* ------------------------------------------------------------------ */
/* Thread structure                                                     */
/* ------------------------------------------------------------------ */

/*
 * thread_t - the schedulable execution unit.
 *
 * Lifetime:
 *   thread_create() -> THREAD_CREATED
 *   thread_ready()  -> THREAD_READY (enqueued on a run queue)
 *   [scheduler picks it] -> THREAD_RUNNING
 *   thread_block()  -> THREAD_BLOCKED (placed on a wait_queue)
 *   thread_exit()   -> THREAD_DEAD
 *   thread_destroy() -> memory freed
 *
 * Ownership:
 *   - thread_t is owned by its parent process_t (via threads list).
 *   - The kernel stack (kstack_top) is owned by the thread; freed in
 *     thread_destroy().
 *   - parent pointer is valid while the process is alive.  The process
 *     must outlive all its threads.
 *
 * lock protects: state, list_node, wq_node, and any field that may be
 * modified from another CPU.
 */
typedef struct thread {
    tid_t           tid;            /* unique thread identifier            */
    struct process *parent;         /* owning process; NULL for kernel-only */

    thread_state_t  state;          /* current lifecycle state             */
    spinlock_t      lock;           /* protects state + list linkage       */

    context_regs_t  context;        /* saved register state (on switch)    */

    uintptr_t       kstack_top;     /* virtual address of kernel stack top */
    uintptr_t       kstack_bottom;  /* = kstack_top - KSTACK_SIZE          */

    list_node_t     list_node;      /* linkage for run queue               */
    list_node_t     wq_node;        /* linkage for wait queue              */
} thread_t;

/* ------------------------------------------------------------------ */
/* Process structure                                                    */
/* ------------------------------------------------------------------ */

/*
 * process_t - the isolation boundary.
 *
 * A process owns a virtual address space (pagemap) and a set of threads.
 * Kernel worker threads that run in the shared kernel address space may
 * be attached to a "kernel process" with a NULL pagemap.
 *
 * lock protects: threads list, pid, name changes.
 */
typedef struct process {
    pid_t        pid;           /* unique process identifier               */
    const char  *name;          /* human-readable name (not owned here)    */

    pagemap_t   *pagemap;       /* address space; NULL for kernel processes */
    spinlock_t   lock;

    list_head_t  threads;       /* thread_t via thread_t.list_node         */
    uint32_t     thread_count;
} process_t;

/* ------------------------------------------------------------------ */
/* Global kernel process                                                */
/* ------------------------------------------------------------------ */

/*
 * kernel_process - the process all kernel threads are attached to.
 * Defined in scheduler.c; available after scheduler_init().
 */
extern process_t kernel_process;

/* ------------------------------------------------------------------ */
/* Scheduler API                                                        */
/* ------------------------------------------------------------------ */

/*
 * scheduler_init - one-time global scheduler setup.
 *
 * Initialises the per-core run queues in cpu_core_local[], sets up the
 * kernel process, and installs the LAPIC tick hook for preemption.
 * Must be called after smp_init() (needs cpu_core_local[] allocated and
 * coreCount set) and after lapic_timer_calibrate() on all cores.
 * Must be called from the BSP before any thread is created.
 */
void scheduler_init(void);

/*
 * thread_create - allocate and initialise a new kernel thread.
 *
 * Allocates a thread_t and a kernel stack.  The thread starts in state
 * THREAD_CREATED; it is not placed on any run queue.  Call thread_ready()
 * or thread_ready_on() to make it schedulable.
 *
 * entry: function the thread will execute; if it returns, thread_exit()
 *        is called automatically.
 * parent: owning process; use &kernel_process for kernel threads.
 *
 * Returns NULL on allocation failure.  Caller owns the thread_t and must
 * call thread_destroy() when the thread has reached THREAD_DEAD.
 */
thread_t *thread_create(process_t *parent, void (*entry)(void));

/*
 * thread_ready - transition a THREAD_CREATED or THREAD_BLOCKED thread to
 * THREAD_READY and enqueue it on the CURRENT core's run queue.
 *
 * Must be called from normal context (not while holding run_queue_lock).
 */
void thread_ready(thread_t *t);

/*
 * thread_ready_on - like thread_ready() but targets a specific core.
 *
 * core_id must be < coreCount.  Safe to call from the BSP during init
 * to pre-populate remote cores' run queues before those cores enter the
 * scheduler.
 */
void thread_ready_on(thread_t *t, cpu_id_t core_id);

/*
 * scheduler_enter - start executing threads on the calling core.
 *
 * Pops the first THREAD_READY thread from the local run queue, marks it
 * THREAD_RUNNING, and performs the initial context switch into it.
 * Does NOT return.  The core's run queue must be non-empty.
 *
 * Calling from the BSP kicks the BSP into the scheduler.  APs enter
 * the scheduler automatically the first time their LAPIC tick fires
 * after their run queue becomes non-empty.
 */
void scheduler_enter(void) __attribute__((noreturn));

/*
 * schedule - pick the next thread and context-switch to it.
 *
 * Called periodically by the LAPIC tick hook (preemption) and may also
 * be called voluntarily (yield).  If the run queue is empty, returns
 * immediately and the current thread continues.
 *
 * Must NOT be called while holding any lock that is inner to
 * run_queue_lock in the lock hierarchy (e.g. wait_queue_t.lock).
 */
void schedule(void);

/*
 * thread_exit - terminate the calling thread.
 *
 * Marks the current thread THREAD_DEAD, removes it from the scheduler,
 * and switches to the next available thread.  If no thread is available,
 * the core enters an idle halt loop.  Does NOT return.
 *
 * Memory for the thread_t and its kernel stack is NOT freed here; a
 * reaper mechanism (not yet implemented) is responsible for cleanup.
 */
void thread_exit(void) __attribute__((noreturn));

/*
 * wait_queue_wake_one - wake the first thread sleeping on wq.
 *
 * Removes the first waiter, transitions it to THREAD_READY, and enqueues
 * it on the current core's run queue.
 */
void wait_queue_wake_one(wait_queue_t *wq);

/*
 * thread_block - block the calling thread on a wait queue.
 *
 * Transitions the calling thread from THREAD_RUNNING to THREAD_BLOCKED,
 * appends its wq_node to wq->waiters, then calls schedule() to switch to
 * the next available thread.  Does not return until another thread calls
 * wait_queue_wake_one() or wait_queue_wake_all() on the same queue.
 *
 * The caller is responsible for holding any external lock needed to prevent
 * the wakeup from occurring before the block completes (check-before-sleep
 * pattern).
 *
 * Must not be called from interrupt context or while holding run_queue_lock.
 * Must not be called on the idle thread.
 */
void thread_block(wait_queue_t *wq);

/*
 * wait_queue_wake_all - wake all threads sleeping on wq.
 */
void wait_queue_wake_all(wait_queue_t *wq);
