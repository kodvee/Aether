# Aether Kernel Architecture

This document describes the kernel as it **currently exists** in code.  
It is the authoritative reference for invariants, ownership rules, concurrency contracts, and scheduler prerequisites.  
The design philosophy and long-term goals live in [README.md](../README.md); this document maps that philosophy to actual implementation.

---

## Table of Contents

1. [System Map](#1-system-map)
2. [Address Space Layout](#2-address-space-layout)
3. [Subsystem Dependency Order](#3-subsystem-dependency-order)
4. [Type System](#4-type-system)
5. [Memory Subsystem](#5-memory-subsystem)
6. [Concurrency Primitives](#6-concurrency-primitives)
7. [Container Primitives](#7-container-primitives)
8. [Hardware Abstraction Layer](#8-hardware-abstraction-layer)
9. [SMP and Per-CPU Infrastructure](#9-smp-and-per-cpu-infrastructure)
10. [Interrupt Handling](#10-interrupt-handling)
11. [Time and Timer Foundation](#11-time-and-timer-foundation)
12. [Execution Context Model](#12-execution-context-model)
13. [Logging and Observability](#13-logging-and-observability)
14. [Test Framework](#14-test-framework)
15. [Concurrency Rules](#15-concurrency-rules)
16. [Memory Ownership Rules](#16-memory-ownership-rules)
17. [Scheduler Prerequisites](#17-scheduler-prerequisites)
18. [What Is Not Yet Implemented](#18-what-is-not-yet-implemented)

---

## 1. System Map

| Subsystem | Location | Status | Header |
|-----------|----------|--------|--------|
| Type system | `include/kernel/types.h` | Stable | `<kernel/types.h>` |
| PMM | `memory/pmm.c` | Stable | `<kernel/pmm.h>` |
| VMM | `memory/vmm.c` | Stable | `<kernel/vmm.h>` |
| Slab allocator | `memory/slab.c` | Stable | `<kernel/mmu.h>` |
| Kernel stack alloc | `memory/kstack.c` | Stable | `<kernel/scheduler.h>` |
| Spinlock | `lib/spinlock.c` | Stable | `<kernel/spinlock.h>` |
| Intrusive list | `include/kernel/list.h` | Stable | `<kernel/list.h>` |
| Allocating list | `lib/dlist.c` | Stable | `<kernel/dlist.h>` |
| GDT | `sys/gdt.c` | Stable | `<kernel/gdt.h>` |
| IDT / ISR dispatch | `sys/idt.c` + `sys/int.S` | Stable | `<kernel/int.h>` |
| CPU feature detection | `sys/cpu.c` | Stable | `<kernel/cpufeature.h>` |
| Per-CPU infrastructure | `include/kernel/cpu.h` | Stable | `<kernel/cpu.h>` |
| LAPIC | `time/lapic.c` | Stable | `<kernel/apic.h>` |
| IOAPIC | `sys/ioapic.c` | Stable | `<kernel/apic.h>` |
| HPET | `time/hpet.c` | Stable | `<kernel/hpet.h>` |
| SMP bringup | `sys/smp.c` | Stable | `<kernel/smp.h>` |
| ACPI parsing | `acpi/acpi.c` | Stable | `<kernel/acpi.h>` |
| ELF symbol resolution | `debug/elf.c` | Stable | `<kernel/elf.h>` |
| Panic subsystem | `debug/panic.c` | Stable | `<kernel/panic.h>` |
| Structured logging | `debug/log.c` | Stable | `<kernel/panic.h>` |
| Test framework | `test/runner.c` | Stable | `<kernel/ktest.h>` |
| Execution context model | `include/kernel/scheduler.h` | Stable | `<kernel/scheduler.h>` |
| Scheduler | `sys/scheduler.c` | Stable | `<kernel/scheduler.h>` |
| Process model | `sys/process.c` | Stable | `<kernel/scheduler.h>` |
| Syscall ABI | `sys/syscall.c`, `sys/syscalls.c` | Stable | `<kernel/syscall.h>` |
| ELF loader | `sys/elf_loader.c` | Stable | `<kernel/elf_loader.h>` |
| VFS | - | **Not implemented** | - |

---

## 2. Address Space Layout

```
0xffffffff80000000  Kernel image (.text, .rodata, .data, .bss)
                    Mapped by VMM at init with section-specific permissions:
                      .text     PTE_PRESENT (execute, no write)
                      .rodata   PTE_PRESENT | PTE_NX (read only)
                      .data     PTE_PRESENT | PTE_WRITABLE | PTE_NX

0xffff800000000000  HHDM - direct map of all tracked physical memory
                    Base stored in hhdm_offset (set once by pmm_init).
                    Formula: vaddr = paddr + hhdm_offset
                             paddr = vaddr - hhdm_offset

0x0000000000000000  User space
         to
0x00007fffffffffff
```

**Physical <-> virtual conversion rules:**
- Never store a virtual address in a `paddr_t`.
- Never add HHDM to an already-virtual address.
- `HHDM_HIGHER_HALF` (`hhdm_offset`) is read-only after `pmm_init()`.

---

## 3. Subsystem Dependency Order

Subsystems are layered. A lower layer must never call upward into a higher layer.

```
[ Hardware / CPUID / ACPI tables ]
          |
          v
[ PMM ]  <- hhdm_offset set here; no allocator yet
          |
          v
[ VMM ]  <- kernel pagemap activated; HHDM mapped
          |
          v
[ GDT ]  <- segment registers loaded
          |
          v
[ IDT ]  <- interrupt vectors installed; irqs[] allocated via slab
          |
          v
[ Slab / malloc / free ]
          |
          v
[ Per-CPU bootstrap (core_bsp) ]  <- GS register set
          |
          v
[ printf / flanterm ]
          |
          v
[ CPU feature detection / ACPI / ELF / HPET / SMP ]
          |
          v
[ ktest (test builds only) ]
          |
          v
[ Scheduler ]
          |
          v
[ Process model / Syscall ABI / ELF loader ]
          |
          v
[ VFS / Drivers ]   <- NOT YET IMPLEMENTED
```

**Consequence:** malloc is not available until after `slab_init()`. Any subsystem that calls `malloc` must be initialised after slab. The IDT allocation is the first post-slab consumer.

---

## 4. Type System

**Header:** `kernel/include/kernel/types.h`

All kernel subsystems use these canonical types to make intent explicit at the type level and prevent silent coercions between physical and virtual addresses or between identifiers.

| Type | Underlying | Meaning |
|------|-----------|---------|
| `pid_t` | `int32_t` | Process identifier; -1 = invalid |
| `tid_t` | `int32_t` | Thread identifier; -1 = invalid |
| `cpu_id_t` | `uint32_t` | Logical CPU index (0-based) |
| `vaddr_t` | `uintptr_t` | Virtual address (kernel or user) |
| `paddr_t` | `uintptr_t` | Physical frame address; never add HHDM |
| `irq_state_t` | `bool` | Snapshot of interrupt-enable flag |
| `errno_t` | `int` | Signed error code; 0 = success, negative = -(POSIX errno) |

Standard error constants (`ENOMEM`, `EINVAL`, `EPERM`, etc.) are defined in `types.h`.

---

## 5. Memory Subsystem

### 5.1 Physical Memory Manager (PMM)

**Header:** `<kernel/pmm.h>`  
**Implementation:** `memory/pmm.c`

**What it does:** Manages physical frames using a flat bitmap. Tracks all frames from address 0 up to the highest non-reserved, non-bad physical address reported by the Limine memory map.

**Invariants:**
- `pmm_init()` must be called before any other PMM function. It also sets `hhdm_offset`.
- All returned physical addresses are 4 KiB aligned and non-zero. Physical frame 0 is permanently marked used.
- `mmu_request_frame()` and `mmu_request_frames()` **panic on OOM**. They are for kernel-internal allocations where out-of-memory is a kernel bug. For user-space allocations (future), a fallible allocator must be used.
- The bitmap itself lives in the first usable physical region large enough to hold it, accessed through the HHDM. The PMM is fully operational before the VMM is active because Limine pre-maps the HHDM.
- The PMM lock (`pmm_lock`) is a spinlock; all frame operations are IRQ-safe.

**Public API:**
```c
uintptr_t mmu_request_frame(void);              // allocate 1 frame, panics on OOM
uintptr_t mmu_request_frames(uint64_t n);       // allocate n contiguous frames
void      mmu_free_frames(void *phys, uint64_t n); // release n frames
void      mmu_frame_set(uintptr_t phys);        // mark frame used
void      mmu_frame_clear(uintptr_t phys);      // mark frame free
uint64_t  clean_reclaimable_memory(void);       // reclaim bootloader pages
```

**HHDM constant:**
```c
extern uint64_t hhdm_offset;
#define HHDM_HIGHER_HALF hhdm_offset
```

### 5.2 Virtual Memory Manager (VMM)

**Header:** `<kernel/vmm.h>`  
**Implementation:** `memory/vmm.c`

**What it does:** Manages x86-64 4-level page tables. On init, builds the kernel pagemap and activates it.

**Invariants:**
- `vmm_init()` must be called after `pmm_init()`.
- After `mmu_switch_pagemap()` the kernel pagemap is active. All accesses to HHDM addresses must be to pages that were mapped before the switch.
- RESERVED memory entries smaller than 512 MiB are mapped into HHDM (to reach BIOS/ACPI regions like the RSDP). RESERVED entries larger than 512 MiB are skipped to avoid mapping large MMIO holes. BAD_MEMORY is never mapped.
- `mmu_kernel_pagemap` is read-only after `vmm_init()`; no other code may reassign the pointer.
- `mmu_map_page` and `mmu_unmap_page` are **not internally synchronized**. Callers that may race on the same pagemap must hold an external lock.

**Page table flags:**
```c
PTE_PRESENT   (1<<0)   // page is present
PTE_WRITABLE  (1<<1)   // page is writable
PTE_USER      (1<<2)   // accessible from user mode
PTE_NX        (1<<63)  // no-execute
```

**Kernel section permissions:**

| Section | Flags |
|---------|-------|
| `.text` | `PTE_PRESENT` |
| `.rodata` | `PTE_PRESENT \| PTE_NX` |
| `.data`, `.bss` | `PTE_PRESENT \| PTE_WRITABLE \| PTE_NX` |

**Note on `const` + `.data` placement:** Any object declared `const` that must be written exactly once at boot (e.g. `kelf`) must be annotated with `__attribute__((section(".data")))` to prevent placement in read-only `.rodata`. Cast-away-const is the intended write mechanism for such one-time initialisation.

### 5.3 Slab Allocator

**Header:** `<kernel/mmu.h>` (also includes `vmm.h` and `pmm.h`)  
**Implementation:** `memory/slab.c`

**What it does:** Provides `malloc` / `free` / `realloc` for general kernel heap use.

**Size classes:** 8, 16, 32, 64, 128, 256, 512, 1024, 2048 bytes.  
**Large allocations (> 2048 bytes):** served directly from the PMM with a header page.

**Invariants:**
- `slab_init()` must be called after `vmm_init()` (needs mmu_request_frame).
- `malloc(0)` returns a valid pointer (treated as `malloc(1)`).
- `free(NULL)` is a no-op.
- Each slab cache is protected by an independent per-cache spinlock. Caches do not contend on each other.
- Page identity is determined by a magic value at the page base. Double-free or bad-pointer detection panics.
- All returned pointers are at least 8-byte aligned.

### 5.4 Kernel Stack Allocator

**Header:** `<kernel/scheduler.h>`  
**Implementation:** `memory/kstack.c`

**What it does:** Allocates/frees contiguous physical-backed kernel stacks mapped into the HHDM.

**Invariants:**
- Stack size is `KSTACK_SIZE` (16 KiB = 4 pages).
- Returns the **top** of the stack (highest address). RSP should be initialized to this value.
- Stack bottom = `top - KSTACK_SIZE`.
- Frames are served from the PMM and surfaced through the pre-existing HHDM mapping. `kstack_alloc` and `kstack_free` do NO page-table manipulation; they rely on `vmm_init()` having permanently mapped all USABLE physical frames into the HHDM.
- No guard pages in the current implementation. Stack overflow corrupts adjacent memory.

---

## 6. Concurrency Primitives

### 6.1 Spinlock

**Header:** `<kernel/spinlock.h>`  
**Implementation:** `lib/spinlock.c`

**What it does:** Ticket spinlock that saves and restores the interrupt-enable flag. Correct for use in both normal context (interrupts enabled) and interrupt context.

**Type:**
```c
typedef volatile struct {
    uint16_t next_ticket;
    uint16_t now_serving;
} spinlock_t;

#define SPINLOCK_ZERO {0}
```

**API:**
```c
bool spinlock_acquire(spinlock_t *lock);            // disables IRQs, returns prior IRQ state
void spinlock_release(spinlock_t *lock, bool irq_state); // restores IRQ state
bool spinlock_is_held(const spinlock_t *lock);      // assertion helper
```

**IRQ safety contract:**
`spinlock_acquire()` calls `disable_interrupts()` before spinning. `spinlock_release()` calls `enable_interrupts()` only if interrupts were enabled at acquire time. This means:
- A spinlock acquired in normal context (interrupts enabled) will restore interrupts on release.
- A spinlock acquired in interrupt context (interrupts already disabled) will not enable interrupts on release.
- This is safe to nest: inner locks will not re-enable interrupts that the outer lock disabled.

**Usage pattern:**
```c
bool irq = spinlock_acquire(&lock);
/* critical section */
spinlock_release(&lock, irq);
```

**Hard rule:** Never sleep, block, yield, or call any function that may sleep while holding a spinlock.

### 6.2 Atomic Operations

The kernel currently uses raw GCC `__atomic_*` builtins directly. There is no abstraction wrapper layer. All atomic accesses are done at the call site with explicit memory orders.

Conventions in use:
- `__ATOMIC_RELAXED` for counter increments with no ordering requirement.
- `__ATOMIC_ACQUIRE` / `__ATOMIC_RELEASE` for lock-acquire / lock-release pairs.
- `__ATOMIC_SEQ_CST` for ownership claims (panic owner, SMP fence).

### 6.3 IRQ Control (standalone)

**Header:** `<kernel/cpu.h>`

For critical sections that need IRQ safety without mutual exclusion (no competing CPUs), use `irq_save()` / `irq_restore()` directly:

```c
irq_state_t s = irq_save();   // disables interrupts, returns prior state
/* IRQ-safe work */
irq_restore(s);                // re-enables only if they were on before
```

These are thin wrappers around `interrupt_state()` + `disable_interrupts()` / `enable_interrupts()`.

### 6.4 Memory Barriers

Memory ordering is established through `__ATOMIC_ACQUIRE` / `__ATOMIC_RELEASE` on atomic operations. There are no standalone barrier functions. Use `asm volatile("" ::: "memory")` as a compiler barrier when needed without hardware ordering.

---

## 7. Container Primitives

### 7.1 Intrusive List (`list.h`)

**Header:** `<kernel/list.h>`  
**Implementation:** inline (header-only)

**What it does:** Allocation-free doubly-linked list. The node (`list_node_t`) is embedded directly inside the owning struct. No heap allocation occurs for list operations.

**Use cases:** Scheduler run queues, wait queues, timer lists, any data structure in hot paths where allocating a wrapper node per entry is unacceptable.

**Key types:**
```c
typedef struct list_node { struct list_node *next, *prev; } list_node_t;
typedef list_node_t list_head_t;
```

**Invariant:** An empty list has `head.next == head.prev == &head` (circular sentinel). A detached node has `node.next == node.prev == &node`.

**Entry recovery:**
```c
thread_t *t = list_entry(node_ptr, thread_t, list_node);
```

**Not thread-safe.** Callers must hold the appropriate lock before any list operation.

### 7.2 Allocating List (`dlist.h`)

**Header:** `<kernel/dlist.h>`  
**Implementation:** `lib/dlist.c`

**What it does:** Non-intrusive doubly-linked list. Allocates a `dlist_node_t` wrapper per entry via `malloc`. Owns the wrapper node (the pointed-to value is not owned).

**Use cases:** ACPI table enumeration, per-process thread list (process-level, not scheduler hot path). Appropriate where allocation overhead during init is acceptable.

**Limitation:** Cannot be used allocation-free. Must not be used for scheduler run queues or wait queues.

---

## 8. Hardware Abstraction Layer

### 8.1 GDT

**Header:** `<kernel/gdt.h>`  
**Implementation:** `sys/gdt.c`

**Current state:** 5-entry flat GDT (null, kernel code 64, kernel data 64, user code 64, user data 64). All cores share this GDT.

**What is missing:** No TSS (Task State Segment) per core. A TSS provides RSP0 (the kernel stack pointer) for interrupt-driven ring-3 -> ring-0 transitions. Without it, hardware interrupts arriving while in user mode may not switch to the correct kernel stack. The current kernel reaches user space via `IRETQ` and returns via `SYSCALL`/`SYSRET`, which does not use the TSS for its stack switch. However, faults and IRQs taken in user mode are unsafe without RSP0 set. Adding a per-core TSS with a valid RSP0 is required for a fully correct user-mode implementation.

Segment selectors:
- `0x08` - kernel code
- `0x10` - kernel data
- `0x18` - user code (DPL 3)
- `0x20` - user data (DPL 3)

### 8.2 ACPI

**Header:** `<kernel/acpi.h>`  
**Implementation:** `acpi/acpi.c`

**What it does:** Parses RSDP -> RSDT/XSDT -> MADT and FADT tables at boot. Populates linked lists of LAPIC, IOAPIC, IO-APIC source override, and NMI entries.

**Invariants:**
- Must be called after slab (for `malloc`) and after `vmm_init` (RSDP address must be mapped - ensured because VMM now maps small RESERVED regions including BIOS area).
- All MADT lists (`madt_lapic`, `madt_ioapic`, etc.) are valid after `acpi_init()` and read-only thereafter.
- `lapic_address` is the **virtual** (HHDM-offset) LAPIC MMIO address after init.

### 8.3 CPU Feature Detection

**Header:** `<kernel/cpufeature.h>`  
**Implementation:** `sys/cpu.c`

Reads CPUID leaf 1 (ECX + EDX) into a single `uint64_t cpu_features`. Access via:
```c
cpu_has_feature(CPU_FEATURE_APIC)  // returns non-zero if present
```

Feature constants are defined as bit positions (bits 0-31 = ECX, bits 32-63 = EDX from CPUID leaf 1).

---

## 9. SMP and Per-CPU Infrastructure

### 9.1 Per-CPU Structure (`core_t`)

**Header:** `<kernel/cpu.h>`

```c
typedef struct core {
    uint32_t       lapic_id;        // stable after init
    cpu_id_t       cpu_id;          // logical 0-based index
    bool           bsp;             // true only on bootstrap processor
    uint32_t       interrupt_depth; // 0 = normal, >0 = inside IRQ handler
    struct thread *current_thread;  // NULL before scheduler_enter()
    list_head_t    run_queue;       // THREAD_READY threads for this core
    spinlock_t     run_queue_lock;  // protects run_queue and state changes
    struct thread *idle_thread;     // per-core idle; never on run queue
} core_t;
```

**Access:** The GS base register always points to the current CPU's `core_t`. Use `this_cpu()` to get the pointer:
```c
core_t *cpu = this_cpu();
```

**Ownership:**
- `lapic_id`, `cpu_id`, `bsp` are written once during init and then read-only.
- `interrupt_depth` and `current_thread` are owned by the CPU they describe. Cross-CPU reads are lock-free; cross-CPU writes require a lock.

**Allocation:** `smp_init()` allocates a single contiguous `cpu_core_local[]` array of `coreCount` entries from `malloc`. The array is never freed.

### 9.2 GS Register Setup

`set_gs_register(ptr)` writes `ptr` to both MSR `0xC0000101` (GS base) and `0xC0000102` (kernel GS base), then executes `SWAPGS`. The ISR stubs in `int.S` do `SWAPGS_CONDITIONAL` (only when coming from ring 3) so the kernel always has GS pointing to its own `core_t`.

The `__seg_gs` GCC address-space qualifier is used in `idt.c` to read `core_local` directly from the GS segment without going through `this_cpu()`.

### 9.3 SMP Bringup

**Implementation:** `sys/smp.c`

All cores are brought online via Limine's SMP response. Each core:
1. Reloads GDT and IDT.
2. Switches to the kernel pagemap.
3. Sets its GS register to its `core_t`.
4. Initialises its LAPIC and calibrates the LAPIC timer.
5. Enables SSE and x87.
6. Increments `initialized` counter (under `lock`), releases, then enters idle halt loop.

The BSP waits after each AP until `initialized` matches the expected count, ensuring ordered one-at-a-time bringup.

After `smp_init()` all cores are in the idle halt loop with interrupts enabled. `coreCount` is the total number of logical cores (including BSP).

### 9.4 Lock Ordering

The full kernel lock hierarchy (outermost -> innermost):

```
process_t.lock
  -> thread_t.lock
    -> wait_queue_t.lock   \  (same level; never both held simultaneously)
    -> dead_list_lock      /
      -> run_queue_lock
        -> slab cache lock
          -> pmm_lock
```

Rules:
1. **process_t.lock > thread_t.lock:** Acquiring a thread lock while holding the process lock is permitted; the reverse is not.
2. **thread_t.lock is released before touching any queue:** State transitions under `thread_t.lock` must complete before `run_queue_lock` or `wait_queue_t.lock` is acquired.
3. **wait_queue_t.lock and dead_list_lock are independent** at the same level; they are never held simultaneously.
4. **run_queue_lock is innermost among scheduler locks:** No other scheduler lock may be acquired while `run_queue_lock` is held.
5. **PMM lock > VMM (no VMM lock):** VMM calls PMM internally; PMM never calls VMM.
6. **Per-cache slab locks are independent:** no nested slab lock acquisition.

---

## 10. Interrupt Handling

**Header:** `<kernel/int.h>`  
**ISR stubs:** `sys/int.S`  
**Dispatch:** `sys/idt.c`

### 10.1 IDT

256-entry IDT shared by all cores (all cores load the same `idtp`). Vectors 0-31 are CPU exceptions. Vectors 32-254 are dynamically allocated hardware/software IRQs. Vector 255 is the SMP halt IPI (used by the panic subsystem to freeze all APs).

### 10.2 ISR Stubs

Each vector has a hand-written stub in `int.S` that:
1. Pushes a dummy error code (if the CPU doesn't push one automatically).
2. Pushes the vector number.
3. Jumps to the common `isr_common` handler.

`isr_common` saves the full register context as `struct regs`, conditionally executes `SWAPGS`, and calls `isr_handler(struct regs *)`. On return, it restores registers and executes `IRETQ`.

### 10.3 Register Save Structure

```c
struct regs {
    uintptr_t cr2, gs, fs, es, ds;
    uintptr_t r15, r14, r13, r12, r11, r10, r9, r8;
    uintptr_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uintptr_t int_no, err_code;   // pushed by stub
    uintptr_t rip, cs, rflags, rsp, ss;  // pushed by CPU
};
```

### 10.4 Dynamic IRQ Allocation

```c
uint8_t idt_allocate(void);                     // returns next free vector (32-254)
void    irq_install(irq_t handler, int vector); // install handler for vector
irq_t   irq_get(int vector);                    // query installed handler
void    irq_uninstall(int vector);              // remove handler
```

`irq_install` / `irq_get` / `irq_uninstall` are **not interrupt-safe** and must be called from normal context only. `idt_allocate` is spinlock-protected and safe from any context.

### 10.5 Exception Dispatch

CPU exceptions (vectors 0-30) are dispatched in `isr_handler()`. Kernel-mode exceptions call the appropriate panic macro. User-mode exceptions currently drop the exception with a TODO comment - signal delivery to the thread is not implemented.

### 10.6 Interrupt Context Rules

- `interrupt_depth` in `core_t` is **not yet incremented** by the ISR stubs. The field exists for future use. Currently there is no runtime way to assert "we are in interrupt context" - this is a known gap.
- Interrupt handlers must not block, sleep, or acquire sleeping locks (mutexes, semaphores - none of which exist yet).
- Interrupt handlers may acquire spinlocks because `spinlock_acquire` disables interrupts, preventing re-entry on the same core.

---

## 11. Time and Timer Foundation

### 11.1 HPET

**Header:** `<kernel/hpet.h>`  
**Implementation:** `time/hpet.c`

The HPET is the kernel's **reference time source**. It is used to calibrate the LAPIC timer and is available as a blocking delay primitive.

**Invariants:**
- `hpet_init()` must be called after `acpi_init()`.
- HPET is mapped into the kernel pagemap at init via `mmu_map_page`.
- `hpet_sleep(ns)` busy-waits (polling the counter). It is **not usable after the scheduler exists** for long sleeps - use `hpet_sleep` only in init paths.
- `hpet_initialized` is set to true after successful init; checked before use.
- The tick period in femtoseconds is stored in `hpetTickPeriod`.

**Available functions:**
```c
void     hpet_sleep(uint64_t ns);       // blocking busy-wait
void     hpet_reset_counter(void);
uint64_t hpet_timer_since(void);        // nanoseconds since HPET start
```

### 11.2 LAPIC Timer

**Header:** `<kernel/apic.h>`  
**Implementation:** `time/lapic.c`

The LAPIC timer drives per-core periodic interrupts. Each core calibrates its own timer against the HPET during bringup.

**Calibration:** `lapic_timer_calibrate(ns)` uses `hpet_sleep(ns)` to measure LAPIC ticks, then programs the timer in periodic mode on vector 32.

**Tick accounting:** `kernel_ticks` increments by `ticksIn10ms` on each LAPIC interrupt (every ~10 ms by default). This is a **global** counter with no synchronisation - it is read-only useful for rough timing, not for precise per-thread accounting.

**Scheduler integration:** The LAPIC timer IRQ (vector 32) drives preemption. After `scheduler_init()`, the caller installs the tick hook via `lapic_set_tick_hook(schedule)`. On each tick, EOI is sent and then `schedule()` is called to preempt the current thread if a higher-priority thread is waiting. The BSP installs the hook after all AP run queues are populated, ensuring no AP fires before it has work.

```c
uint64_t get_kernel_ticks(void);        // rough tick count since LAPIC init
uint64_t lapic_get_frequency(void);     // LAPIC ticks per second
```

### 11.3 Monotonic Time (Gap)

There is currently **no monotonic wall-clock API**. `hpet_timer_since()` provides nanoseconds since HPET init but is not authoritative. A future `ktime_get()` / `ktime_ns()` API should wrap HPET or TSC into a stable monotonic source.

---

## 12. Execution Context Model

The scheduler is fully implemented in `sys/scheduler.c`. This section documents the live data model and runtime behavior.

### 12.1 Thread Structure (`thread_t`)

**Header:** `<kernel/scheduler.h>`

```c
typedef struct thread {
    tid_t           tid;
    struct process *parent;
    thread_state_t  state;
    spinlock_t      lock;
    context_regs_t  context;
    uintptr_t       kstack_top;
    uintptr_t       kstack_bottom;
    list_node_t     list_node;   // run queue OR dead list linkage (mutually exclusive)
    list_node_t     wq_node;     // wait queue linkage
} thread_t;
```

`list_node` is used for the run queue when `state == THREAD_READY`, and for `dead_list` when `state == THREAD_DEAD`. It must never be on both simultaneously.

### 12.2 Thread State Machine

```
THREAD_CREATED -> THREAD_READY -> THREAD_RUNNING
                       ^                |
                   (wake)         THREAD_BLOCKED
                       ^                |
                       +----------------+
                    (wait_queue_wake_*)

               (any state) -> THREAD_DEAD  (via thread_exit())
```

All state transitions are made while holding `thread_t.lock`, except the RUNNING -> BLOCKED transition in `thread_block()` which also requires the wait queue lock immediately after. State transitions inside `schedule()` happen under `run_queue_lock`.

### 12.3 Context Switch Register State

```c
typedef struct {
    uintptr_t rip;   // resume address (return from context_switch or thread entry)
    uintptr_t rsp;   // kernel stack pointer at switch point
    uintptr_t rbx, rbp, r12, r13, r14, r15;
} context_regs_t;
```

Only callee-saved registers are stored. The context switch stub (`sys/switch.S`) saves these from the outgoing thread and loads them for the incoming thread.

**New thread setup:** `context.rip` is set to `thread_entry_trampoline` (which does STI then calls the entry function stored in `context.r12`). `context.rsp` is the top of the kernel stack.

### 12.4 Process Structure (`process_t`)

```c
typedef struct process {
    pid_t        pid;
    const char  *name;

    pagemap_t   *pagemap;       // address space; NULL for kernel processes
    spinlock_t   lock;

    list_head_t  threads;       // thread_t via thread_t.list_node
    uint32_t     thread_count;

    // User address space
    list_head_t  vma_list;      // vma_t entries; protected by lock
    uintptr_t    mmap_base;     // watermark for next anonymous mmap()
    uintptr_t    brk;           // program break; set by ELF loader
} process_t;
```

`kernel_process` is the global kernel process. All kernel threads (idle, reaper, and any other kernel worker) are attached to it with `pagemap == NULL`.

VMA management is implemented in `sys/process.c`. Each `vma_t` records a page-aligned `[base, base+length)` region with `PROT_*` flags. `process_mmap` adds VMAs and allocates physical frames lazily (demand paging via `page_fault_handle`). `process_munmap` removes VMAs and unmaps pages. `process_mprotect` splits VMAs at range boundaries and re-maps pages with updated PTE flags.

### 12.5 Wait Queue (`wait_queue_t`)

```c
typedef struct {
    spinlock_t  lock;
    list_head_t waiters;     // thread_t via thread_t.wq_node
} wait_queue_t;
```

**Blocking:** `thread_block(wq)` transitions the caller to `THREAD_BLOCKED`, appends its `wq_node` to `wq->waiters`, then calls `schedule()`. The function returns only after a waker has transitioned the thread back to `THREAD_READY` and the scheduler has picked it up.

**Waking:** `wait_queue_wake_one(wq)` / `wait_queue_wake_all(wq)` drain waiters from the queue (under `wq->lock`) then call `thread_ready()` for each (after releasing `wq->lock`).

**Race note:** The caller of `thread_block()` is responsible for holding any external lock needed to prevent the wakeup from racing the block. The canonical pattern is: set a condition flag, then release the external lock and call `thread_block()`.

### 12.6 Idle Thread

Each core has a dedicated idle thread (`core_t.idle_thread`). It runs `enable_interrupts(); hlt;` in a tight loop. The idle thread is **never on any run queue**; `schedule()` switches to it explicitly when the run queue is empty and the current thread is not runnable (blocked or dead).

The idle thread is created by `scheduler_init()` for every core. It is not freed for the lifetime of the kernel.

### 12.7 Reaper Thread

A single global reaper thread drains `dead_list` (threads that have called `thread_exit()`), freeing their kernel stacks and `thread_t` allocations. It calls `thread_block(&reaper_wq)` when the list is empty, and is woken by `thread_exit()` via `wait_queue_wake_one(&reaper_wq)`.

The reaper thread is created by `scheduler_init()` but not enqueued until `scheduler_start_reaper()` is called (after all other per-core queues are populated, so the queues appear empty at test time). It runs on core 0 initially and floats to whichever core wakes it thereafter.

### 12.8 Kernel Stack

```c
uintptr_t kstack_alloc(void);          // returns stack top (RSP initial value)
void      kstack_free(uintptr_t top);  // releases stack
#define   KSTACK_SIZE (16u * 1024u)    // 16 KiB
```

Kernel stacks are owned by their `thread_t`. The reaper calls `kstack_free(t->kstack_top)` before freeing the `thread_t`.

### 12.9 Scheduler API Summary

```c
// Scheduler lifecycle
void      scheduler_init(void);
void      scheduler_enter(void) __noreturn;
void      schedule(void);

// Kernel thread management
thread_t *thread_create(process_t *p, void (*fn)(void));
void      thread_ready(thread_t *t);
void      thread_ready_on(thread_t *t, cpu_id_t core);
void      thread_block(wait_queue_t *wq);
void      thread_exit(void) __noreturn;
void      wait_queue_wake_one(wait_queue_t *wq);
void      wait_queue_wake_all(wait_queue_t *wq);

// User thread
thread_t *thread_create_user(process_t *proc, uintptr_t entry, uintptr_t usp);

// Process lifecycle
process_t *process_create(const char *name);
void       process_destroy(process_t *proc);

// Process address space
uintptr_t  process_mmap(process_t *proc, uintptr_t hint, size_t length, uint32_t prot);
void       process_munmap(process_t *proc, uintptr_t addr, size_t length);
errno_t    process_mprotect(process_t *proc, uintptr_t addr, size_t length, uint32_t prot);
uintptr_t  process_alloc_ustack(process_t *proc);
uintptr_t  process_ensure_page(process_t *proc, uintptr_t vaddr);
void       page_fault_handle(struct regs *r);
```

---

## 13. Logging and Observability

### 13.1 kprintf

**Header:** `<kernel/kprintf.h>`

`kprintf(fmt, ...)` writes to the flanterm framebuffer terminal. Available after `printf_init()`. Not usable before that call (pre-init output must use serial directly as the panic subsystem does).

`kprintf` is **not safe in interrupt context** in the current implementation - it does not hold a lock and the framebuffer context is not re-entrant. Using `kprintf` inside an interrupt handler is a current design gap; fix before interrupt handlers become frequent.

### 13.2 Structured Logging (`klog`)

**Header:** `<kernel/panic.h>`

```c
void klog(log_severity_t sev, const char *subsys, const char *fmt, ...);

// Shorthands:
KINFO("subsys", "fmt", ...);
KWARN("subsys", "fmt", ...);
KERROR("subsys", "fmt", ...);
KDEBUG("subsys", "fmt", ...);
```

Severity levels: `LOG_DEBUG=0`, `LOG_INFO=1`, `LOG_WARN=2`, `LOG_ERROR=3`.

`klog` calls `kprintf` internally and has the same context restrictions.

### 13.3 Panic Subsystem

**Header:** `<kernel/panic.h>`  
**Implementation:** `debug/panic.c`

The panic subsystem is completely allocation-free and spinlock-free. It uses direct flanterm writes and polling serial output (COM1).

**Entry points (use macros, not `_kpanic_impl` directly):**
```c
PANIC(msg)                          // generic, no register context
SUBSYS_PANIC(subsys, msg)           // tagged panic
KERNEL_ASSERT(cond)                 // runtime assertion
KERNEL_BUG(msg)                     // unreachable code reached
EXCEPTION_PANIC(subsys, msg, regs)  // CPU exception with register dump
PAGE_FAULT_PANIC(regs)              // page fault with CR2 decode
DOUBLE_FAULT_PANIC(regs)            // double fault
```

**SMP behaviour:** On entry, the panicking CPU broadcasts vector 255 IPI to all other cores (which execute `cli; hlt`). Only one CPU renders the diagnostic output.

**Depth gating:**
- Depth 1: full diagnostic to framebuffer + serial.
- Depth 2 (nested panic): serial only, minimal output.
- Depth >= 3: immediate halt, system too broken to render.

**Safe in:** interrupt context, panic context, before `printf_init()`, before `slab_init()`.  
**Not safe in:** nothing - this is the path of last resort.

### 13.4 Stack Trace

**Header:** `<kernel/stacktrace.h>`  
**Implementation:** `debug/stacktrace.c`

Requires the kernel compiled with `-fno-omit-frame-pointer` (enforced in the Makefile). Walks the frame chain from an RBP value. Resolves symbols through `kelf` if ELF init has completed.

All functions are allocation-free and safe in interrupt and panic context.

---

## 14. Test Framework

**Header:** `<kernel/ktest.h>`  
**Implementation:** `test/runner.c`

**Registration:**
```c
KTEST("name", "subsystem", "description", KT_FLAG_CRITICAL, test_fn);
```

Tests are placed in the `.ktest` linker section and auto-discovered at boot without a central registry.

**Expected-panic testing:**
```c
KT_EXPECT_PANIC(ctx, "substring") {
    KERNEL_ASSERT(false);
}
KT_EXPECT_PANIC_VERIFY(ctx);
```

**Build flags:** Compiled separately with `-DKTEST_ENABLED`. The normal kernel build is unaffected.

**CI integration:** The test runner writes a result code to QEMU's `isa-debug-exit` device (exit code 1 = all passed).

**Currently registered tests:** 186 tests across `memory`, `slab`, `vmm`, `list`, `dlist`, `process`, `cpu`, `hpet`, `types`, `smp`, `panic`, `spinlock`, `scheduler`, and `elf` subsystems.

---

## 15. Concurrency Rules

These are hard invariants. Violation is a kernel bug, not a trade-off.

**1. No sleeping while holding a spinlock.**  
Any code that may yield, block, or call the scheduler must not hold a spinlock. If you need to hold a resource lock and sleep, that resource needs a mutex or semaphore - neither of which exists yet. This means the current kernel has no lock that permits sleeping.

**2. No blocking in interrupt context.**  
Interrupt handlers run on the interrupted thread's stack. They must not block, call malloc, or call any function that may sleep.

**3. Lock ordering must be globally consistent.**  
When multiple locks must be held simultaneously, they must always be acquired in the same documented order. The current lock hierarchy is shallow; it must be updated here as new locks are added.

**Current lock ordering (outermost -> innermost):**
```
process_t.lock
  -> thread_t.lock
    -> wait_queue_t.lock   \  (same level; never both held simultaneously)
    -> dead_list_lock      /
      -> run_queue_lock
        -> slab cache lock
          -> pmm_lock
```

A CPU must never acquire a lock that is higher in this order while holding a lower one. `wait_queue_t.lock` and `dead_list_lock` are at the same level and are never held simultaneously. Acquiring the same lock level on two different objects requires a consistent tie-breaking rule (e.g., acquire by address order).

**4. Critical sections must be minimal.**  
Spinlocks must be held for the shortest possible duration. Work that does not require mutual exclusion must be moved outside the locked region.

**5. All shared state must have a synchronisation owner.**  
Every global or shared data structure must have a single designated lock. Implicit protection is not acceptable.

**6. Cross-CPU access to `core_t`.**  
Reading another CPU's `current_thread` is permitted lock-free for approximate checks. Writing another CPU's `core_t` fields requires a lock or IPI coordination. The panicking CPU sets `g_panic_owner` with `__ATOMIC_SEQ_CST` as an example of the correct cross-CPU coordination pattern.

---

## 16. Memory Ownership Rules

**1. Allocations have exactly one owner.**  
When ownership is transferred across a function boundary, the transfer is explicit: document it at the call site or in the header contract.

**2. Shared objects use reference counting.**  
Objects accessible from multiple contexts with non-trivial lifetimes must use reference counting. The process model is implemented; VFS file descriptors and inodes will be the first consumers of a formal refcount API.

**3. No hidden allocations.**  
A function that allocates memory internally and returns a pointer without documenting it in its interface creates implicit ownership. The kernel currently has several violations of this rule in ACPI (populating global lists without documented cleanup paths). These are acceptable at init time but the pattern must not spread to runtime paths.

**4. Every allocation has a cleanup path.**  
Even if cleanup is never called in practice, it must be designed. Exceptions: allocations made once at boot that live for the kernel lifetime (kernel pagemap, cpu_core_local, irqs[], etc.) do not need a cleanup path, but must be documented as permanent.

**5. Reclaimed memory is gone.**  
After `clean_reclaimable_memory()`, Limine pages (bootloader data, ACPI tables in reclaimable regions) are invalid. Data needed beyond that point must be copied to the heap before reclaim. `elf_init()` does this correctly for the kernel ELF image.

**6. Kernel stack ownership.**  
A `thread_t` owns its `kstack_top` allocation. The reaper thread calls `kstack_free(t->kstack_top)` then `free(t)` for every thread that reaches `THREAD_DEAD`.

---

## 17. Scheduler Prerequisites

All prerequisites for the base scheduler are met. The scheduler is implemented and running.

| Prerequisite | Status | Notes |
|-------------|--------|-------|
| `thread_t` structure defined | done | `scheduler.h` |
| `process_t` structure defined | done | `scheduler.h` |
| `thread_state_t` enum | done | `scheduler.h` |
| `context_regs_t` callee-save layout | done | `scheduler.h` |
| `wait_queue_t` type | done | `scheduler.h` |
| `kstack_alloc()` / `kstack_free()` | done | `memory/kstack.c` |
| `this_cpu()` returning `core_t *` | done | `cpu.h` |
| `core_t.current_thread` field | done | `cpu.h` |
| `core_t.idle_thread` field | done | `cpu.h` |
| `core_t.run_queue` + `run_queue_lock` | done | `cpu.h` |
| `core_t.interrupt_depth` field | done | `cpu.h` (not yet incremented by ISR stubs) |
| Intrusive list for run queues | done | `list.h` |
| Spinlock with IRQ save/restore | done | `spinlock.h` |
| LAPIC timer delivering IRQ | done | vector 32, periodic |
| Per-core GS register set | done | `smp_init()` |
| Context switch assembly stub | done | `sys/switch.S` |
| `thread_create()` | done | `sys/scheduler.c` |
| `thread_ready()` / `thread_ready_on()` | done | `sys/scheduler.c` |
| `schedule()` (pick next thread) | done | `sys/scheduler.c` |
| `thread_block()` / `wait_queue_wake_*()` | done | `sys/scheduler.c` |
| `thread_exit()` + reaper | done | `sys/scheduler.c` |
| Per-core idle thread | done | `sys/scheduler.c` |
| Preemption via LAPIC tick hook | done | `lapic_set_tick_hook(schedule)` in `kernel.c` |
| TSS per core in GDT | done | `sys/gdt.c`; `tss.rsp0` updated by `schedule()` and `scheduler_enter()` |
| `interrupt_depth` incremented in ISR stubs | missing | Needed for "in interrupt context?" assertion |

---

## 18. What Is Not Yet Implemented

The following subsystems and features are explicitly absent from the current codebase.

| Feature | Notes |
|---------|-------|
| VFS / file descriptors | Blocks: open, read, fstat, dynamic linking -- design planned, implementation next |
| Signal delivery | Blocks: kill, sigaction, POSIX process control |
| `fork` / `exec` | Blocks: shell, conventional process lifecycle |
| Dynamic ELF loading | Requires PT_INTERP support and dynamic linker; only static binaries work today |
| Monotonic clock API | HPET counter readable; no clock_gettime syscall wired yet |
| `kprintf` re-entrancy / locking | Currently unsafe in interrupt context |
| Guard pages on kernel stacks | Needs VMM support for intentionally unmapped pages |
| Mutex / semaphore | thread_block() exists -- the sleeping-lock API can now be built on top |
| Per-thread CPU time accounting | Tick hook exists -- needs a per-thread counter in thread_t |
| SMP load balancing | Scheduler exists -- needs work-stealing or push policy |
| MLFQ / CFS scheduling | Round-robin is sufficient until user-process workloads justify it |
| `interrupt_depth` tracking | Field exists in core_t but ISR stubs do not increment it |
| Drivers | Driver model not designed; will follow VFS |
| Networking | Not designed |
| IPC (pipes, sockets) | Not designed; pipes will follow VFS |
