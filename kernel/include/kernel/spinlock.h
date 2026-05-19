#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef volatile struct {
    uint16_t next_ticket;
    uint16_t now_serving;
} spinlock_t;

#define SPINLOCK_ZERO {0}

bool spinlock_acquire(spinlock_t *lock);
void spinlock_release(spinlock_t *lock, bool int_state);

/* True if at least one ticket is outstanding (i.e., some CPU holds the lock).
 * Use in assertions to verify a required lock is held before calling an
 * unlocked internal helper. */
static inline bool spinlock_is_held(const spinlock_t *lock) {
    return lock->next_ticket != lock->now_serving;
}
