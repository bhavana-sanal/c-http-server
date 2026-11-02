#include <stdbool.h>
#include <stdlib.h>

#include "check.h"
#include "timer_heap.h"

#define N 1000

/* Random pushes, updates and cancels; then drain and verify the heap hands
 * back exactly the live timers in deadline order. */
int main(void)
{
    static struct timer timers[N];
    struct timer_heap h = {0};
    bool live[N] = {0};
    uint64_t state = 42;

    for (size_t i = 0; i < N; i++)
        timers[i].idx = TIMER_IDLE;

    for (int op = 0; op < 20000; op++) {
        state = state * 6364136223846793005u + 1442695040888963407u;
        size_t i = (size_t)(state >> 33) % N;
        uint64_t deadline = (state >> 13) % 100000;
        switch ((state >> 7) % 3) {
        case 0:
            if (live[i])
                timer_heap_update(&h, &timers[i], deadline);
            else
                CHECK(timer_heap_push(&h, &timers[i], deadline) == 0, "push");
            live[i] = true;
            break;
        case 1:
            if (live[i])
                timer_heap_update(&h, &timers[i], deadline);
            break;
        case 2:
            timer_heap_cancel(&h, &timers[i]);
            CHECK(timers[i].idx == TIMER_IDLE, "cancel");
            live[i] = false;
            break;
        }
    }

    size_t n_live = 0;
    for (size_t i = 0; i < N; i++)
        n_live += live[i];
    CHECK(h.len == n_live, "size");

    uint64_t prev = 0;
    struct timer *t;
    while ((t = timer_heap_peek(&h))) {
        CHECK(t->deadline >= prev, "order");
        CHECK(live[t - timers], "only live timers");
        live[t - timers] = false;
        prev = t->deadline;
        timer_heap_cancel(&h, t);
    }
    for (size_t i = 0; i < N; i++)
        CHECK(!live[i], "every live timer came out");

    timer_heap_free(&h);
    return check_summary("test_timer_heap");
}
