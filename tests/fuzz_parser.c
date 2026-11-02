/*
 * libFuzzer target for the request parser.
 *
 * Besides memory errors (caught by ASan/UBSan), it checks a differential
 * property: the input parsed as one buffer and the same input delivered in
 * two pieces, split at a point chosen by the fuzzer, must produce identical
 * results for every pipelined request.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "http_parser.h"

#define BUF_SIZE (HTTP_MAX_HEAD + HTTP_MAX_BODY + 4096)

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static uint64_t mix(uint64_t h, const void *p, size_t n)
{
    const unsigned char *b = p;
    for (size_t i = 0; i < n; i++)
        h = (h ^ b[i]) * 1099511628211u; /* FNV-1a */
    return h;
}

static uint64_t mix_str(uint64_t h, struct http_str s, const char *buf, size_t len)
{
    /* Every slice must point inside the buffer. */
    if (s.len > 0 && (s.p < buf || s.p + s.len > buf + len))
        abort();
    h = mix(h, &s.len, sizeof s.len);
    return mix(h, s.p, s.len);
}

/* Feed `data` in two pieces at `split`, return a digest of all outcomes. */
static uint64_t run(const uint8_t *data, size_t size, size_t split)
{
    static char buf[BUF_SIZE];
    struct http_parser p;
    struct http_request req;
    size_t len = 0, fed = 0;
    uint64_t h = 14695981039346656037u;

    http_parser_init(&p, &req);
    for (;;) {
        enum http_parse_result r = http_parse(&p, buf, &len);
        /* INCOMPLETE is not recorded: how often it happens depends on the split. */
        if (r == HTTP_PARSE_ERROR)
            return mix(h, &p.status, sizeof p.status);
        if (r == HTTP_PARSE_DONE) {
            if (p.pos > len)
                abort();
            h = mix_str(h, req.method, buf, len);
            h = mix_str(h, req.target, buf, len);
            h = mix(h, &req.minor, sizeof req.minor);
            for (size_t i = 0; i < req.n_headers; i++) {
                h = mix_str(h, req.headers[i].name, buf, len);
                h = mix_str(h, req.headers[i].value, buf, len);
            }
            h = mix_str(h, req.body, buf, len);
            h = mix(h, &p.pos, sizeof p.pos);
            len -= p.pos;
            memmove(buf, buf + p.pos, len);
            http_parser_init(&p, &req);
            continue;
        }
        size_t end = fed < split ? split : size;
        size_t n = end - fed;
        if (n > BUF_SIZE - len)
            n = BUF_SIZE - len;
        if (n == 0)
            return mix(h, "incomplete", 10);
        memcpy(buf + len, data + fed, n);
        len += n;
        fed += n;
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 2)
        return 0;
    size_t split = data[0] % size;
    data++;
    size--;
    if (split > size)
        split = size;
    if (run(data, size, size) != run(data, size, split))
        abort();
    return 0;
}
