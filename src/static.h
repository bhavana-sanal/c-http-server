#ifndef STATIC_H
#define STATIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "http.h"

struct static_config {
    const char *root; /* canonical path without trailing slash ("" for "/") */
    bool autoindex;
};

enum range_result {
    RANGE_IGNORE,        /* absent, malformed or unsupported: send the whole thing */
    RANGE_OK,
    RANGE_UNSATISFIABLE, /* 416 */
};

/*
 * Percent-decode a request path and resolve "." and ".." segments. Returns
 * the length of the result in `out` (NUL-terminated, starting with '/',
 * keeping a trailing '/'), or -1 if the path is malformed, contains NUL, or
 * climbs above the root.
 */
int path_normalize(const char *in, size_t len, char *out, size_t size);

/* Parse a Range field value against a representation of `size` bytes. */
enum range_result range_parse(struct http_str value, uint64_t size, uint64_t *first,
                              uint64_t *last);

const char *mime_type(const char *path);

void static_serve(struct http_ctx *ctx, const struct static_config *cfg);

#endif
