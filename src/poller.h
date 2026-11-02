#ifndef POLLER_H
#define POLLER_H

/*
 * Readiness notification, implemented with kqueue (poller_kqueue.c) or
 * epoll (poller_epoll.c); the Makefile picks one per platform.
 *
 * Registrations are edge-triggered: an fd is reported when it *becomes*
 * readable or writable, so its owner must read or write until EAGAIN before
 * waiting again. Closing an fd removes its registration.
 */

#define POLLER_IN  1u
#define POLLER_OUT 2u
#define POLLER_MAX_EVENTS 64

int poller_create(void);
int poller_add(int pfd, int fd, unsigned events, void *data);
/* Fills `ready` with the `data` of up to POLLER_MAX_EVENTS ready fds. The same
 * pointer may appear twice (once per direction). timeout_ms < 0 blocks. */
int poller_wait(int pfd, void **ready, int timeout_ms);

#endif
