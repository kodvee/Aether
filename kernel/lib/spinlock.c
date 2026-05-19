#include <kernel/spinlock.h>
#include <kernel/cpu.h>

bool spinlock_acquire(spinlock_t *lock) {
    bool int_state = interrupt_state();
    disable_interrupts();

    /* Grab our position in line */
    uint16_t ticket = __atomic_fetch_add(&lock->next_ticket, 1, __ATOMIC_RELAXED);

    /* Spin until the lock holder finishes and serves our ticket.
     * ACQUIRE pairs with the RELEASE in spinlock_release, ensuring all
     * stores made inside the previous critical section are visible to us. */
    while (__atomic_load_n(&lock->now_serving, __ATOMIC_ACQUIRE) != ticket) {
        asm volatile ("pause");
    }

    return int_state;
}

void spinlock_release(spinlock_t *lock, bool int_state) {
    /* Advance now_serving to hand off to the next waiter.
     * RELEASE makes our critical-section stores visible before the handoff. */
    __atomic_fetch_add(&lock->now_serving, 1, __ATOMIC_RELEASE);
    if (int_state) enable_interrupts();
}
