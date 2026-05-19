# Aether

Aether is a monolithic, internally modular x86_64 SMP kernel. It is designed for long-term growth, maintainability, correctness, and deterministic behavior.

This document is the primary design reference for all architectural decisions. It defines what Aether is, how it is structured, and what rules govern its construction. New subsystems, APIs, and implementation choices must be consistent with the principles stated here.

---

## Table of Contents

1. [Design Philosophy](#1-design-philosophy)
2. [Architecture Overview](#2-architecture-overview)
3. [Subsystem Rules](#3-subsystem-rules)
4. [Concurrency and Synchronization](#4-concurrency-and-synchronization)
5. [Memory Management](#5-memory-management)
6. [Error Handling](#6-error-handling)
7. [Execution Model](#7-execution-model)
8. [Observability](#8-observability)
9. [Userspace ABI](#9-userspace-abi)
10. [Subsystem Roadmap](#10-subsystem-roadmap)
11. [Current State](#11-current-state)
12. [Building and Running](#12-building-and-running)
13. [Vendors](#13-vendors)

---

## 1. Design Philosophy

Aether is guided by a small set of principles that take precedence over implementation convenience, performance, or familiarity. These are not aspirational - they are enforced design constraints.

**Explicitness over implicit behavior.**
Functions must not silently allocate memory, acquire locks, block execution, or trigger scheduling unless explicitly documented. Every API must communicate its ownership semantics, locking expectations, execution context requirements, failure modes, and lifetime guarantees at the call site or in the header contract. Hidden side effects are design violations.

**Correctness before optimization.**
The kernel prefers correct, readable, and debuggable code over clever or premature optimizations. Performance work is acceptable only within a subsystem once its correctness is established and its behavior is observable. Global optimizations that obscure ownership or control flow are rejected.

**Determinism over emergence.**
Concurrency is designed explicitly, not incidental. Synchronization paths, interrupt behavior, scheduling decisions, and locking sequences must be predictable and analyzable. Behavior that emerges from uncontrolled interaction between subsystems is a defect.

**Illegal states must be hard to represent.**
Subsystem APIs should be designed so that valid use is the natural path and misuse requires active effort. When illegal states occur, they must be easy to detect. Invariants must be enforceable and testable.

**Debuggability is load-bearing.**
Diagnostics, testing, tracing, and validation are not auxiliary systems. They are core architectural components built alongside the subsystems they observe. A subsystem that cannot be diagnosed or tested is considered incomplete.

**Clarity of design over cleverness of implementation.**
Small, composable abstractions are preferred over large generic frameworks. A function that does one thing correctly is preferred over a function that handles many cases through layered indirection.

---

## 2. Architecture Overview

Aether is a **monolithic kernel**: all subsystems run in a single privileged address space. It is **internally modular**: each subsystem has defined boundaries, a stable interface exposed through its header, and hidden internal implementation. Subsystems communicate through explicit contracts, not shared internal state.

The kernel targets **x86_64** exclusively. SMP support is a first-class design requirement, not an afterthought. All shared state must account for concurrent access from multiple cores from the moment it is introduced.

The kernel is **Unix-influenced at the userspace boundary**. Process behavior, file abstractions, and syscall semantics follow POSIX-like conventions. This is an ABI and behavioral constraint, not an internal design constraint. The kernel's internal scheduler, memory manager, driver model, and IPC mechanisms are designed independently and are not required to resemble Linux or any other existing kernel internally.

### Address Space Layout

| Region | Range | Contents |
|--------|-------|----------|
| Higher half kernel | `0xffffffff80000000` | Kernel text, rodata, data, bss |
| Physical memory map | `0xffff800000000000` | Direct-mapped physical memory |
| User space | `0x0000000000000000` - `0x00007fffffffffff` | Per-process user mappings |

### Subsystem Dependency Order

Subsystems are layered. Higher layers may depend on lower layers; lower layers must not depend on higher layers. Cross-layer coupling must go through explicit interfaces.

```
[ hardware / ACPI / CPUID ]
        |
[ PMM -> VMM -> Slab allocator ]
        |
[ GDT / IDT / LAPIC / HPET / SMP ]
        |
[ ELF / Panic / Logging / ktest ]
        |
[ Scheduler / Process model ]
        |
[ VFS / Syscall ABI / Drivers ]        <- not yet implemented
        |
[ Userspace ]                          <- not yet implemented
```

---

## 3. Subsystem Rules

Every subsystem in Aether must satisfy these rules. They apply equally to new subsystems and modifications to existing ones.

**3.1 Clear responsibility.**
A subsystem owns one concern. If a change requires modifying two subsystems' internals simultaneously for a single logical operation, one of them has too broad or too narrow a scope.

**3.2 Stable interface.**
A subsystem's public interface is defined by its header(s) under `kernel/include/kernel/`. Internal implementation files must not be included directly by other subsystems. Internal helpers must not appear in public headers.

**3.3 Explicit ownership at the interface boundary.**
Every pointer, handle, or resource crossing a subsystem boundary must have documented ownership semantics: who allocated it, who frees it, and when it becomes invalid.

**3.4 No upward dependencies.**
Lower subsystems must not call into higher ones. A memory allocator must not call the scheduler. A timer driver must not call into the VFS. Upward coupling must be resolved through callbacks, event queues, or inversion of control - never by adding a direct include.

**3.5 Testability.**
Every subsystem must expose behavior that can be validated by the kernel test framework without running the full system. Subsystems that cannot be tested in isolation contain hidden coupling.

**3.6 Documented invariants.**
Each subsystem must define what it guarantees, what preconditions it requires, and what states it considers impossible. These invariants must be enforced with assertions or stated explicitly in the header.

---

## 4. Concurrency and Synchronization

Concurrency in Aether is designed explicitly. Every piece of shared state must have a clearly defined synchronization owner before it is introduced.

### Hard Rules

These are invariants. Violation is a kernel bug, not a tradeoff.

1. **No sleeping while holding a spinlock.** Any code path that may block, yield, or call into the scheduler must not hold a spinlock. If a subsystem needs to sleep while holding a resource lock, it requires a mutex or semaphore, not a spinlock.

2. **No blocking in interrupt context.** Interrupt handlers and code running with interrupts disabled must not block, allocate memory from a sleeping allocator, or call any function that may sleep.

3. **Lock ordering must be globally consistent.** When multiple locks must be held simultaneously, they must always be acquired in the same order throughout the entire kernel. The lock ordering must be documented. Violations cause deadlocks that are difficult to reproduce.

4. **Critical sections must be minimal.** Spinlocks must be held for the shortest possible time. Work that does not require mutual exclusion must be moved outside the locked region.

5. **All shared state must have a synchronization owner.** Every global or shared data structure must have a single designated lock, atomic operation, or access rule that protects it. Implicit or ad-hoc protection is not acceptable.

### Interrupt Safety

Functions must be annotated with their interrupt-safety expectations. The following contexts are distinct and must not be confused:

- **Normal context** - interrupts enabled, can sleep, can acquire spinlocks.
- **Spinlock context** - interrupts disabled (by spinlock acquire), cannot sleep.
- **Interrupt context** - running inside an interrupt handler, cannot sleep, cannot acquire sleeping locks.

The `spinlock_acquire()` primitive saves and disables interrupts on entry and restores them on `spinlock_release()`. This is the correct mechanism for short critical sections that must be safe across both normal and interrupt context.

### SMP

Per-core state is accessed through the GS segment base register (`core_t __seg_gs`). Global shared state must be synchronized. Kernel code must not assume it runs on a specific core unless it has explicitly pinned itself.

---

## 5. Memory Management

Memory management in Aether follows strict ownership rules. Every allocation has a defined owner, a defined lifetime, and a defined cleanup path.

### Layers

| Layer | Mechanism | Granularity | Ownership |
|-------|-----------|-------------|-----------|
| Physical | PMM bitmap | 4 KiB pages | Explicit: caller frees |
| Virtual | VMM page tables | 4 KiB pages | Tied to address space |
| Kernel heap | Slab allocator | 8 B - 2 KiB + large | Explicit: caller frees |

`malloc()` / `free()` are the general kernel heap interface. They are backed by the slab allocator for small objects and fall through to the VMM for large allocations.

### Ownership Rules

1. **Allocations have exactly one owner.** When ownership is transferred, the transfer must be explicit and documented at the call site.

2. **Shared objects use reference counting.** Any object that may be accessed from multiple contexts simultaneously and has a non-trivial lifetime must use reference counting. The object must not be freed until its reference count reaches zero.

3. **Hidden allocations are design violations.** A function that allocates memory internally and returns a pointer to it - without documenting this in its interface - creates implicit ownership. Document it, or refactor to let the caller provide the buffer.

4. **Every allocation has a cleanup path.** Allocations made during subsystem initialization must have a corresponding teardown path, even if teardown is not currently exercised. Designing without a cleanup path creates permanent resource leaks.

5. **Reclaimed memory is gone.** Limine-provided pages (kernel image, module data, ACPI tables) are valid only until `clean_reclaimable_memory()` is called. Any data needed beyond that point must be copied to the heap before reclaim.

---

## 6. Error Handling

Aether distinguishes between three categories of failure. Each has a distinct response and must not be conflated with the others.

### Programmer / Invariant Failures -> Panic

These are states that should never occur if the kernel is correct. When they occur, continued execution would produce undefined or corrupted behavior. The correct response is an immediate kernel panic with full diagnostics.

Use `KERNEL_ASSERT(cond)`, `KERNEL_BUG(msg)`, or `SUBSYS_PANIC(subsys, msg)`.

Examples: use-after-free, null pointer dereference in a path that guarantees non-null, violation of a lock ordering invariant, memory corruption detected by a slab sentinel.

### Recoverable Operational Failures -> Return Values

These are expected failure conditions that occur during normal operation. The caller must be able to detect and handle them without crashing the system.

Use return values (`NULL`, negative error codes, `bool` success flags). Subsystem APIs must document all possible failure modes.

Examples: out of memory for a user allocation, file not found, invalid syscall arguments, device not present.

### Hardware / Transient Failures -> Local Recovery

These are failures caused by hardware conditions or transient state. They should be handled as close to the hardware as possible, without propagating upward unless the condition is unrecoverable.

Examples: ECC-correctable memory errors, transient I/O timeouts, spurious interrupts.

### Panic Policy

A kernel panic must only occur when:

- A kernel invariant is demonstrably violated.
- The kernel's internal state is provably corrupted and continued execution would cause harm.
- A hardware condition makes continued operation impossible.

Normal operational failures (OOM for user memory, invalid syscall arguments, bad user pointers) must never directly produce a kernel panic. The kernel must return an error and recover.

---

## 7. Execution Model

### Current State

The kernel initializes all subsystems on the BSP (Bootstrap Processor), brings secondary processors online through LAPIC INIT/SIPI, and runs a preemptive SMP scheduler. Each core runs independent threads driven by LAPIC timer ticks. Kernel threads can block on wait queues and are cleaned up by a dedicated reaper thread.

### Intended Model

The execution model is built around **processes** and **kernel threads**.

- A **process** is the unit of isolation. It owns a virtual address space, a file descriptor table, and a set of threads. Processes are isolated from each other through the MMU.
- A **kernel thread** is the unit of scheduling. Each thread has its own kernel stack, register state, and scheduling context.
- The **scheduler** is preemptive, SMP-aware, and operates on per-core run queues. Load balancing is explicit and governed by defined policy, not emergent behavior.
- **Per-core structures** (`core_t`) hold the current thread pointer, local interrupt state, LAPIC ID, and other execution context. They are accessed lock-free through GS.

### Kernel Thread Rules

- Kernel threads may sleep, block on locks, and perform long-running operations.
- Interrupt handlers are not kernel threads. They run in interrupt context, must be short, and must not block.
- The scheduling boundary is explicit: a thread yields at defined preemption points or system call returns, not at arbitrary points in kernel code.

---

## 8. Observability

Observability is a first-class architectural concern. Every major subsystem must eventually expose diagnostics, statistics, and testability hooks.

### Panic Subsystem

The panic subsystem provides unified fatal error reporting with:

- Atomic SMP ownership (one CPU renders, all others halt).
- Depth-gated rendering: full diagnostics at depth 1, serial-only emergency at depth 2, immediate halt at depth >= 3.
- Full register dump, decoded exception info, and stack trace with resolved symbols.
- CPU info, kernel version, and build metadata.
- No dependency on `kprintf` or any spinlock - output goes directly to flanterm and COM1.

### Logging

`klog(severity, subsystem, fmt, ...)` provides structured kernel logging with severity levels `DEBUG`, `INFO`, `WARN`, `ERROR`. Shorthands: `KINFO`, `KWARN`, `KERROR`, `KDEBUG`.

### Test Framework

The kernel includes a built-in test framework (`ktest`) that runs inside the kernel itself at boot, controlled via the Limine kernel cmdline and build flags.

**Registration** is automatic through the `.ktest` ELF section - no central registry is needed:

```c
static void test_example(ktest_ctx_t *ctx) {
    void *p = malloc(64);
    KT_ASSERT_NONNULL(p);
    free(p);
}

KTEST("example", "memory", "malloc returns non-NULL", KT_FLAG_CRITICAL, test_example);
```

**Expected-panic tests** allow validating panic paths without halting:

```c
KT_EXPECT_PANIC(ctx, "assertion failed") {
    KERNEL_ASSERT(1 == 2);
}
KT_EXPECT_PANIC_VERIFY(ctx);
```

**Make targets:**

| Target | Runs |
|--------|------|
| `make test` | All non-destructive, non-stress tests |
| `make test-critical` | Critical tests only |
| `make test-memory` | Memory subsystem tests |
| `make test-panic` | Expected-panic tests |
| `make test-smp` | SMP and IRQ tests |
| `make stress` | Long-running stress tests |
| `make test-filter TEST_FILTER=x` | Tests whose name contains `x` |

Tests communicate results to CI through the QEMU `isa-debug-exit` device: exit code 1 = all passed, any other code = failure.

---

## 9. Userspace ABI

### Compatibility Goal

The long-term goal is to run unmodified Linux x86_64 binaries and libraries (musl, glibc) without a translation layer. This is a hard design constraint that must be honoured from the start -- retrofitting ABI compatibility after a divergent interface is built is not feasible.

This means:

- **Syscall numbers match Linux exactly** (x86_64 -- `read=0`, `write=1`, `open=2`, ...).
- **Syscall entry via `SYSCALL`/`SYSRET`**, not `INT 0x80`.
- **All structs exposed to userspace** (`stat`, `timespec`, `iovec`, `sigaction`, signal numbers, `mmap`/`open` flag values, `errno` values) must match Linux layouts byte-for-byte.
- **Dynamic linking must work**: the ELF loader must handle `PT_INTERP` so that `ld-linux.so` or musl's dynamic linker can be loaded. Without this only static binaries run.

The minimum syscall surface to run a musl-linked binary is approximately: `read`, `write`, `open`, `close`, `exit`, `exit_group`, `mmap`, `munmap`, `mprotect`, `brk`, `fstat`, `ioctl`, `getpid`, `uname`, `writev`, `arch_prctl` (TLS setup).

The kernel's internal design (scheduler, VFS, memory manager) is unconstrained. The ABI surface is the contract; everything below it is free.

### ABI Surface

- **Syscall interface** using the x86_64 System V ABI calling convention, Linux-compatible syscall numbering.
- **Process lifecycle**: `fork`, `exec`, `exit`, `wait` semantics.
- **File descriptor model** over a virtual filesystem abstraction.
- **Signal delivery** with standard Unix signal semantics and `sigaction` layout.
- **Memory mapping** via `mmap`/`munmap` with `MAP_ANONYMOUS` and file-backed mappings.

---

## 10. Subsystem Roadmap

Milestones are listed in dependency order. Each milestone should produce a clean, testable subsystem before the next begins.

| # | Milestone | Key Deliverables |
|---|-----------|-----------------|
| 1 | **Scheduler and kernel threading** | Preemptive SMP scheduler, per-core run queues, kernel thread lifecycle, context switch |
| 2 | **Process abstraction** | Process struct, address space isolation, kernel/user stack separation, process lifecycle |
| 3 | **Syscall ABI** | Syscall dispatch, user/kernel boundary, argument validation, error propagation |
| 4 | **Virtual filesystem** | VFS layer, file descriptor table, `open`/`read`/`write`/`close`, `stat`, directory traversal |
| 5 | **Initial userspace** | ELF loader, `exec`, initial user process, basic `libc` compatibility |
| 6 | **Device and driver framework** | Driver registration model, character device interface, `/dev` integration |
| 7 | **IPC and synchronization** | Pipes, signals, futex-like primitives, shared memory |
| 8 | **Networking** | Network stack, socket interface, TCP/IP |
| 9 | **Advanced VM** | `mmap`, demand paging, copy-on-write, page reclaim |
| 10 | **POSIX userspace** | Port of a shell, coreutils, eventually GNU toolchain |

---

## 11. Current State

| Subsystem | Status | Notes |
|-----------|--------|-------|
| Physical memory manager (PMM) | Stable | Bitmap allocator, contiguous frame support |
| Virtual memory manager (VMM) | Stable | Kernel page tables, higher-half mapping |
| Slab allocator | Stable | Fixed-size caches, large-object fallthrough |
| GDT | Stable | TSS per core |
| IDT / ISR dispatch | Stable | Dynamic IRQ allocation (vectors 32-254), SMP halt vector |
| LAPIC / IOAPIC | Stable | Timer, IPI, SMP bringup |
| HPET | Stable | Used for LAPIC calibration |
| SMP bringup | Stable | All APs reach idle, per-core GS |
| ACPI parsing | Stable | MADT, HPET table |
| ELF symbol resolution | Stable | O(log n) address lookup, section traversal |
| Panic subsystem | Stable | Full diagnostics, SMP freeze, depth gating |
| Structured logging | Stable | `klog` with severity levels |
| Test framework (ktest) | Stable | 66 tests, CI-ready QEMU exit code |
| Scheduler | Stable | Round-robin SMP, thread_block, idle threads, reaper |
| Process model | Not started | - |
| Syscall ABI | Not started | - |
| VFS | Not started | - |
| Drivers | Not started | - |

---

## 12. Building and Running

### Requirements

- `gcc` (primary compiler; clang may work but is not actively maintained)
- `ld` (GNU linker)
- `git`
- `xorriso`
- `qemu-system-x86_64` (for running)

### Build

```sh
# Build and run (BIOS boot)
make run

# Build and run (UEFI boot)
make run-uefi

# Build kernel only
make kernel
```

### Testing

```sh
# Run full test suite
make test

# Run specific category
make test-critical
make test-memory
make test-panic
make test-smp

# Run tests matching a name substring
make test-filter TEST_FILTER=slab
```

The test kernel is compiled separately with `-DKTEST_ENABLED`. The main kernel build is not affected.

### Limine cmdline flags (test mode)

| Flag | Effect |
|------|--------|
| `ktest` | Enable test runner (all non-stress tests) |
| `ktest=critical` | Critical tests only |
| `ktest=panic` | Panic-validation tests only |
| `ktest=stress` | Stress tests |
| `ktest.sub=memory` | Filter by subsystem |
| `ktest.filter=name` | Filter by name substring |
| `ktest.headless` | Serial-only output |
| `ktest.ci` | Write QEMU isa-debug-exit result |

---

## 13. Vendors

- [Flanterm](https://github.com/mintsuki/flanterm) - framebuffer terminal
- [printf](https://github.com/eyalroz/printf) - freestanding printf implementation
- [Limine](https://github.com/limine-bootloader/limine) - bootloader
