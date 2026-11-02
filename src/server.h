#ifndef SERVER_H
#define SERVER_H

#include <stdbool.h>

#include "static.h"

struct server_config {
    struct static_config files;
    int timeout_ms; /* idle, request and write-progress timeout */
    bool quiet;     /* no access log */
};

/* Run the event loop on a non-blocking listening socket until SIGINT or
 * SIGTERM, then stop accepting and finish in-flight responses. Takes
 * ownership of listen_fd. Returns 0 on a clean shutdown. */
int server_run(int listen_fd, const struct server_config *cfg);

int set_nonblock(int fd);
/* Install `handler` for SIGINT and SIGTERM, then unblock both. */
int install_signal_handlers(void (*handler)(int));

#endif
