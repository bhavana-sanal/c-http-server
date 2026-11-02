#include <errno.h>
#include <limits.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "server.h"

#define MAX_WORKERS 64

static pid_t workers[MAX_WORKERS];
static int n_workers;

static void usage(FILE *out)
{
    fputs("usage: c-http-server [-b addr] [-p port] [-r root] [-w workers] [-t seconds] [-a] [-q]\n"
          "  -b addr     address to bind (default 127.0.0.1)\n"
          "  -p port     port to listen on, 0 for any free port (default 8080)\n"
          "  -r root     directory to serve (default .)\n"
          "  -w workers  processes sharing the port via SO_REUSEPORT (default 1)\n"
          "  -t seconds  idle, request and write timeout (default 5)\n"
          "  -a          list directories that have no index.html\n"
          "  -q          disable the access log\n",
          out);
}

static bool parse_int(const char *s, long min, long max, long *out)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno || end == s || *end || v < min || v > max)
        return false;
    *out = v;
    return true;
}

static int open_listener(const struct sockaddr *addr, socklen_t len, bool reuseport)
{
    int one = 1;
    const char *failed = NULL;
    int fd = socket(addr->sa_family, SOCK_STREAM, 0);

    if (fd < 0) {
        perror("socket");
        return -1;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) < 0)
        failed = "SO_REUSEADDR";
    else if (reuseport && setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one) < 0)
        failed = "SO_REUSEPORT";
    else if (bind(fd, addr, len) < 0)
        failed = "bind";
    else if (listen(fd, SOMAXCONN) < 0)
        failed = "listen";
    else if (set_nonblock(fd) < 0)
        failed = "fcntl";
    if (failed) {
        perror(failed);
        close(fd);
        return -1;
    }
    return fd;
}

/* Bind n sockets to the same address. The first one resolves port 0, so the
 * rest (with SO_REUSEPORT) can join it on the port the kernel picked. */
static int open_listeners(const char *host, const char *port, int *fds, int n, char *where,
                          size_t where_len)
{
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM, .ai_flags = AI_PASSIVE};
    struct addrinfo *ai;
    struct sockaddr_storage addr;
    socklen_t len;
    int err = getaddrinfo(host, port, &hints, &ai);

    if (err) {
        fprintf(stderr, "c-http-server: %s:%s: %s\n", host, port, gai_strerror(err));
        return -1;
    }
    len = ai->ai_addrlen;
    memcpy(&addr, ai->ai_addr, len);
    freeaddrinfo(ai);

    for (int i = 0; i < n; i++) {
        fds[i] = open_listener((struct sockaddr *)&addr, len, n > 1);
        if (fds[i] < 0 || (i == 0 && getsockname(fds[0], (struct sockaddr *)&addr, &len) < 0)) {
            if (fds[i] >= 0)
                perror("getsockname");
            for (int j = 0; j <= i; j++)
                if (fds[j] >= 0)
                    close(fds[j]);
            return -1;
        }
    }

    char h[NI_MAXHOST], p[NI_MAXSERV];
    err = getnameinfo((struct sockaddr *)&addr, len, h, sizeof h, p, sizeof p,
                      NI_NUMERICHOST | NI_NUMERICSERV);
    if (err)
        snprintf(where, where_len, "%s:%s", host, port);
    else
        snprintf(where, where_len, strchr(h, ':') ? "[%s]:%s" : "%s:%s", h, p);
    return 0;
}

static void forward_signal(int sig)
{
    int saved = errno;
    for (int i = 0; i < n_workers; i++)
        if (workers[i] > 0)
            kill(workers[i], sig);
    errno = saved;
}

static int run_workers(const int *fds, int n, const struct server_config *cfg)
{
    sigset_t set;
    int status = 0;

    /* Hold shutdown signals until every process has its own handler: a child
     * must not run the parent's forwarding handler, nor die on the default one. */
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &set, NULL) < 0) {
        perror("sigprocmask");
        return 1;
    }

    for (int i = 0; i < n; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            status = 1;
            break;
        }
        if (pid == 0) {
            for (int j = 0; j < n; j++)
                if (j != i)
                    close(fds[j]);
            exit(server_run(fds[i], cfg) == 0 ? 0 : 1);
        }
        workers[n_workers++] = pid;
    }
    for (int i = 0; i < n; i++)
        close(fds[i]);

    if (install_signal_handlers(forward_signal) < 0) {
        perror("sigaction");
        status = 1;
    }
    if (status)
        forward_signal(SIGTERM);

    for (int alive = n_workers; alive > 0;) {
        int ws;
        pid_t pid = waitpid(-1, &ws, 0);
        if (pid < 0) {
            if (errno == EINTR)
                continue;
            perror("waitpid");
            return 1;
        }
        alive--;
        for (int i = 0; i < n_workers; i++)
            if (workers[i] == pid)
                workers[i] = 0;
        if (!WIFEXITED(ws) || WEXITSTATUS(ws) != 0) {
            fprintf(stderr, "c-http-server: worker %ld exited abnormally\n", (long)pid);
            status = 1;
        }
    }
    return status;
}

int main(int argc, char **argv)
{
    const char *host = "127.0.0.1", *port = "8080", *root = ".";
    long nworkers = 1, timeout = 5;
    struct server_config cfg = {0};
    char root_real[PATH_MAX], where[NI_MAXHOST + NI_MAXSERV + 4];
    struct stat st;
    int fds[MAX_WORKERS];
    int opt;

    while ((opt = getopt(argc, argv, "b:p:r:w:t:aqh")) != -1) {
        switch (opt) {
        case 'b': host = optarg; break;
        case 'p': port = optarg; break;
        case 'r': root = optarg; break;
        case 'a': cfg.files.autoindex = true; break;
        case 'q': cfg.quiet = true; break;
        case 'h': usage(stdout); return 0;
        case 'w':
            if (!parse_int(optarg, 1, MAX_WORKERS, &nworkers)) {
                fprintf(stderr, "c-http-server: -w wants 1..%d\n", MAX_WORKERS);
                return 2;
            }
            break;
        case 't':
            if (!parse_int(optarg, 1, 3600, &timeout)) {
                fputs("c-http-server: -t wants 1..3600 seconds\n", stderr);
                return 2;
            }
            break;
        default:
            usage(stderr);
            return 2;
        }
    }
    if (optind != argc) {
        usage(stderr);
        return 2;
    }

    if (!realpath(root, root_real) || stat(root_real, &st) < 0) {
        fprintf(stderr, "c-http-server: %s: %s\n", root, strerror(errno));
        return 1;
    }
    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "c-http-server: %s: not a directory\n", root);
        return 1;
    }
    if (strcmp(root_real, "/") == 0)
        root_real[0] = '\0'; /* paths are built as root + "/...", so avoid "//" */
    cfg.files.root = root_real;
    cfg.timeout_ms = (int)timeout * 1000;

    /* Peers that disconnect mid-response must cost an EPIPE, not the process. */
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        perror("signal");
        return 1;
    }
    /* One write per access-log line, so workers sharing stdout don't interleave. */
    if (setvbuf(stdout, NULL, _IOLBF, 0) != 0) {
        perror("setvbuf");
        return 1;
    }

    if (open_listeners(host, port, fds, (int)nworkers, where, sizeof where) < 0)
        return 1;
    fprintf(stderr, "listening on http://%s (%ld worker%s)\n", where, nworkers,
            nworkers == 1 ? "" : "s");

    if (nworkers == 1)
        return server_run(fds[0], &cfg) == 0 ? 0 : 1;
    return run_workers(fds, (int)nworkers, &cfg);
}
