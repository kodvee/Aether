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
