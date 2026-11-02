#include "buf.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool reserve(struct buf *b, size_t extra)
{
    if (b->failed)
        return false;
    if (b->cap - b->len >= extra)
        return true;

    size_t cap = b->cap ? b->cap : 256;
    while (cap - b->len < extra) {
        if (cap > SIZE_MAX / 2) {
            b->failed = true;
            return false;
        }
        cap *= 2;
    }
    char *data = realloc(b->data, cap);
    if (!data) {
        b->failed = true;
        return false;
    }
    b->data = data;
    b->cap = cap;
    return true;
}

void buf_append(struct buf *b, const void *data, size_t len)
{
    if (len > 0 && reserve(b, len)) {
        memcpy(b->data + b->len, data, len);
        b->len += len;
    }
}

void buf_puts(struct buf *b, const char *s)
{
    buf_append(b, s, strlen(s));
}

void buf_vprintf(struct buf *b, const char *fmt, va_list ap)
{
    va_list copy;

    va_copy(copy, ap);
    int n = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (n < 0) {
        b->failed = true;
        return;
    }
    if (!reserve(b, (size_t)n + 1))
        return;
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap);
    b->len += (size_t)n;
}

void buf_printf(struct buf *b, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    buf_vprintf(b, fmt, ap);
    va_end(ap);
}

void buf_free(struct buf *b)
{
    free(b->data);
    *b = (struct buf){0};
}
