#ifndef TIMER_HEAP_H
#define TIMER_HEAP_H

#include <stddef.h>
#include <stdint.h>

/*
 * Binary min-heap of deadlines, with each timer remembering its slot so it
 * can be moved or removed in O(log n).
 *
 * Chosen over a timer wheel because the loop needs the exact time of the
 * next expiry to use as its poll timeout: with a heap an idle server sleeps
 * until something is actually due, instead of waking every tick to advance
 * a wheel. At the connection counts a single process handles, log n is a
 * handful of pointer swaps.
 */

#define TIMER_IDLE SIZE_MAX

struct timer {
    uint64_t deadline; /* milliseconds, monotonic clock */
    size_t idx;        /* slot in the heap, or TIMER_IDLE */
};

struct timer_heap {
    struct timer **items;
    size_t len, cap;
};

int timer_heap_push(struct timer_heap *h, struct timer *t, uint64_t deadline);
/* `t` must currently be in the heap. */
void timer_heap_update(struct timer_heap *h, struct timer *t, uint64_t deadline);
void timer_heap_cancel(struct timer_heap *h, struct timer *t);
struct timer *timer_heap_peek(const struct timer_heap *h);
void timer_heap_free(struct timer_heap *h);

#endif
