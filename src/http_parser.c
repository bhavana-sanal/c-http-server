#include "http_parser.h"

#include <string.h>

enum {
    S_START,        /* before the request line; skipping empty lines */
    S_START_LF,
    S_METHOD,
    S_TARGET_START,
    S_TARGET,
    S_VERSION,
    S_REQLINE_LF,
    S_FIELD_START,  /* start of a field line, or the empty line ending the section */
    S_FIELD_NAME,
    S_FIELD_OWS,
    S_FIELD_VALUE,
    S_FIELD_LF,
    S_HEAD_LF,
    S_BODY,         /* Content-Length body */
    S_CHUNK_START,
    S_CHUNK_SIZE,
    S_CHUNK_EXT,
    S_CHUNK_SIZE_LF,
    S_CHUNK_DATA,
    S_CHUNK_DATA_CR,
    S_CHUNK_DATA_LF,
    S_DONE,
    S_ERROR,
};

/* tchar from RFC 9110 §5.6.2 */
static bool is_tchar(unsigned char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c != '\0' && strchr("!#$%&'*+-.^_`|~", c) != NULL);
}

static bool is_vchar(unsigned char c)
{
    return c > 0x20 && c < 0x7f;
}

/* field-vchar, obs-text, SP and HTAB (RFC 9110 §5.5). Rejects CR, LF and NUL. */
static bool is_field_char(unsigned char c)
{
    return c == '\t' || (c >= 0x20 && c != 0x7f);
}

static char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

int http_hex_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    c = lower(c);
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

bool http_str_eq(struct http_str s, const char *lit)
{
    size_t n = strlen(lit);
    return s.len == n && memcmp(s.p, lit, n) == 0;
}

bool http_str_ieq(struct http_str s, const char *lit)
{
    if (s.len != strlen(lit))
        return false;
    for (size_t i = 0; i < s.len; i++)
        if (lower(s.p[i]) != lower(lit[i]))
            return false;
    return true;
}

struct http_str http_str_trim(struct http_str s)
{
    while (s.len > 0 && (s.p[0] == ' ' || s.p[0] == '\t')) {
        s.p++;
        s.len--;
    }
    while (s.len > 0 && (s.p[s.len - 1] == ' ' || s.p[s.len - 1] == '\t'))
        s.len--;
    return s;
}

const struct http_str *http_header_get(const struct http_request *req, const char *name)
{
    for (size_t i = 0; i < req->n_headers; i++)
        if (http_str_ieq(req->headers[i].name, name))
            return &req->headers[i].value;
    return NULL;
}

/* Does a comma-separated field value contain `token` (case-insensitively)? */
static bool list_has(struct http_str list, const char *token)
{
    const char *p = list.p, *end = list.p + list.len;
    for (;;) {
        const char *comma = memchr(p, ',', (size_t)(end - p));
        const char *stop = comma ? comma : end;
        if (http_str_ieq(http_str_trim((struct http_str){p, (size_t)(stop - p)}), token))
            return true;
        if (!comma)
            return false;
        p = comma + 1;
    }
}

static struct http_str slice(const char *buf, size_t from, size_t to)
{
    return (struct http_str){buf + from, to - from};
}

void http_parser_init(struct http_parser *p, struct http_request *req)
{
    *p = (struct http_parser){.req = req, .state = S_START};
    *req = (struct http_request){.minor = -1};
}

static enum http_parse_result fail(struct http_parser *p, int status)
{
    p->state = S_ERROR;
    p->status = status;
    return HTTP_PARSE_ERROR;
}

static enum http_parse_result done(struct http_parser *p, const char *buf)
{
    size_t end = p->chunked ? p->dst : p->pos;
    p->req->body = slice(buf, p->head_len, end);
    p->state = S_DONE;
    return HTTP_PARSE_DONE;
}

/* HTTP-version = "HTTP/" DIGIT "." DIGIT (RFC 9112 §2.3) */
static enum http_parse_result parse_version(struct http_parser *p, struct http_str v)
{
    if (v.len != 8 || memcmp(v.p, "HTTP/", 5) != 0 || v.p[5] < '0' || v.p[5] > '9' ||
        v.p[6] != '.' || v.p[7] < '0' || v.p[7] > '9')
        return fail(p, 400);
    if (v.p[5] != '1')
        return fail(p, 505);
    p->req->minor = v.p[7] - '0';
    return HTTP_PARSE_INCOMPLETE;
}

/* Content-Length = 1*DIGIT; saturates so oversized values still map to 413. */
static bool parse_length(struct http_str v, uint64_t *out)
{
    uint64_t n = 0;
    if (v.len == 0)
        return false;
    for (size_t i = 0; i < v.len; i++) {
        if (v.p[i] < '0' || v.p[i] > '9')
            return false;
        unsigned d = (unsigned)(v.p[i] - '0');
        n = n > (UINT64_MAX - d) / 10 ? UINT64_MAX : n * 10 + d;
    }
    *out = n;
    return true;
}

static enum http_parse_result start_chunked(struct http_parser *p, struct http_str te)
{
    /* Only the last coding matters for framing (RFC 9112 §6.3 rule 4): if it
     * isn't chunked the body length is unknowable. Codings layered under
     * chunked (gzip, chunked) are valid but not something we decode. */
    const char *last = te.p;
    for (size_t i = 0; i < te.len; i++)
        if (te.p[i] == ',')
            last = te.p + i + 1;
    struct http_str coding = http_str_trim((struct http_str){last, (size_t)(te.p + te.len - last)});
    if (!http_str_ieq(coding, "chunked"))
        return fail(p, 400);
    if (last != te.p)
        return fail(p, 501);

    p->chunked = true;
    p->dst = p->head_len;
    p->budget = HTTP_MAX_CHUNK_LINE;
    p->state = S_CHUNK_START;
    return HTTP_PARSE_INCOMPLETE;
}

/* Validate the header section and decide how the body is framed (RFC 9112 §6.3). */
static enum http_parse_result finish_head(struct http_parser *p)
{
    struct http_request *req = p->req;
    const struct http_str *length = NULL, *coding = NULL;
    size_t hosts = 0;
    bool want_close = false, want_keep_alive = false;

    for (size_t i = 0; i < req->n_headers; i++) {
        const struct http_header *h = &req->headers[i];
        if (http_str_ieq(h->name, "Host")) {
            hosts++;
        } else if (http_str_ieq(h->name, "Content-Length")) {
            if (length)
                return fail(p, 400);
            length = &h->value;
        } else if (http_str_ieq(h->name, "Transfer-Encoding")) {
            if (coding)
                return fail(p, 400);
            coding = &h->value;
        } else if (http_str_ieq(h->name, "Connection")) {
            want_close |= list_has(h->value, "close");
            want_keep_alive |= list_has(h->value, "keep-alive");
        } else if (http_str_ieq(h->name, "Expect")) {
            /* RFC 9110 §10.1.1: HTTP/1.0 clients don't understand 100 Continue. */
            req->expect_continue = req->minor >= 1 && http_str_ieq(h->value, "100-continue");
        }
    }

    /* RFC 9112 §3.2: exactly one Host in HTTP/1.1, never more than one. */
    if (hosts > 1 || (hosts == 0 && req->minor >= 1))
        return fail(p, 400);
    /* RFC 9112 §9.3: 1.1 is persistent unless told otherwise, 1.0 the reverse. */
    req->keep_alive = !want_close && (req->minor >= 1 || want_keep_alive);

    if (coding) {
        /* Both framings at once, or Transfer-Encoding in 1.0, is the classic
         * request smuggling setup; RFC 9112 §6.1 and §6.3 let us refuse it. */
        if (length || req->minor == 0)
            return fail(p, 400);
        return start_chunked(p, *coding);
    }
    if (length) {
        if (!parse_length(*length, &p->remaining))
            return fail(p, 400);
        if (p->remaining > HTTP_MAX_BODY)
            return fail(p, 413);
    }
    p->state = p->remaining > 0 ? S_BODY : S_DONE;
    return HTTP_PARSE_INCOMPLETE;
}

static enum http_parse_result step(struct http_parser *p, char *buf, size_t len)
{
    struct http_request *req = p->req;

    while (p->pos < len) {
        if (p->state == S_BODY || p->state == S_CHUNK_DATA) {
            size_t n = len - p->pos;
            if (n > p->remaining)
                n = (size_t)p->remaining;
            if (p->state == S_CHUNK_DATA) {
                memmove(buf + p->dst, buf + p->pos, n);
                p->dst += n;
            }
            p->pos += n;
            p->remaining -= n;
            if (p->remaining == 0) {
                if (p->state == S_BODY)
                    return done(p, buf);
                p->state = S_CHUNK_DATA_CR;
            }
            continue;
        }

        if (p->head_len == 0 && p->pos >= HTTP_MAX_HEAD)
            return fail(p, p->state == S_TARGET ? 414 : 431);
        if (p->head_len != 0) {
            if (p->budget == 0)
                return fail(p, p->trailers ? 431 : 400);
            p->budget--;
        }

        size_t i = p->pos++;
        unsigned char c = (unsigned char)buf[i];

        switch (p->state) {
        case S_START:
            /* RFC 9112 §2.2: tolerate empty lines before the request line. */
            if (c == '\r') {
                p->state = S_START_LF;
            } else if (is_tchar(c)) {
                p->mark = i;
                p->state = S_METHOD;
            } else {
                return fail(p, 400);
            }
            break;
        case S_START_LF:
            if (c != '\n')
                return fail(p, 400);
            p->state = S_START;
            break;
        case S_METHOD:
            if (c == ' ') {
                req->method = slice(buf, p->mark, i);
                p->state = S_TARGET_START;
            } else if (!is_tchar(c)) {
                return fail(p, 400);
            } else if (i - p->mark >= HTTP_MAX_METHOD) {
                return fail(p, 501);
            }
            break;
        case S_TARGET_START:
            if (!is_vchar(c))
                return fail(p, 400);
            p->mark = i;
            p->state = S_TARGET;
            break;
        case S_TARGET:
            if (c == ' ') {
                req->target = slice(buf, p->mark, i);
                p->mark = p->pos;
                p->state = S_VERSION;
            } else if (!is_vchar(c)) {
                return fail(p, 400);
            } else if (i - p->mark >= HTTP_MAX_TARGET) {
                return fail(p, 414);
            }
            break;
        case S_VERSION:
            if (c == '\r') {
                if (parse_version(p, slice(buf, p->mark, i)) == HTTP_PARSE_ERROR)
                    return HTTP_PARSE_ERROR;
                p->state = S_REQLINE_LF;
            } else if (!is_vchar(c) || i - p->mark >= 8) {
                return fail(p, 400);
            }
            break;
        case S_REQLINE_LF:
            /* Bare LF line endings are a smuggling vector; RFC 9112 §2.2 lets us refuse them. */
            if (c != '\n')
                return fail(p, 400);
            p->state = S_FIELD_START;
            break;
        case S_FIELD_START:
            if (c == '\r') {
                p->state = S_HEAD_LF;
            } else if (is_tchar(c)) {
                if (!p->trailers && req->n_headers == HTTP_MAX_HEADERS)
                    return fail(p, 431);
                p->mark = i;
                p->state = S_FIELD_NAME;
            } else {
                /* Includes obs-fold continuation lines (RFC 9112 §5.2). */
                return fail(p, 400);
            }
            break;
        case S_FIELD_NAME:
            if (c == ':') {
                if (!p->trailers)
                    req->headers[req->n_headers].name = slice(buf, p->mark, i);
                p->state = S_FIELD_OWS;
            } else if (!is_tchar(c)) {
                /* Includes whitespace before the colon (RFC 9112 §5.1). */
                return fail(p, 400);
            }
            break;
        case S_FIELD_OWS:
            if (c == ' ' || c == '\t')
                break;
            p->mark = p->value_end = i;
            if (c == '\r') {
                p->state = S_FIELD_LF;
            } else if (is_field_char(c)) {
                p->value_end = i + 1;
                p->state = S_FIELD_VALUE;
            } else {
                return fail(p, 400);
            }
            break;
        case S_FIELD_VALUE:
            if (c == '\r')
                p->state = S_FIELD_LF;
            else if (!is_field_char(c))
                return fail(p, 400);
            else if (c != ' ' && c != '\t')
                p->value_end = i + 1;
            break;
        case S_FIELD_LF:
            if (c != '\n')
                return fail(p, 400);
            if (!p->trailers)
                req->headers[req->n_headers++].value = slice(buf, p->mark, p->value_end);
            p->state = S_FIELD_START;
            break;
        case S_HEAD_LF:
            if (c != '\n')
                return fail(p, 400);
            if (p->trailers)
                return done(p, buf);
            p->head_len = p->pos;
            if (finish_head(p) == HTTP_PARSE_ERROR)
                return HTTP_PARSE_ERROR;
            if (p->state == S_DONE)
                return done(p, buf);
            break;
        case S_CHUNK_START:
        case S_CHUNK_SIZE: {
            int digit = http_hex_value((char)c);
            if (digit >= 0) {
                p->remaining = p->remaining * 16 + (unsigned)digit;
                if (p->remaining > HTTP_MAX_BODY - (p->dst - p->head_len))
                    return fail(p, 413);
                p->state = S_CHUNK_SIZE;
            } else if (p->state == S_CHUNK_START) {
                return fail(p, 400);
            } else if (c == ';' || c == ' ' || c == '\t') {
                p->state = S_CHUNK_EXT;
            } else if (c == '\r') {
                p->state = S_CHUNK_SIZE_LF;
            } else {
                return fail(p, 400);
            }
            break;
        }
        case S_CHUNK_EXT:
            /* Extensions carry nothing we use; skip them, bounded by the line budget. */
            if (c == '\r')
                p->state = S_CHUNK_SIZE_LF;
            else if (!is_field_char(c))
                return fail(p, 400);
            break;
        case S_CHUNK_SIZE_LF:
            if (c != '\n')
                return fail(p, 400);
            if (p->remaining == 0) {
                /* last-chunk: parse and discard the trailer section (RFC 9112 §7.1.2). */
                p->trailers = true;
                p->budget = HTTP_MAX_HEAD;
                p->state = S_FIELD_START;
            } else {
                p->state = S_CHUNK_DATA;
            }
            break;
        case S_CHUNK_DATA_CR:
            if (c != '\r')
                return fail(p, 400);
            p->state = S_CHUNK_DATA_LF;
            break;
        case S_CHUNK_DATA_LF:
            if (c != '\n')
                return fail(p, 400);
            p->budget = HTTP_MAX_CHUNK_LINE;
            p->state = S_CHUNK_START;
            break;
        }
    }
    return HTTP_PARSE_INCOMPLETE;
}

enum http_parse_result http_parse(struct http_parser *p, char *buf, size_t *len)
{
    if (p->state == S_DONE)
        return HTTP_PARSE_DONE;
    if (p->state == S_ERROR)
        return HTTP_PARSE_ERROR;

    enum http_parse_result r = step(p, buf, *len);

    if (p->chunked && r != HTTP_PARSE_ERROR && p->dst < p->pos) {
        /* Squeeze out the chunk framing consumed so far, so the buffer holds
         * head + decoded body + unparsed input and never fills up with framing. */
        memmove(buf + p->dst, buf + p->pos, *len - p->pos);
        *len -= p->pos - p->dst;
        p->pos = p->dst;
    }
    return r;
}
