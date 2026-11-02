#include "poller.h"

#include <stddef.h>
#include <sys/event.h>
#include <sys/types.h>
#include <time.h>

int poller_create(void)
{
    return kqueue();
}

int poller_add(int pfd, int fd, unsigned events, void *data)
{
    struct kevent changes[2];
    int n = 0;

    if (events & POLLER_IN)
        EV_SET(&changes[n++], fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, data);
    if (events & POLLER_OUT)
        EV_SET(&changes[n++], fd, EVFILT_WRITE, EV_ADD | EV_CLEAR, 0, 0, data);
    return kevent(pfd, changes, n, NULL, 0, NULL);
}

int poller_wait(int pfd, void **ready, int timeout_ms)
{
    struct kevent events[POLLER_MAX_EVENTS];
    struct timespec ts = {timeout_ms / 1000, (long)(timeout_ms % 1000) * 1000000};

    int n = kevent(pfd, NULL, 0, events, POLLER_MAX_EVENTS, timeout_ms < 0 ? NULL : &ts);
    for (int i = 0; i < n; i++)
        ready[i] = events[i].udata;
    return n;
}
