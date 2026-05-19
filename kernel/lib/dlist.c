#include <kernel/dlist.h>
#include <kernel/mmu.h>

dlist_t *dlist_create(void) {
    dlist_t *list = malloc(sizeof(dlist_t));
    if (list) {
        list->head   = NULL;
        list->tail   = NULL;
        list->length = 0;
    }
    return list;
}

void dlist_destroy(dlist_t *list) {
    dlist_node_t *cur = list->head;
    while (cur) {
        dlist_node_t *next = cur->next;
        free(cur);
        cur = next;
    }
    free(list);
}

void dlist_push_back(dlist_t *list, void *value) {
    dlist_node_t *node = malloc(sizeof(dlist_node_t));
    if (!node) return;
    node->value = value;
    node->next  = NULL;
    node->prev  = list->tail;
    if (list->tail)
        list->tail->next = node;
    else
        list->head = node;
    list->tail = node;
    list->length++;
}

void dlist_push_front(dlist_t *list, void *value) {
    dlist_node_t *node = malloc(sizeof(dlist_node_t));
    if (!node) return;
    node->value = value;
    node->prev  = NULL;
    node->next  = list->head;
    if (list->head)
        list->head->prev = node;
    else
        list->tail = node;
    list->head = node;
    list->length++;
}

void *dlist_pop_back(dlist_t *list) {
    if (!list->tail) return NULL;
    dlist_node_t *node = list->tail;
    void *value = node->value;
    list->tail = node->prev;
    if (list->tail)
        list->tail->next = NULL;
    else
        list->head = NULL;
    list->length--;
    free(node);
    return value;
}

void *dlist_pop_front(dlist_t *list) {
    if (!list->head) return NULL;
    dlist_node_t *node = list->head;
    void *value = node->value;
    list->head = node->next;
    if (list->head)
        list->head->prev = NULL;
    else
        list->tail = NULL;
    list->length--;
    free(node);
    return value;
}

size_t dlist_get_length(dlist_t *list) {
    return list->length;
}

void *dlist_get(dlist_t *list, size_t index) {
    if (index >= list->length) return NULL;
    dlist_node_t *cur = list->head;
    for (size_t i = 0; i < index; i++)
        cur = cur->next;
    return cur->value;
}

void dlist_remove(dlist_t *list, void *value) {
    for (dlist_node_t *cur = list->head; cur; cur = cur->next) {
        if (cur->value != value) continue;
        if (cur->prev)
            cur->prev->next = cur->next;
        else
            list->head = cur->next;
        if (cur->next)
            cur->next->prev = cur->prev;
        else
            list->tail = cur->prev;
        list->length--;
        free(cur);
        return;
    }
}
