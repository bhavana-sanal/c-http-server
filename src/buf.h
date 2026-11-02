#ifndef BUF_H
#define BUF_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * Growable byte buffer. Allocation failure is sticky: once `failed` is set
 * every append is a no-op, so callers build a whole message and check once.
 */
struct buf {
    char *data;
    size_t len, cap;
    bool failed;
};

void buf_append(struct buf *b, const void *data, size_t len);
void buf_puts(struct buf *b, const char *s);
void buf_vprintf(struct buf *b, const char *fmt, va_list ap)
    __attribute__((format(printf, 2, 0)));
void buf_printf(struct buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void buf_free(struct buf *b);

#endif
