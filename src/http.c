#include "http.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void http_ctx_init(struct http_ctx *ctx, const struct http_request *req, struct buf *out,
                   const char *peer, bool keep_alive)
{
    *ctx = (struct http_ctx){
        .req = req,
        .peer = peer,
        .out = out,
        .keep_alive = keep_alive && req->keep_alive,
        .head = http_str_eq(req->method, "HEAD"),
        .file_fd = -1,
    };
}

const char *http_reason(int status)
{
    switch (status) {
    case 200: return "OK";
    case 206: return "Partial Content";
    case 301: return "Moved Permanently";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 416: return "Range Not Satisfiable";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 505: return "HTTP Version Not Supported";
    default:  return "Unknown";
    }
}

void http_begin(struct http_ctx *ctx, int status)
{
    char date[HTTP_DATE_SIZE];

    ctx->status = status;
    http_format_date(time(NULL), date);
    /* RFC 9110 §2.5: always answer with the highest 1.x version we speak. */
    buf_printf(ctx->out, "HTTP/1.1 %d %s\r\nDate: %s\r\nServer: c-http-server\r\n", status,
               http_reason(status), date);
    if (!ctx->keep_alive)
        buf_puts(ctx->out, "Connection: close\r\n");
    else if (ctx->req->minor == 0)
        buf_puts(ctx->out, "Connection: keep-alive\r\n");
}

void http_header(struct http_ctx *ctx, const char *name, const char *fmt, ...)
{
    va_list ap;

    buf_printf(ctx->out, "%s: ", name);
    va_start(ap, fmt);
    buf_vprintf(ctx->out, fmt, ap);
    va_end(ap);
    buf_puts(ctx->out, "\r\n");
}

void http_end(struct http_ctx *ctx, uint64_t content_length)
{
    bool bodiless = ctx->status == 304; /* RFC 9110 §15.4.5 */

    if (!bodiless)
        http_header(ctx, "Content-Length", "%" PRIu64, content_length);
    buf_puts(ctx->out, "\r\n");
    ctx->body_len = ctx->head || bodiless ? 0 : content_length;
}

void http_finish(struct http_ctx *ctx, const char *type, const void *body, size_t len)
{
    http_header(ctx, "Content-Type", "%s", type);
    http_end(ctx, len);
    if (!ctx->head)
        buf_append(ctx->out, body, len);
}

void http_error_body(struct http_ctx *ctx)
{
    char msg[64];
    int n = snprintf(msg, sizeof msg, "%d %s\n", ctx->status, http_reason(ctx->status));
    http_finish(ctx, "text/plain; charset=utf-8", msg, (size_t)n);
}

void http_error(struct http_ctx *ctx, int status)
{
    http_begin(ctx, status);
    http_error_body(ctx);
}

void http_reject_method(struct http_ctx *ctx, const char *allow)
{
    static const char *const standard[] = {
        "GET", "HEAD", "POST", "PUT", "DELETE", "CONNECT", "OPTIONS", "TRACE", "PATCH",
    };

    for (size_t i = 0; i < sizeof standard / sizeof *standard; i++) {
        if (http_str_eq(ctx->req->method, standard[i])) {
            http_begin(ctx, 405);
            http_header(ctx, "Allow", "%s", allow); /* required by RFC 9110 §15.5.6 */
            http_error_body(ctx);
            return;
        }
    }
    http_error(ctx, 501); /* RFC 9110 §9.1: unrecognized method */
}

void http_format_date(time_t t, char out[HTTP_DATE_SIZE])
{
    struct tm tm;

    if (!gmtime_r(&t, &tm) || strftime(out, HTTP_DATE_SIZE, "%a, %d %b %Y %H:%M:%S GMT", &tm) == 0)
        out[0] = '\0';
}

bool http_parse_date(struct http_str s, time_t *out)
{
    /* RFC 9110 §5.6.7: recipients must accept all three historical formats. */
    static const char *const formats[] = {
        "%a, %d %b %Y %H:%M:%S GMT", /* IMF-fixdate */
        "%A, %d-%b-%y %H:%M:%S GMT", /* RFC 850 */
        "%a %b %d %H:%M:%S %Y",      /* asctime(); the space before %d also eats padding */
    };
    char tmp[64];

    if (s.len >= sizeof tmp)
        return false;
    memcpy(tmp, s.p, s.len);
    tmp[s.len] = '\0';

    for (size_t i = 0; i < sizeof formats / sizeof *formats; i++) {
        struct tm tm = {0};
        const char *end = strptime(tmp, formats[i], &tm);
        if (end && *end == '\0') {
            *out = timegm(&tm);
            return true;
        }
    }
    return false;
}
