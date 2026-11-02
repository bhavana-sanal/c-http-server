#include "handlers.h"

#include <string.h>

/* Split the request-target into path and query (RFC 9112 §3.2). */
static bool split_target(struct http_ctx *ctx)
{
    struct http_str t = ctx->req->target;
    const char *p = t.p, *end = t.p + t.len;

    if (*p != '/') {
        /* absolute-form, which servers must accept (§3.2.2): drop scheme://authority */
        if (t.len >= 7 && http_str_ieq((struct http_str){p, 7}, "http://"))
            p += 7;
        else if (t.len >= 8 && http_str_ieq((struct http_str){p, 8}, "https://"))
            p += 8;
        else
            return false;
        while (p < end && *p != '/' && *p != '?')
            p++;
    }

    const char *q = memchr(p, '?', (size_t)(end - p));
    const char *path_end = q ? q : end;
    ctx->path = p == path_end ? (struct http_str){"/", 1}
                              : (struct http_str){p, (size_t)(path_end - p)};
    ctx->query = q ? (struct http_str){q + 1, (size_t)(end - q - 1)} : (struct http_str){"", 0};
    return true;
}

static void handle_healthz(struct http_ctx *ctx)
{
    if (!http_is_get_or_head(ctx)) {
        http_reject_method(ctx, "GET, HEAD");
        return;
    }
    http_begin(ctx, 200);
    http_finish(ctx, "text/plain; charset=utf-8", "ok\n", 3);
}

/* Bytes outside printable ASCII become \u00XX, which keeps the output valid
 * JSON whatever the client sent. */
static void json_string(struct buf *b, struct http_str s)
{
    buf_puts(b, "\"");
    for (size_t i = 0; i < s.len; i++) {
        unsigned char c = (unsigned char)s.p[i];
        if (c == '"' || c == '\\')
            buf_printf(b, "\\%c", c);
        else if (c >= 0x20 && c < 0x7f)
            buf_append(b, &s.p[i], 1);
        else
            buf_printf(b, "\\u%04x", c);
    }
    buf_puts(b, "\"");
}

static void handle_echo(struct http_ctx *ctx)
{
    const struct http_request *req = ctx->req;
    struct buf json = {0};

    buf_puts(&json, "{\"method\":");
    json_string(&json, req->method);
    buf_puts(&json, ",\"target\":");
    json_string(&json, req->target);
    buf_puts(&json, ",\"path\":");
    json_string(&json, ctx->path);
    buf_puts(&json, ",\"query\":");
    json_string(&json, ctx->query);
    buf_printf(&json, ",\"version\":\"HTTP/1.%d\",\"remote\":", req->minor);
    json_string(&json, (struct http_str){ctx->peer, strlen(ctx->peer)});
    buf_puts(&json, ",\"headers\":[");
    for (size_t i = 0; i < req->n_headers; i++) {
        buf_puts(&json, i ? ",[" : "[");
        json_string(&json, req->headers[i].name);
        buf_puts(&json, ",");
        json_string(&json, req->headers[i].value);
        buf_puts(&json, "]");
    }
    buf_puts(&json, "],\"body\":");
    json_string(&json, req->body);
    buf_puts(&json, "}\n");

    if (json.failed) {
        http_error(ctx, 500);
    } else {
        http_begin(ctx, 200);
        http_finish(ctx, "application/json", json.data, json.len);
    }
    buf_free(&json);
}

void route(struct http_ctx *ctx, const struct static_config *files)
{
    static const struct {
        const char *path;
        void (*handle)(struct http_ctx *ctx);
    } routes[] = {
        {"/healthz", handle_healthz},
        {"/echo", handle_echo},
    };

    if (!split_target(ctx)) {
        http_error(ctx, 400);
        return;
    }
    for (size_t i = 0; i < sizeof routes / sizeof *routes; i++) {
        if (http_str_eq(ctx->path, routes[i].path)) {
            routes[i].handle(ctx);
            return;
        }
    }
    static_serve(ctx, files);
}
