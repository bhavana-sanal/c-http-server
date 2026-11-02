#include "poller.h"

#include <sys/epoll.h>

int poller_create(void)
{
    return epoll_create1(EPOLL_CLOEXEC);
}

int poller_add(int pfd, int fd, unsigned events, void *data)
{
    struct epoll_event ev = {.events = EPOLLET, .data.ptr = data};

    if (events & POLLER_IN)
        ev.events |= EPOLLIN;
    if (events & POLLER_OUT)
        ev.events |= EPOLLOUT;
    return epoll_ctl(pfd, EPOLL_CTL_ADD, fd, &ev);
}

int poller_wait(int pfd, void **ready, int timeout_ms)
{
    struct epoll_event events[POLLER_MAX_EVENTS];

    int n = epoll_wait(pfd, events, POLLER_MAX_EVENTS, timeout_ms);
    for (int i = 0; i < n; i++)
        ready[i] = events[i].data.ptr;
    return n;
}
