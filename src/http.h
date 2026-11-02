#ifndef HTTP_H
#define HTTP_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

#include "buf.h"
#include "http_parser.h"

#define HTTP_DATE_SIZE 30 /* "Sun, 06 Nov 1994 08:49:37 GMT" + NUL */

/* One request/response exchange, as seen by handlers. */
struct http_ctx {
    const struct http_request *req;
    const char *peer;            /* numeric client address */
    struct http_str path, query; /* from the request-target, still percent-encoded */
    struct buf *out;             /* serialized response head and in-memory body */
    int status;
    bool keep_alive;
    bool head;                   /* HEAD request: headers only */
    uint64_t body_len;           /* content bytes sent, for the access log */
    int file_fd;                 /* if >= 0, file_len bytes at file_off follow `out` */
    off_t file_off;
    uint64_t file_len;
};

void http_ctx_init(struct http_ctx *ctx, const struct http_request *req, struct buf *out,
                   const char *peer, bool keep_alive);

/* Response construction: http_begin, any http_header calls, then exactly one
 * of http_end (caller supplies the body), http_finish or http_error_body. */
void http_begin(struct http_ctx *ctx, int status);
void http_header(struct http_ctx *ctx, const char *name, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
void http_end(struct http_ctx *ctx, uint64_t content_length);
void http_finish(struct http_ctx *ctx, const char *type, const void *body, size_t len);
void http_error_body(struct http_ctx *ctx);
void http_error(struct http_ctx *ctx, int status);
/* 405 with an Allow header for methods we know, 501 for ones we don't. */
void http_reject_method(struct http_ctx *ctx, const char *allow);

const char *http_reason(int status);
void http_format_date(time_t t, char out[HTTP_DATE_SIZE]);
bool http_parse_date(struct http_str s, time_t *out);

static inline bool http_is_get_or_head(const struct http_ctx *ctx)
{
    return ctx->head || http_str_eq(ctx->req->method, "GET");
}

#endif
