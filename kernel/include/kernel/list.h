#pragma once

/*
 * list.h - allocation-free intrusive doubly-linked list
 *
 * Design: embed list_node_t directly inside the owning struct.
 * The list never allocates; node memory is part of the object.
 * Use list_entry() to recover the containing struct from a node pointer.
 *
 * Invariants:
 *   - An empty list has head.next == head.prev == &head (circular sentinel).
 *   - A node not on any list should be initialised with LIST_NODE_INIT or
 *     list_node_init() so its pointers are non-garbage.
 *   - Callers are responsible for external synchronisation; these operations
 *     are NOT thread-safe and must be called under the appropriate lock.
 *   - list_entry() is only valid when the node IS on the list (or known to
 *     point to a valid containing struct).
 *
 * Contrast with dlist_t (kernel/lib/dlist.c):
 *   dlist_t is a non-intrusive list that allocates a wrapper node per entry.
 *   It is suitable for use during early-boot / init paths (ACPI table lists,
 *   module enumeration) where allocation overhead is acceptable.
 *   list_t (this header) is for hot paths - scheduler run queues, wait
 *   queues, timer lists - where allocations inside the data structure itself
 *   are a design violation.
 */

#include <stddef.h>
#include <stdbool.h>

/* -- Node -------------------------------------------------------------- */

typedef struct list_node {
    struct list_node *next;
    struct list_node *prev;
} list_node_t;

/* Initialise a node that is not yet on any list */
#define LIST_NODE_INIT(n) { .next = &(n), .prev = &(n) }

static inline void list_node_init(list_node_t *n) {
    n->next = n;
    n->prev = n;
}

/* -- Head (alias for node, used to make intent clear) ------------------ */

typedef list_node_t list_head_t;

#define LIST_HEAD_INIT(h) LIST_NODE_INIT(h)

static inline void list_head_init(list_head_t *h) {
    list_node_init(h);
}

/* -- Entry recovery ---------------------------------------------------- */

/*
 * list_entry - recover the pointer to the containing struct.
 *
 *   ptr    : pointer to the list_node_t member
 *   type   : type of the containing struct
 *   member : name of the list_node_t member within that struct
 */
#define list_entry(ptr, type, member) \
    ((type *)((uintptr_t)(ptr) - offsetof(type, member)))

/* -- Predicates -------------------------------------------------------- */

static inline bool list_empty(const list_head_t *h) {
    return h->next == h;
}

/* -- Insert / remove --------------------------------------------------- */

/* Insert 'n' between 'prev' and 'next' */
static inline void _list_insert(list_node_t *prev, list_node_t *next,
                                 list_node_t *n)
{
    n->prev    = prev;
    n->next    = next;
    prev->next = n;
    next->prev = n;
}

/* Add 'n' at the front of the list headed by 'h' (new head->next) */
static inline void list_push_front(list_head_t *h, list_node_t *n) {
    _list_insert(h, h->next, n);
}

/* Add 'n' at the back of the list headed by 'h' (new head->prev) */
static inline void list_push_back(list_head_t *h, list_node_t *n) {
    _list_insert(h->prev, h, n);
}

/* Remove 'n' from whichever list it is on */
static inline void list_remove(list_node_t *n) {
    n->prev->next = n->next;
    n->next->prev = n->prev;
    list_node_init(n); /* leave in a safe, detached state */
}

/* -- Pop --------------------------------------------------------------- */

/*
 * list_pop_front - remove and return the first node, or NULL if empty.
 * The returned node is in detached state (next == prev == self).
 */
static inline list_node_t *list_pop_front(list_head_t *h) {
    if (list_empty(h)) return NULL;
    list_node_t *n = h->next;
    list_remove(n);
    return n;
}

/* -- Iteration --------------------------------------------------------- */

/*
 * list_for_each - iterate over all nodes.
 * 'pos' is a list_node_t * cursor; do not modify the list inside the loop
 * unless using list_for_each_safe.
 */
#define list_for_each(pos, head) \
    for ((pos) = (head)->next; (pos) != (head); (pos) = (pos)->next)

/*
 * list_for_each_safe - iteration safe against removal of 'pos' inside the
 * loop body.  'tmp' is a list_node_t * scratch variable.
 */
#define list_for_each_safe(pos, tmp, head) \
    for ((pos) = (head)->next, (tmp) = (pos)->next; \
         (pos) != (head); \
         (pos) = (tmp), (tmp) = (pos)->next)
