#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "buf.h"
#include "check.h"
#include "http_parser.h"

#define BUF_SIZE (HTTP_MAX_HEAD + HTTP_MAX_BODY + 4096)

#define DONE HTTP_PARSE_DONE
#define INCOMPLETE HTTP_PARSE_INCOMPLETE
#define ERROR HTTP_PARSE_ERROR

struct parse_case {
    const char *name;
    const char *input;
    enum http_parse_result result;
    int status; /* for ERROR */
    const char *method, *target;
    int minor;
    bool keep_alive;
    const char *body;
    const char *header, *value; /* a field that must be present with this value */
    const char *absent;         /* a field that must not be present */
};

#define HOST "Host: a\r\n"

static const struct parse_case cases[] = {
    {.name = "minimal GET", .input = "GET / HTTP/1.1\r\n" HOST "\r\n", .result = DONE,
     .method = "GET", .target = "/", .minor = 1, .keep_alive = true, .body = "",
     .header = "host", .value = "a"},
    {.name = "HTTP/1.0 needs no Host and closes", .input = "GET /x HTTP/1.0\r\n\r\n",
     .result = DONE, .method = "GET", .target = "/x", .minor = 0, .keep_alive = false, .body = ""},
    {.name = "HTTP/1.0 keep-alive", .input = "GET / HTTP/1.0\r\nConnection: Keep-Alive\r\n\r\n",
     .result = DONE, .method = "GET", .target = "/", .minor = 0, .keep_alive = true, .body = ""},
    {.name = "Connection: close in a list",
     .input = "GET / HTTP/1.1\r\n" HOST "Connection: foo, close\r\n\r\n", .result = DONE,
     .method = "GET", .target = "/", .minor = 1, .keep_alive = false, .body = ""},
    {.name = "leading empty lines", .input = "\r\n\r\nGET / HTTP/1.1\r\n" HOST "\r\n",
     .result = DONE, .method = "GET", .target = "/", .minor = 1, .keep_alive = true, .body = ""},
    {.name = "OWS around value trimmed", .input = "GET / HTTP/1.1\r\n" HOST "X-Pad: \t a b \t\r\n\r\n",
     .result = DONE, .method = "GET", .target = "/", .minor = 1, .keep_alive = true, .body = "",
     .header = "X-Pad", .value = "a b"},
    {.name = "empty value", .input = "GET / HTTP/1.1\r\n" HOST "X-Empty:\r\n\r\n", .result = DONE,
     .method = "GET", .target = "/", .minor = 1, .keep_alive = true, .body = "",
     .header = "x-empty", .value = ""},
    {.name = "obs-text in value", .input = "GET / HTTP/1.1\r\n" HOST "X-Latin: caf\xe9\r\n\r\n",
     .result = DONE, .method = "GET", .target = "/", .minor = 1, .keep_alive = true, .body = "",
     .header = "X-Latin", .value = "caf\xe9"},
    {.name = "absolute-form target", .input = "GET http://a/b?c HTTP/1.1\r\n" HOST "\r\n",
     .result = DONE, .method = "GET", .target = "http://a/b?c", .minor = 1, .keep_alive = true,
     .body = ""},
    {.name = "unknown method is still a token", .input = "BREW /pot HTTP/1.1\r\n" HOST "\r\n",
     .result = DONE, .method = "BREW", .target = "/pot", .minor = 1, .keep_alive = true, .body = ""},
    {.name = "higher 1.x minor", .input = "GET / HTTP/1.2\r\n" HOST "\r\n", .result = DONE,
     .method = "GET", .target = "/", .minor = 2, .keep_alive = true, .body = ""},
    {.name = "Content-Length body",
     .input = "POST /e HTTP/1.1\r\n" HOST "Content-Length: 5\r\n\r\nhello", .result = DONE,
     .method = "POST", .target = "/e", .minor = 1, .keep_alive = true, .body = "hello"},
    {.name = "Content-Length zero", .input = "POST /e HTTP/1.1\r\n" HOST "Content-Length: 0\r\n\r\n",
     .result = DONE, .method = "POST", .target = "/e", .minor = 1, .keep_alive = true, .body = ""},
    {.name = "chunked body with extensions",
     .input = "POST /e HTTP/1.1\r\n" HOST "Transfer-Encoding: chunked\r\n\r\n"
              "5\r\nhello\r\n6;name=\"v\"\r\n world\r\nA\r\n0123456789\r\n0\r\n\r\n",
     .result = DONE, .method = "POST", .target = "/e", .minor = 1, .keep_alive = true,
     .body = "hello world0123456789"},
    {.name = "chunked trailers are discarded",
     .input = "POST /e HTTP/1.1\r\n" HOST "Transfer-Encoding: Chunked\r\n\r\n"
              "3\r\nabc\r\n000\r\nX-Sum: 1\r\nX-More: 2\r\n\r\n",
     .result = DONE, .method = "POST", .target = "/e", .minor = 1, .keep_alive = true,
     .body = "abc", .absent = "X-Sum"},
    {.name = "incomplete head", .input = "GET / HTTP/1.1\r\n" HOST, .result = INCOMPLETE},
    {.name = "incomplete body", .input = "POST / HTTP/1.1\r\n" HOST "Content-Length: 10\r\n\r\nabc",
     .result = INCOMPLETE},
    {.name = "incomplete chunked", .input = "POST / HTTP/1.1\r\n" HOST
                                            "Transfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n",
     .result = INCOMPLETE},

    {.name = "bare LF", .input = "GET / HTTP/1.1\nHost: a\n\n", .result = ERROR, .status = 400},
    {.name = "bare CR in value", .input = "GET / HTTP/1.1\r\nHost: a\rb\r\n\r\n", .result = ERROR,
     .status = 400},
    {.name = "space before colon", .input = "GET / HTTP/1.1\r\nHost : a\r\n\r\n", .result = ERROR,
     .status = 400},
    {.name = "obs-fold", .input = "GET / HTTP/1.1\r\n" HOST "X: 1\r\n  2\r\n\r\n", .result = ERROR,
     .status = 400},
    {.name = "empty field name", .input = "GET / HTTP/1.1\r\n" HOST ": x\r\n\r\n", .result = ERROR,
     .status = 400},
    {.name = "missing Host in 1.1", .input = "GET / HTTP/1.1\r\n\r\n", .result = ERROR, .status = 400},
    {.name = "duplicate Host", .input = "GET / HTTP/1.1\r\n" HOST HOST "\r\n", .result = ERROR,
     .status = 400},
    {.name = "Content-Length with Transfer-Encoding",
     .input = "POST / HTTP/1.1\r\n" HOST "Content-Length: 3\r\nTransfer-Encoding: chunked\r\n\r\n",
     .result = ERROR, .status = 400},
    {.name = "duplicate Content-Length",
     .input = "POST / HTTP/1.1\r\n" HOST "Content-Length: 1\r\nContent-Length: 1\r\n\r\na",
     .result = ERROR, .status = 400},
    {.name = "Content-Length not a number",
     .input = "POST / HTTP/1.1\r\n" HOST "Content-Length: 5a\r\n\r\n", .result = ERROR, .status = 400},
    {.name = "negative Content-Length", .input = "POST / HTTP/1.1\r\n" HOST "Content-Length: -1\r\n\r\n",
     .result = ERROR, .status = 400},
    {.name = "empty Content-Length", .input = "POST / HTTP/1.1\r\n" HOST "Content-Length:\r\n\r\n",
     .result = ERROR, .status = 400},
    {.name = "Content-Length over limit",
     .input = "POST / HTTP/1.1\r\n" HOST "Content-Length: 8193\r\n\r\n", .result = ERROR, .status = 413},
    {.name = "Content-Length overflow",
     .input = "POST / HTTP/1.1\r\n" HOST "Content-Length: 999999999999999999999999\r\n\r\n",
     .result = ERROR, .status = 413},
    {.name = "unknown transfer coding",
     .input = "POST / HTTP/1.1\r\n" HOST "Transfer-Encoding: gzip\r\n\r\n", .result = ERROR,
     .status = 400},
    {.name = "chunked not final", .input = "POST / HTTP/1.1\r\n" HOST
                                           "Transfer-Encoding: chunked, gzip\r\n\r\n",
     .result = ERROR, .status = 400},
    {.name = "coding stacked under chunked",
     .input = "POST / HTTP/1.1\r\n" HOST "Transfer-Encoding: gzip, chunked\r\n\r\n", .result = ERROR,
     .status = 501},
    {.name = "Transfer-Encoding in 1.0",
     .input = "POST / HTTP/1.0\r\nTransfer-Encoding: chunked\r\n\r\n", .result = ERROR, .status = 400},
    {.name = "HTTP/2.0", .input = "GET / HTTP/2.0\r\n" HOST "\r\n", .result = ERROR, .status = 505},
    {.name = "version not a digit", .input = "GET / HTTP/1.x\r\n" HOST "\r\n", .result = ERROR,
     .status = 400},
    {.name = "lowercase protocol", .input = "GET / http/1.1\r\n" HOST "\r\n", .result = ERROR,
     .status = 400},
    {.name = "two-digit major", .input = "GET / HTTP/11.1\r\n" HOST "\r\n", .result = ERROR,
     .status = 400},
    {.name = "space after version", .input = "GET / HTTP/1.1 \r\n" HOST "\r\n", .result = ERROR,
     .status = 400},
    {.name = "double space after method", .input = "GET  / HTTP/1.1\r\n" HOST "\r\n",
     .result = ERROR, .status = 400},
    {.name = "missing version", .input = "GET /\r\n" HOST "\r\n", .result = ERROR, .status = 400},
    {.name = "control byte in target", .input = "GET /a\x01 HTTP/1.1\r\n" HOST "\r\n",
     .result = ERROR, .status = 400},
    {.name = "bad method char", .input = "G(T / HTTP/1.1\r\n" HOST "\r\n", .result = ERROR,
     .status = 400},
    {.name = "bad chunk size", .input = "POST / HTTP/1.1\r\n" HOST
                                        "Transfer-Encoding: chunked\r\n\r\nzz\r\n",
     .result = ERROR, .status = 400},
    {.name = "chunk data overruns size",
     .input = "POST / HTTP/1.1\r\n" HOST "Transfer-Encoding: chunked\r\n\r\n3\r\nabcd\r\n",
     .result = ERROR, .status = 400},
    {.name = "chunk size with bare LF",
     .input = "POST / HTTP/1.1\r\n" HOST "Transfer-Encoding: chunked\r\n\r\n3\nabc\r\n",
     .result = ERROR, .status = 400},
    {.name = "chunk over limit",
     .input = "POST / HTTP/1.1\r\n" HOST "Transfer-Encoding: chunked\r\n\r\n2001\r\n",
     .result = ERROR, .status = 413},
};

static void run_case(const struct parse_case *tc)
{
    static char buf[BUF_SIZE];
    struct http_parser p;
    struct http_request req;
    size_t len = strlen(tc->input);

    memcpy(buf, tc->input, len);
    http_parser_init(&p, &req);
    enum http_parse_result r = http_parse(&p, buf, &len);

    CHECK(r == tc->result, tc->name);
    if (r == ERROR && tc->result == ERROR)
        CHECK(p.status == tc->status, tc->name);
    if (r != DONE || tc->result != DONE)
        return;
    CHECK(http_str_eq(req.method, tc->method), tc->name);
    CHECK(http_str_eq(req.target, tc->target), tc->name);
    CHECK(req.minor == tc->minor, tc->name);
    CHECK(req.keep_alive == tc->keep_alive, tc->name);
    CHECK(http_str_eq(req.body, tc->body), tc->name);
    CHECK(p.pos == len, tc->name);
    if (tc->header) {
        const struct http_str *v = http_header_get(&req, tc->header);
        CHECK(v && http_str_eq(*v, tc->value), tc->name);
    }
    if (tc->absent)
        CHECK(http_header_get(&req, tc->absent) == NULL, tc->name);
}

/* Parse a single buffer and return the outcome; for generated inputs. */
static enum http_parse_result parse_once(const char *input, size_t len, int *status)
{
    static char buf[BUF_SIZE * 2];
    struct http_parser p;
    struct http_request req;

    memcpy(buf, input, len);
    http_parser_init(&p, &req);
    enum http_parse_result r = http_parse(&p, buf, &len);
    *status = p.status;
    return r;
}

static void expect_error(const char *what, struct buf *input, int status)
{
    int got;
    CHECK(!input->failed, what);
    CHECK(parse_once(input->data, input->len, &got) == ERROR && got == status, what);
    buf_free(input);
}

static void test_limits(void)
{
    struct buf b = {0};

    buf_puts(&b, "GET /");
    for (int i = 0; i < HTTP_MAX_TARGET; i++)
        buf_puts(&b, "a");
    buf_puts(&b, " HTTP/1.1\r\n" HOST "\r\n");
    expect_error("target too long", &b, 414);

    buf_puts(&b, "GET / HTTP/1.1\r\n" HOST "X-Big: ");
    for (int i = 0; i < HTTP_MAX_HEAD; i++)
        buf_puts(&b, "a");
    buf_puts(&b, "\r\n\r\n");
    expect_error("head too large", &b, 431);

    buf_puts(&b, "GET / HTTP/1.1\r\n" HOST);
    for (int i = 0; i < HTTP_MAX_HEADERS; i++)
        buf_printf(&b, "X-%d: v\r\n", i);
    buf_puts(&b, "\r\n");
    expect_error("too many fields", &b, 431);

    for (int i = 0; i <= HTTP_MAX_METHOD; i++)
        buf_puts(&b, "M");
    buf_puts(&b, " / HTTP/1.1\r\n" HOST "\r\n");
    expect_error("method too long", &b, 501);

    buf_puts(&b, "POST / HTTP/1.1\r\n" HOST "Transfer-Encoding: chunked\r\n\r\n");
    for (int i = 0; i < 3; i++) {
        buf_printf(&b, "%x\r\n", HTTP_MAX_BODY / 2);
        for (int j = 0; j < HTTP_MAX_BODY / 2; j++)
            buf_puts(&b, "z");
        buf_puts(&b, "\r\n");
    }
    expect_error("chunks add up past the body limit", &b, 413);

    buf_puts(&b, "POST / HTTP/1.1\r\n" HOST "Transfer-Encoding: chunked\r\n\r\n1;");
    for (int i = 0; i < HTTP_MAX_CHUNK_LINE; i++)
        buf_puts(&b, "e");
    buf_puts(&b, "\r\nx\r\n0\r\n\r\n");
    expect_error("chunk extension too long", &b, 400);

    buf_puts(&b, "POST / HTTP/1.1\r\n" HOST "Transfer-Encoding: chunked\r\n\r\n0\r\n");
    for (int i = 0; i < HTTP_MAX_HEAD / 8; i++)
        buf_puts(&b, "T: 12345\r\n");
    buf_puts(&b, "\r\n");
    expect_error("trailers too large", &b, 431);

    static const char nul[] = "GET / HTTP/1.1\r\n" HOST "X: a\0b\r\n\r\n";
    int status;
    CHECK(parse_once(nul, sizeof nul - 1, &status) == ERROR && status == 400, "NUL in value");

    /* Exactly at the limits is fine. */
    buf_puts(&b, "POST / HTTP/1.1\r\n" HOST);
    buf_printf(&b, "Content-Length: %d\r\n\r\n", HTTP_MAX_BODY);
    for (int i = 0; i < HTTP_MAX_BODY; i++)
        buf_puts(&b, "b");
    CHECK(parse_once(b.data, b.len, &status) == DONE, "body at the limit");
    buf_free(&b);
}

static void test_expect_continue(void)
{
    char in[] = "POST / HTTP/1.1\r\n" HOST "Expect: 100-Continue\r\nContent-Length: 3\r\n\r\n";
    size_t len = sizeof in - 1;
    struct http_parser p;
    struct http_request req;

    http_parser_init(&p, &req);
    CHECK(http_parse(&p, in, &len) == INCOMPLETE, "expect continue");
    CHECK(req.expect_continue, "expect continue");
    CHECK(p.head_len == len, "expect continue: head parsed");

    char old[] = "POST / HTTP/1.0\r\nExpect: 100-continue\r\nContent-Length: 3\r\n\r\n";
    len = sizeof old - 1;
    http_parser_init(&p, &req);
    CHECK(http_parse(&p, old, &len) == INCOMPLETE && !req.expect_continue, "1.0 ignores Expect");
}

/* --- Split-point invariance --------------------------------------------------
 * The same bytes must parse identically however they are chopped up by the
 * network. Each run feeds an input in chunks chosen by `next_chunk` and
 * records a transcript of every outcome, including pipelined follow-ups. */

static uint64_t rng_state;

static uint64_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

static size_t chunk_all(void) { return SIZE_MAX; }
static size_t chunk_byte(void) { return 1; }
static size_t chunk_small(void) { return 1 + (size_t)(rng() % 16); }
static size_t chunk_large(void) { return 1 + (size_t)(rng() % 512); }

static void describe(struct buf *out, const struct http_parser *p, const struct http_request *req)
{
    buf_printf(out, "DONE %zu %.*s %.*s 1.%d ka=%d ec=%d", p->pos, (int)req->method.len,
               req->method.p, (int)req->target.len, req->target.p, req->minor, req->keep_alive,
               req->expect_continue);
    for (size_t i = 0; i < req->n_headers; i++)
        buf_printf(out, " [%.*s=%.*s]", (int)req->headers[i].name.len, req->headers[i].name.p,
                   (int)req->headers[i].value.len, req->headers[i].value.p);
    buf_puts(out, " body=");
    buf_append(out, req->body.p, req->body.len);
    buf_puts(out, "\n");
}

static void transcript(const char *in, size_t total, size_t (*next_chunk)(void), struct buf *out)
{
    static char buf[BUF_SIZE];
    size_t len = 0, fed = 0;
    struct http_parser p;
    struct http_request req;

    http_parser_init(&p, &req);
    for (;;) {
        enum http_parse_result r = http_parse(&p, buf, &len);
        if (r == ERROR) {
            buf_printf(out, "ERROR %d\n", p.status);
            return;
        }
        if (r == DONE) {
            describe(out, &p, &req);
            len -= p.pos;
            memmove(buf, buf + p.pos, len);
            http_parser_init(&p, &req);
            continue;
        }
        if (fed == total) {
            buf_puts(out, "INCOMPLETE\n");
            return;
        }
        size_t n = next_chunk();
        if (n > total - fed)
            n = total - fed;
        if (n > BUF_SIZE - len)
            n = BUF_SIZE - len;
        if (n == 0) {
            buf_puts(out, "BUFFER FULL\n");
            return;
        }
        memcpy(buf + len, in + fed, n);
        len += n;
        fed += n;
    }
}

static void check_split_invariance(const char *name, const char *in, size_t len)
{
    struct buf want = {0}, got = {0};

    transcript(in, len, chunk_all, &want);
    transcript(in, len, chunk_byte, &got);
    CHECK(!got.failed && got.len == want.len && memcmp(got.data, want.data, got.len) == 0, name);
    for (int i = 0; i < 300; i++) {
        uint64_t seed = rng_state;
        got.len = 0;
        transcript(in, len, i % 2 ? chunk_small : chunk_large, &got);
        bool same = got.len == want.len && memcmp(got.data, want.data, got.len) == 0;
        CHECK(same, name);
        if (!same) {
            fprintf(stderr, "  seed %llu\n  want: %.*s  got:  %.*s", (unsigned long long)seed,
                    (int)want.len, want.data, (int)got.len, got.data);
            break;
        }
    }
    buf_free(&want);
    buf_free(&got);
}

static void test_split_invariance(void)
{
    static const char pipeline[] =
        "GET /a HTTP/1.1\r\nHost: x\r\n\r\n"
        "POST /b HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\nabc"
        "\r\n"
        "PUT /c HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n"
        "4;x=y\r\nwiki\r\n5\r\npedia\r\n0\r\nT: v\r\n\r\n"
        "GET /d HTTP/1.0\r\n\r\n"
        "POST /e HTTP/1.1\r\nHost: x\r\nExpect: 100-continue\r\nContent-Length: 2\r\n\r\nok"
        "GET /f HTTP/1.1\r\nHost: x\r\n";
    static const char pipeline_error[] = "GET / HTTP/1.1\r\nHost: x\r\n\r\n"
                                         "GET /bad HTTP/1.1\r\nHost : x\r\n\r\n";

    rng_state = 0x9e3779b97f4a7c15u;
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++)
        check_split_invariance(cases[i].name, cases[i].input, strlen(cases[i].input));
    check_split_invariance("pipeline", pipeline, sizeof pipeline - 1);
    check_split_invariance("pipeline ending in error", pipeline_error, sizeof pipeline_error - 1);

    /* Sanity-check the reference transcript itself. */
    struct buf t = {0};
    transcript(pipeline, sizeof pipeline - 1, chunk_all, &t);
    buf_append(&t, "", 1);
    CHECK(!t.failed, "transcript");
    CHECK(strstr(t.data, " PUT /c 1.1 ka=1 ec=0 [Host=x] [Transfer-Encoding=chunked] body=wikipedia\n"),
          "pipelined chunked request");
    CHECK(strstr(t.data, " POST /e 1.1 ka=1 ec=1") != NULL, "pipelined expect");
    CHECK(strstr(t.data, " GET /d 1.0 ka=0") != NULL, "pipelined 1.0");
    CHECK(t.len > 11 && strcmp(t.data + t.len - 12, "INCOMPLETE\n") == 0, "pipeline tail");
    buf_free(&t);
}

int main(void)
{
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++)
        run_case(&cases[i]);
    test_limits();
    test_expect_continue();
    test_split_invariance();
    return check_summary("test_parser");
}
