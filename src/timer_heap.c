#include "timer_heap.h"

#include <stdlib.h>

static void place(struct timer_heap *h, size_t i, struct timer *t)
{
    h->items[i] = t;
    t->idx = i;
}

static void sift_up(struct timer_heap *h, size_t i)
{
    struct timer *t = h->items[i];
    while (i > 0) {
        size_t parent = (i - 1) / 2;
        if (h->items[parent]->deadline <= t->deadline)
            break;
        place(h, i, h->items[parent]);
        i = parent;
    }
    place(h, i, t);
}

static void sift_down(struct timer_heap *h, size_t i)
{
    struct timer *t = h->items[i];
    for (;;) {
        size_t child = 2 * i + 1;
        if (child >= h->len)
            break;
        if (child + 1 < h->len && h->items[child + 1]->deadline < h->items[child]->deadline)
            child++;
        if (t->deadline <= h->items[child]->deadline)
            break;
        place(h, i, h->items[child]);
        i = child;
    }
    place(h, i, t);
}

int timer_heap_push(struct timer_heap *h, struct timer *t, uint64_t deadline)
{
    if (h->len == h->cap) {
        size_t cap = h->cap ? h->cap * 2 : 64;
        struct timer **items = realloc(h->items, cap * sizeof *items);
        if (!items)
            return -1;
        h->items = items;
        h->cap = cap;
    }
    t->deadline = deadline;
    place(h, h->len++, t);
    sift_up(h, t->idx);
    return 0;
}

void timer_heap_update(struct timer_heap *h, struct timer *t, uint64_t deadline)
{
    uint64_t old = t->deadline;
    t->deadline = deadline;
    if (deadline < old)
        sift_up(h, t->idx);
    else
        sift_down(h, t->idx);
}

void timer_heap_cancel(struct timer_heap *h, struct timer *t)
{
    if (t->idx == TIMER_IDLE)
        return;
    size_t i = t->idx;
    struct timer *last = h->items[--h->len];
    t->idx = TIMER_IDLE;
    if (last != t) {
        place(h, i, last);
        sift_up(h, i);
        sift_down(h, last->idx);
    }
}

struct timer *timer_heap_peek(const struct timer_heap *h)
{
    return h->len > 0 ? h->items[0] : NULL;
}

void timer_heap_free(struct timer_heap *h)
{
    free(h->items);
    *h = (struct timer_heap){0};
}
