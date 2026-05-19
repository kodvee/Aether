/*
 * scheduler.c - round-robin preemptive per-core scheduler
 *
 * Each logical CPU owns a run queue (list_head_t + spinlock_t embedded in
 * core_t).  The LAPIC timer drives preemption via a tick hook installed by
 * the caller after scheduler_init().  Each core also has a dedicated idle
 * thread (never on any run queue) that runs when no runnable work exists.
 *
 * A global reaper thread (running on whatever core picks it up) drains the
 * dead_list and frees the kernel stack and thread_t for exited threads.
 *
 * Lock ordering (outermost -> innermost):
 *   process_t.lock
 *     -> thread_t.lock
 *       -> wait_queue_t.lock   \  (never both held simultaneously)
 *       -> dead_list_lock      /
 *         -> run_queue_lock
 *
 * Within schedule() the run_queue_lock protects all state mutations for
 * threads on this core's run queue.  thread_t.lock is acquired separately
 * and released before run_queue_lock is taken (never nested).
 */

#include <kernel/scheduler.h>
#include <kernel/cpu.h>
#include <kernel/apic.h>
#include <kernel/panic.h>
#include <kernel/mmu.h>
#include <kernel/kprintf.h>
#include <stdint.h>
#include <stddef.h>

/* asm stubs in sys/switch.S */
extern void context_switch(context_regs_t *outgoing, context_regs_t *incoming);
extern void context_enter(context_regs_t *incoming);
extern void thread_entry_trampoline(void);
extern void user_thread_trampoline(void);

/* Atomic ID allocators */
static _Atomic int32_t next_tid = 1;
static _Atomic int32_t next_pid = 1;

pid_t scheduler_alloc_pid(void) {
    return (pid_t)__atomic_fetch_add(&next_pid, 1, __ATOMIC_RELAXED);
}

/* Global kernel process */
process_t kernel_process;

/* ------------------------------------------------------------------ */
/* Dead list and reaper                                                 */
/* ------------------------------------------------------------------ */

static list_head_t  dead_list;
static spinlock_t   dead_list_lock;
static wait_queue_t reaper_wq;
static thread_t    *reaper_thread;

static void idle_fn(void) {
    for (;;) {
        enable_interrupts();
        asm volatile("hlt");
    }
}

static void reaper_fn(void) {
    for (;;) {
        list_head_t local;
        list_head_init(&local);

        bool irq = spinlock_acquire(&dead_list_lock);
        list_node_t *n;
        while ((n = list_pop_front(&dead_list)) != NULL)
            list_push_back(&local, n);
        spinlock_release(&dead_list_lock, irq);

        while ((n = list_pop_front(&local)) != NULL) {
            thread_t  *t    = list_entry(n, thread_t, list_node);
            process_t *proc = t->parent;
            kstack_free(t->kstack_top);
            free(t);

            if (proc) {
                bool pirq = spinlock_acquire(&proc->lock);
                bool last = (--proc->thread_count == 0);
                spinlock_release(&proc->lock, pirq);
                if (last && proc != &kernel_process)
                    process_destroy(proc);
            }
        }

        thread_block(&reaper_wq);
    }
}

/* ------------------------------------------------------------------ */
/* Scheduler init                                                       */
/* ------------------------------------------------------------------ */

void scheduler_init(void) {
    kernel_process.pid          = scheduler_alloc_pid();
    kernel_process.name         = "kernel";
    kernel_process.pagemap      = NULL;
    kernel_process.lock         = (spinlock_t)SPINLOCK_ZERO;
    list_head_init(&kernel_process.threads);
    kernel_process.thread_count = 0;
    list_head_init(&kernel_process.vma_list);
    kernel_process.mmap_base    = 0;

    list_head_init(&dead_list);
    dead_list_lock = (spinlock_t)SPINLOCK_ZERO;
    wait_queue_init(&reaper_wq);

    for (uint64_t i = 0; i < coreCount; i++) {
        list_head_init(&cpu_core_local[i].run_queue);
        cpu_core_local[i].run_queue_lock = (spinlock_t)SPINLOCK_ZERO;

        thread_t *idle = thread_create(&kernel_process, idle_fn);
        KERNEL_ASSERT(idle != NULL);
        cpu_core_local[i].idle_thread = idle;
        /* idle is never enqueued; schedule() switches to it explicitly */
    }

    /* Reaper: create but do NOT enqueue yet.  The caller must call
     * scheduler_start_reaper() once all other threads are populated so that
     * per-core run queues are empty immediately after scheduler_init(). */
    reaper_thread = thread_create(&kernel_process, reaper_fn);
    KERNEL_ASSERT(reaper_thread != NULL);

    KINFO("sched", "Initialized (%lu cores)", coreCount);
}

void scheduler_start_reaper(void) {
    KERNEL_ASSERT(reaper_thread != NULL);
    thread_ready_on(reaper_thread, 0);
}

/* ------------------------------------------------------------------ */
/* Thread lifecycle                                                     */
/* ------------------------------------------------------------------ */

thread_t *thread_create(process_t *parent, void (*entry)(void)) {
    thread_t *t = malloc(sizeof(thread_t));
    if (!t) return NULL;

    uintptr_t stack_top = kstack_alloc();
    if (!stack_top) {
        free(t);
        return NULL;
    }

    t->tid           = __atomic_fetch_add(&next_tid, 1, __ATOMIC_RELAXED);
    t->parent        = parent;
    t->state         = THREAD_CREATED;
    t->lock          = (spinlock_t)SPINLOCK_ZERO;
    t->kstack_top    = stack_top;
    t->kstack_bottom = stack_top - KSTACK_SIZE;
    t->ustack_top    = 0;           /* kernel thread; set by caller for user threads */
    list_node_init(&t->list_node);
    list_node_init(&t->wq_node);

    bool pirq = spinlock_acquire(&parent->lock);
    parent->thread_count++;
    spinlock_release(&parent->lock, pirq);

    /*
     * Initial context for first execution:
     *   rip -> thread_entry_trampoline  (enables interrupts, calls entry)
     *   rsp -> stack_top                (16-byte aligned; no return addr yet)
     *   r12 -> entry                    (trampoline reads r12 for the target)
     *
     * Callee-saved registers other than r12 are zeroed.
     */
    t->context.rip = (uintptr_t)thread_entry_trampoline;
    t->context.rsp = stack_top;
    t->context.rbx = 0;
    t->context.rbp = 0;
    t->context.r12 = (uintptr_t)entry;
    t->context.r13 = 0;
    t->context.r14 = 0;
    t->context.r15 = 0;

    return t;
}

thread_t *thread_create_user(process_t *proc, uintptr_t entry, uintptr_t usp) {
    thread_t *t = thread_create(proc, NULL);
    if (!t) return NULL;

    t->ustack_top  = usp;
    /* user_thread_trampoline reads these from the restored callee-saved regs:
     *   r12 = user entry point
     *   r13 = user RSP
     *   r14 = CR3 value (pagemap physical address = pagemap_virt - HHDM) */
    t->context.rip = (uintptr_t)user_thread_trampoline;
    t->context.r12 = entry;
    t->context.r13 = usp;
    t->context.r14 = (uintptr_t)proc->pagemap - HHDM_HIGHER_HALF;
    return t;
}

void thread_ready_on(thread_t *t, cpu_id_t core_id) {
    KERNEL_ASSERT(core_id < (cpu_id_t)coreCount);
    core_t *cpu = &cpu_core_local[core_id];

    /* State transition under thread lock; released before touching queue */
    bool tirq = spinlock_acquire(&t->lock);
    KERNEL_ASSERT(t->state == THREAD_CREATED || t->state == THREAD_BLOCKED);
    t->state = THREAD_READY;
    spinlock_release(&t->lock, tirq);

    bool rirq = spinlock_acquire(&cpu->run_queue_lock);
    list_push_back(&cpu->run_queue, &t->list_node);
    spinlock_release(&cpu->run_queue_lock, rirq);
}

void thread_ready(thread_t *t) {
    thread_ready_on(t, this_cpu()->cpu_id);
}

/* ------------------------------------------------------------------ */
/* Scheduler entry                                                      */
/* ------------------------------------------------------------------ */

void __attribute__((noreturn)) scheduler_enter(void) {
    core_t   *cpu  = this_cpu();
    thread_t *idle = cpu->idle_thread;

    bool irq = spinlock_acquire(&cpu->run_queue_lock);
    list_node_t *node = list_pop_front(&cpu->run_queue);

    if (!node) {
        /*
         * Run queue empty: LAPIC already fired and schedule() performed the
         * first context switch on this core.  Fall back to idle; the tick
         * hook drives scheduling from here.
         */
        KERNEL_ASSERT(idle != NULL);
        idle->state         = THREAD_RUNNING;
        cpu->current_thread = idle;
        cpu->tss.rsp0       = idle->kstack_top;
        cpu->syscall_ksp    = idle->kstack_top;
        spinlock_release(&cpu->run_queue_lock, false); /* keep IRQs off until trampoline sti */

        context_enter(&idle->context);
        __builtin_unreachable();
    }

    thread_t *first     = list_entry(node, thread_t, list_node);
    first->state        = THREAD_RUNNING;
    cpu->current_thread = first;
    cpu->tss.rsp0       = first->kstack_top;
    cpu->syscall_ksp    = first->kstack_top;

    spinlock_release(&cpu->run_queue_lock, false); /* keep IRQs off until trampoline sti */

    context_enter(&first->context);
    __builtin_unreachable();
}

/* ------------------------------------------------------------------ */
/* Round-robin scheduler                                                */
/* ------------------------------------------------------------------ */

void schedule(void) {
    core_t   *cpu     = this_cpu();
    thread_t *current = cpu->current_thread;
    thread_t *idle    = cpu->idle_thread;

    /* Guard: scheduler_init() hasn't run yet (no idle thread) */
    if (!idle) return;

    bool irq = spinlock_acquire(&cpu->run_queue_lock);

    bool is_idle  = (current == idle);
    bool runnable = !is_idle && current != NULL
                    && current->state == THREAD_RUNNING;

    thread_t *next = NULL;
    if (!list_empty(&cpu->run_queue)) {
        list_node_t *node = list_pop_front(&cpu->run_queue);
        next = list_entry(node, thread_t, list_node);
    }

    if (!next) {
        if (runnable || is_idle) {
            /* Nothing else ready: keep running current (or stay idle) */
            spinlock_release(&cpu->run_queue_lock, irq);
            return;
        }
        /* Current is blocked or dead with an empty queue: fall to idle */
        next = idle;
    } else if (runnable) {
        /* Re-enqueue current at back of queue (round-robin) */
        current->state = THREAD_READY;
        list_push_back(&cpu->run_queue, &current->list_node);
    }
    /* If blocked/dead and next exists: just switch, don't re-enqueue */

    next->state         = THREAD_RUNNING;
    cpu->current_thread = next;
    cpu->tss.rsp0       = next->kstack_top;   /* ring-3 -> ring-0 stack */
    cpu->syscall_ksp    = next->kstack_top;

    spinlock_release(&cpu->run_queue_lock, false); /* keep IRQs off across context switch */

    if (current != NULL) {
        context_switch(&current->context, &next->context);
        irq_restore(irq); /* restore caller's IRQ state when this thread resumes */
    } else {
        context_enter(&next->context); /* noreturn; trampoline does sti */
    }
}

/* ------------------------------------------------------------------ */
/* Thread block / exit                                                  */
/* ------------------------------------------------------------------ */

void thread_block(wait_queue_t *wq) {
    core_t   *cpu = this_cpu();
    thread_t *t   = cpu->current_thread;

    KERNEL_ASSERT(t != NULL);
    KERNEL_ASSERT(t != cpu->idle_thread);

    bool tirq = spinlock_acquire(&t->lock);
    KERNEL_ASSERT(t->state == THREAD_RUNNING);
    t->state = THREAD_BLOCKED;
    spinlock_release(&t->lock, tirq);

    bool wirq = spinlock_acquire(&wq->lock);
    list_push_back(&wq->waiters, &t->wq_node);
    spinlock_release(&wq->lock, wirq);

    schedule();
    /* Resumes here after wait_queue_wake_one/all transitions us to READY
     * and a subsequent schedule() picks us back up. */
}

void __attribute__((noreturn)) thread_exit(void) {
    core_t   *cpu   = this_cpu();
    thread_t *dying = cpu->current_thread;

    KERNEL_ASSERT(dying != NULL);
    KERNEL_ASSERT(dying != cpu->idle_thread);

    /* Mark dead under thread lock */
    bool tirq = spinlock_acquire(&dying->lock);
    dying->state = THREAD_DEAD;
    spinlock_release(&dying->lock, tirq);

    /* Hand off to reaper */
    bool dirq = spinlock_acquire(&dead_list_lock);
    list_push_back(&dead_list, &dying->list_node);
    spinlock_release(&dead_list_lock, dirq);

    /* Wake reaper so it collects dying's stack and thread_t */
    wait_queue_wake_one(&reaper_wq);

    /* Switch away; schedule() sees THREAD_DEAD and will not re-enqueue */
    schedule();

    KERNEL_BUG("thread_exit: returned from schedule()");
}

/* ------------------------------------------------------------------ */
/* Wait queue wake primitives                                           */
/* ------------------------------------------------------------------ */

void wait_queue_wake_one(wait_queue_t *wq) {
    bool irq = spinlock_acquire(&wq->lock);
    list_node_t *node = list_pop_front(&wq->waiters);
    spinlock_release(&wq->lock, irq);

    if (node) {
        thread_t *t = list_entry(node, thread_t, wq_node);
        thread_ready(t);
    }
}

void wait_queue_wake_all(wait_queue_t *wq) {
    /*
     * Drain waiters while holding wq->lock, then enqueue each thread after
     * the lock is released to avoid holding wq->lock and run_queue_lock
     * simultaneously (lock ordering violation).
     */
    list_head_t local;
    list_head_init(&local);

    bool irq = spinlock_acquire(&wq->lock);
    list_node_t *node;
    while ((node = list_pop_front(&wq->waiters)) != NULL)
        list_push_back(&local, node);
    spinlock_release(&wq->lock, irq);

    while ((node = list_pop_front(&local)) != NULL) {
        thread_t *t = list_entry(node, thread_t, wq_node);
        thread_ready(t);
    }
}
