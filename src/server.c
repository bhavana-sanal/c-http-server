#include "server.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/sendfile.h>
#elif defined(__APPLE__)
#include <sys/uio.h>
#else
#error "sendfile(): only Linux and macOS are supported"
#endif

#include "handlers.h"
#include "http.h"
#include "poller.h"
#include "timer_heap.h"

/* A request is at most a full head plus a full decoded body; the slack lets
 * the chunk terminator and trailers arrive after a maximal chunked body. */
#define CONN_IN_SIZE (HTTP_MAX_HEAD + HTTP_MAX_BODY + 4096)

/* Bytes we are willing to read and discard while lingering before close. */
#define LINGER_MAX (256 * 1024)

enum conn_state {
    CONN_READING, /* parsing a request (possibly with a 100 Continue queued) */
    CONN_WRITING, /* sending the response */
    CONN_CLOSING, /* write side shut down; draining input until the peer closes */
};

struct conn {
    int fd; /* -1 once closed */
    enum conn_state state;
    struct timer timer;
    struct conn *prev, *next; /* live list; `next` links the closed list */
    struct http_parser parser;
    struct http_request req;
    struct http_ctx ctx;
    struct buf out;
    size_t out_off;
    bool continue_queued;
    char peer[NI_MAXHOST];
    size_t in_len;
    char in[CONN_IN_SIZE];
};

struct server {
    const struct server_config *cfg;
    int pfd;
    int listen_fd;
    int sig_pipe[2];
    bool draining;
    uint64_t now; /* ms, monotonic; refreshed after every poll */
    struct timer_heap timers;
    struct conn *conns;
    /* Closed connections are freed only after the current event batch, since
     * a later event in the same batch may still point at them. */
    struct conn *closed;
};

static int signal_fd = -1;

static void on_signal(int sig)
{
    int saved = errno;
    char byte = (char)sig;
    /* If the pipe is full a wakeup is already pending, so a failed write is fine. */
    ssize_t r = write(signal_fd, &byte, 1);
    (void)r;
    errno = saved;
}

int install_signal_handlers(void (*handler)(int))
{
    struct sigaction sa = {0};
    sigset_t set;

    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    if (sigaction(SIGINT, &sa, NULL) < 0 || sigaction(SIGTERM, &sa, NULL) < 0)
        return -1;
    return sigprocmask(SIG_UNBLOCK, &set, NULL);
}

int set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* The two kernels disagree on sendfile()'s signature and on how a partial
 * write is reported; normalise to write()-like semantics. */
static ssize_t send_file(int sock, int fd, off_t off, uint64_t len)
{
    size_t chunk = len > (1u << 30) ? (1u << 30) : (size_t)len;
#if defined(__linux__)
    return sendfile(sock, fd, &off, chunk);
#else
    off_t sent = (off_t)chunk;
    /* On EAGAIN/EINTR macOS still reports the bytes it managed to send. */
    if (sendfile(fd, sock, off, &sent, NULL, 0) < 0 && sent == 0)
        return -1;
    return (ssize_t)sent;
#endif
}

static void conn_touch(struct server *s, struct conn *c)
{
    timer_heap_update(&s->timers, &c->timer, s->now + (uint64_t)s->cfg->timeout_ms);
}

static void conn_close(struct server *s, struct conn *c)
{
    if (close(c->fd) < 0)
        perror("close");
    c->fd = -1;
    if (c->ctx.file_fd >= 0 && close(c->ctx.file_fd) < 0)
        perror("close");
    c->ctx.file_fd = -1;
    timer_heap_cancel(&s->timers, &c->timer);

    if (c->prev)
        c->prev->next = c->next;
    else
        s->conns = c->next;
    if (c->next)
        c->next->prev = c->prev;
    c->next = s->closed;
    s->closed = c;
}

static void free_closed(struct server *s)
{
    while (s->closed) {
        struct conn *c = s->closed;
        s->closed = c->next;
        buf_free(&c->out);
        free(c);
    }
}

static void conn_open(struct server *s, int fd, const struct sockaddr *addr, socklen_t addr_len)
{
    int one = 1;
    struct conn *c = malloc(sizeof *c);

    /* Nagle would hold back the tail of a response sent as header + sendfile
     * until the client's delayed ACK, adding ~40 ms to small requests. */
    if (!c || set_nonblock(fd) < 0 ||
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one) < 0) {
        perror("accept");
        free(c);
        close(fd);
        return;
    }
    memset(c, 0, offsetof(struct conn, in));
    c->fd = fd;
    c->state = CONN_READING;
    c->timer.idx = TIMER_IDLE;
    c->ctx.file_fd = -1;
    http_parser_init(&c->parser, &c->req);
    if (getnameinfo(addr, addr_len, c->peer, sizeof c->peer, NULL, 0, NI_NUMERICHOST) != 0)
        strcpy(c->peer, "-");

    if (timer_heap_push(&s->timers, &c->timer, s->now + (uint64_t)s->cfg->timeout_ms) < 0) {
        perror("accept");
        free(c);
        close(fd);
        return;
    }
    if (poller_add(s->pfd, fd, POLLER_IN | POLLER_OUT, c) < 0) {
        perror("poller_add");
        timer_heap_cancel(&s->timers, &c->timer);
        free(c);
        close(fd);
        return;
    }
    c->next = s->conns;
    if (s->conns)
        s->conns->prev = c;
    s->conns = c;
}

static void accept_all(struct server *s)
{
    for (;;) {
        struct sockaddr_storage addr;
        socklen_t len = sizeof addr;
        int fd = accept(s->listen_fd, (struct sockaddr *)&addr, &len);
        if (fd >= 0) {
            conn_open(s, fd, (struct sockaddr *)&addr, len);
            continue;
        }
        if (errno == EINTR || errno == ECONNABORTED)
            continue;
        /* On EMFILE the backlog stays queued until the next connection
         * re-arms the edge; there is no spare fd to shed load with. */
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            perror("accept");
        return;
    }
}

static void log_escaped(struct http_str s)
{
    for (size_t i = 0; i < s.len; i++) {
        if (s.p[i] == '"' || s.p[i] == '\\')
            putchar('\\');
        putchar(s.p[i]);
    }
}

/* Common Log Format: host ident authuser [date] "request" status bytes */
static void access_log(const struct conn *c)
{
    char when[64];
    time_t now = time(NULL);
    struct tm tm;

    if (!localtime_r(&now, &tm) || strftime(when, sizeof when, "%d/%b/%Y:%H:%M:%S %z", &tm) == 0)
        strcpy(when, "-");
    printf("%s - - [%s] \"", c->peer, when);
    if (c->req.minor >= 0) {
        log_escaped(c->req.method);
        putchar(' ');
        log_escaped(c->req.target);
        printf(" HTTP/1.%d", c->req.minor);
    } else {
        putchar('-');
    }
    if (c->ctx.body_len > 0)
        printf("\" %d %" PRIu64 "\n", c->ctx.status, c->ctx.body_len);
    else
        printf("\" %d -\n", c->ctx.status);
}

/* Returns 1 once everything queued is sent, 0 if the socket is full, -1 on error. */
static int conn_flush(struct server *s, struct conn *c)
{
    struct http_ctx *ctx = &c->ctx;
    bool progress = false;
    int rc = 1;

    while (c->out_off < c->out.len) {
        ssize_t n = send(c->fd, c->out.data + c->out_off, c->out.len - c->out_off, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            rc = errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
            goto out;
        }
        c->out_off += (size_t)n;
        progress = true;
    }
    c->out.len = c->out_off = 0;

    while (ctx->file_len > 0) {
        ssize_t n = send_file(c->fd, ctx->file_fd, ctx->file_off, ctx->file_len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            rc = errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
            goto out;
        }
        if (n == 0) { /* the file shrank under us; the response can't be completed */
            rc = -1;
            goto out;
        }
        ctx->file_off += n;
        ctx->file_len -= (uint64_t)n;
        progress = true;
    }
    if (ctx->file_fd >= 0) {
        if (close(ctx->file_fd) < 0)
            perror("close");
        ctx->file_fd = -1;
    }
out:
    /* Writes extend the deadline: a slow reader is fine, a stalled one isn't. */
    if (progress)
        conn_touch(s, c);
    return rc;
}

static bool respond(struct server *s, struct conn *c, int error_status)
{
    http_ctx_init(&c->ctx, &c->req, &c->out, c->peer, error_status == 0 && !s->draining);
    if (error_status)
        http_error(&c->ctx, error_status);
    else
        route(&c->ctx, &s->cfg->files);
    if (c->out.failed) {
        conn_close(s, c);
        return false;
    }
    c->state = CONN_WRITING;
    return true;
}

/* Parse what we have; read more if needed. Returns false when the caller
 * should stop driving this connection (waiting for input, or closed). */
static bool conn_read(struct server *s, struct conn *c)
{
    switch (http_parse(&c->parser, c->in, &c->in_len)) {
    case HTTP_PARSE_DONE:
        return respond(s, c, 0);
    case HTTP_PARSE_ERROR:
        return respond(s, c, c->parser.status);
    case HTTP_PARSE_INCOMPLETE:
        break;
    }

    if (c->req.expect_continue && !c->continue_queued) {
        /* The head is in and the body isn't: invite the client to send it
         * (RFC 9110 §10.1.1) rather than make it wait out its own timer. */
        c->continue_queued = true;
        buf_puts(&c->out, "HTTP/1.1 100 Continue\r\n\r\n");
        if (c->out.failed) {
            conn_close(s, c);
            return false;
        }
        return true;
    }

    for (;;) {
        ssize_t n = recv(c->fd, c->in + c->in_len, sizeof c->in - c->in_len, 0);
        if (n > 0) {
            c->in_len += (size_t)n;
            return true;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
            conn_close(s, c);
        return false;
    }
}

static bool response_done(struct server *s, struct conn *c)
{
    if (!s->cfg->quiet)
        access_log(c);

    if (c->ctx.keep_alive && !s->draining) {
        /* Keep any pipelined bytes that followed this request. */
        size_t used = c->parser.pos;
        c->in_len -= used;
        memmove(c->in, c->in + used, c->in_len);
        http_parser_init(&c->parser, &c->req);
        c->continue_queued = false;
        c->state = CONN_READING;
        conn_touch(s, c);
        return true;
    }

    /* Lingering close (RFC 9112 §9.6): closing with unread input would make
     * the kernel send RST, which can destroy the response before the client
     * reads it. Half-close instead and drain until the client hangs up. */
    if (shutdown(c->fd, SHUT_WR) < 0) {
        conn_close(s, c);
        return false;
    }
    c->state = CONN_CLOSING;
    conn_touch(s, c);
    return true;
}

static void discard_input(struct server *s, struct conn *c)
{
    char sink[4096];

    for (size_t total = 0; total < LINGER_MAX;) {
        ssize_t n = recv(c->fd, sink, sizeof sink, 0);
        if (n > 0) {
            total += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        break;
    }
    conn_close(s, c);
}

/* Drive a connection as far as it can go without blocking. Safe to call on
 * any event: every path either makes progress or stops at EAGAIN, which is
 * what edge-triggered notification requires. */
static void conn_run(struct server *s, struct conn *c)
{
    for (;;) {
        int rc = conn_flush(s, c);
        if (rc < 0) {
            conn_close(s, c);
            return;
        }
        if (rc == 0)
            return;

        bool more = false;
        switch (c->state) {
        case CONN_WRITING:
            more = response_done(s, c);
            break;
        case CONN_READING:
            more = conn_read(s, c);
            break;
        case CONN_CLOSING:
            discard_input(s, c);
            break;
        }
        if (!more)
            return;
    }
}

static void begin_drain(struct server *s)
{
    char sink[64];
    size_t busy = 0;

    while (read(s->sig_pipe[0], sink, sizeof sink) > 0) {
    }
    if (s->draining)
        return;
    s->draining = true;
    if (close(s->listen_fd) < 0)
        perror("close");
    s->listen_fd = -1;

    /* Idle keep-alive connections can go now; the rest finish their current
     * response and are closed by response_done(). */
    for (struct conn *c = s->conns, *next; c; c = next) {
        next = c->next;
        if (c->state == CONN_READING && c->in_len == 0)
            conn_close(s, c);
        else
            busy++;
    }
    fprintf(stderr, "shutting down, draining %zu connection%s\n", busy, busy == 1 ? "" : "s");
}

static int next_timeout(const struct server *s)
{
    const struct timer *t = timer_heap_peek(&s->timers);

    if (!t)
        return -1;
    if (t->deadline <= s->now)
        return 0;
    uint64_t ms = t->deadline - s->now;
    return ms > INT_MAX ? INT_MAX : (int)ms;
}

static void expire_timers(struct server *s)
{
    struct timer *t;

    while ((t = timer_heap_peek(&s->timers)) && t->deadline <= s->now)
        conn_close(s, (struct conn *)((char *)t - offsetof(struct conn, timer)));
}

int server_run(int listen_fd, const struct server_config *cfg)
{
    struct server s = {.cfg = cfg, .pfd = -1, .listen_fd = listen_fd, .sig_pipe = {-1, -1}};
    int rc = -1;

    if ((s.pfd = poller_create()) < 0 || pipe(s.sig_pipe) < 0 || set_nonblock(s.sig_pipe[0]) < 0 ||
        set_nonblock(s.sig_pipe[1]) < 0) {
        perror("server setup");
        goto out;
    }
    signal_fd = s.sig_pipe[1];
    if (install_signal_handlers(on_signal) < 0 ||
        poller_add(s.pfd, listen_fd, POLLER_IN, &s.listen_fd) < 0 ||
        poller_add(s.pfd, s.sig_pipe[0], POLLER_IN, s.sig_pipe) < 0) {
        perror("server setup");
        goto out;
    }

    while (!s.draining || s.conns) {
        void *ready[POLLER_MAX_EVENTS];
        s.now = now_ms();
        int n = poller_wait(s.pfd, ready, next_timeout(&s));
        if (n < 0 && errno != EINTR) {
            perror("poller_wait");
            goto out;
        }
        s.now = now_ms();
        for (int i = 0; i < n; i++) {
            if (ready[i] == &s.listen_fd) {
                if (s.listen_fd >= 0)
                    accept_all(&s);
            } else if (ready[i] == s.sig_pipe) {
                begin_drain(&s);
            } else {
                struct conn *c = ready[i];
                if (c->fd >= 0)
                    conn_run(&s, c);
            }
        }
        expire_timers(&s);
        free_closed(&s);
    }
    rc = 0;

out:
    while (s.conns)
        conn_close(&s, s.conns);
    free_closed(&s);
    timer_heap_free(&s.timers);
    signal_fd = -1;
    for (int i = 0; i < 2; i++)
        if (s.sig_pipe[i] >= 0)
            close(s.sig_pipe[i]);
    if (s.listen_fd >= 0)
        close(s.listen_fd);
    if (s.pfd >= 0)
        close(s.pfd);
    return rc;
}
