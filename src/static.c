#include "static.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

int path_normalize(const char *in, size_t len, char *out, size_t size)
{
    size_t n = 0;

    if (len == 0 || in[0] != '/' || len >= size)
        return -1;

    /* Decode first, so that encoded dots and slashes (%2e%2e%2f) go through
     * the same segment handling as literal ones. */
    for (size_t i = 0; i < len; i++) {
        char c = in[i];
        if (c == '%') {
            int hi, lo;
            if (len - i < 3 || (hi = http_hex_value(in[i + 1])) < 0 ||
                (lo = http_hex_value(in[i + 2])) < 0)
                return -1;
            c = (char)(hi << 4 | lo);
            if (c == '\0')
                return -1;
            i += 2;
        }
        out[n++] = c;
    }

    /* Resolve segments in place; the write cursor never passes the read cursor. */
    size_t w = 0;
    bool trailing_slash = false;
    for (size_t r = 0; r < n;) {
        size_t start = r + 1, end = start;
        while (end < n && out[end] != '/')
            end++;
        size_t seg = end - start;

        trailing_slash = true;
        if (seg == 0 || (seg == 1 && out[start] == '.')) {
            /* empty or "." */
        } else if (seg == 2 && out[start] == '.' && out[start + 1] == '.') {
            if (w == 0)
                return -1;
            while (out[w - 1] != '/')
                w--;
            w--;
        } else {
            out[w++] = '/';
            memmove(out + w, out + start, seg);
            w += seg;
            trailing_slash = false;
        }
        r = end;
    }
    if (w == 0 || trailing_slash)
        out[w++] = '/';
    out[w] = '\0';
    return (int)w;
}

static bool parse_u64(const char *p, const char *end, uint64_t *out)
{
    uint64_t v = 0;

    if (p == end)
        return false;
    for (; p < end; p++) {
        if (*p < '0' || *p > '9')
            return false;
        unsigned d = (unsigned)(*p - '0');
        if (v > (UINT64_MAX - d) / 10)
            return false;
        v = v * 10 + d;
    }
    *out = v;
    return true;
}

/* RFC 9110 §14.1-14.2. A single range only: multiple ranges would need a
 * multipart/byteranges body, and ignoring Range is always allowed. */
enum range_result range_parse(struct http_str value, uint64_t size, uint64_t *first,
                              uint64_t *last)
{
    if (value.len < 6 || !http_str_ieq((struct http_str){value.p, 6}, "bytes="))
        return RANGE_IGNORE;
    struct http_str spec = http_str_trim((struct http_str){value.p + 6, value.len - 6});
    const char *end = spec.p + spec.len;
    const char *dash = memchr(spec.p, '-', spec.len);
    uint64_t a = 0, b = 0;

    if (!dash || memchr(spec.p, ',', spec.len))
        return RANGE_IGNORE;

    if (dash == spec.p) { /* suffix range "-N": the last N bytes */
        if (!parse_u64(dash + 1, end, &b))
            return RANGE_IGNORE;
        if (b == 0)
            return RANGE_UNSATISFIABLE;
        if (size == 0)
            return RANGE_IGNORE; /* nothing to slice; a 200 with the empty body is right */
        *first = b < size ? size - b : 0;
        *last = size - 1;
        return RANGE_OK;
    }

    if (!parse_u64(spec.p, dash, &a))
        return RANGE_IGNORE;
    if (dash + 1 == end)
        b = UINT64_MAX;
    else if (!parse_u64(dash + 1, end, &b) || b < a)
        return RANGE_IGNORE;
    if (a >= size)
        return RANGE_UNSATISFIABLE;
    *first = a;
    *last = b < size ? b : size - 1;
    return RANGE_OK;
}

const char *mime_type(const char *path)
{
    static const struct {
        const char *ext, *type;
    } types[] = {
        {"html", "text/html; charset=utf-8"},
        {"htm", "text/html; charset=utf-8"},
        {"css", "text/css; charset=utf-8"},
        {"js", "text/javascript; charset=utf-8"},
        {"mjs", "text/javascript; charset=utf-8"},
        {"json", "application/json"},
        {"txt", "text/plain; charset=utf-8"},
        {"md", "text/markdown; charset=utf-8"},
        {"csv", "text/csv; charset=utf-8"},
        {"xml", "application/xml"},
        {"svg", "image/svg+xml"},
        {"png", "image/png"},
        {"jpg", "image/jpeg"},
        {"jpeg", "image/jpeg"},
        {"gif", "image/gif"},
        {"webp", "image/webp"},
        {"avif", "image/avif"},
        {"ico", "image/vnd.microsoft.icon"},
        {"pdf", "application/pdf"},
        {"wasm", "application/wasm"},
        {"woff", "font/woff"},
        {"woff2", "font/woff2"},
        {"ttf", "font/ttf"},
        {"mp3", "audio/mpeg"},
        {"wav", "audio/wav"},
        {"mp4", "video/mp4"},
        {"webm", "video/webm"},
        {"zip", "application/zip"},
        {"gz", "application/gzip"},
        {"tar", "application/x-tar"},
    };
    const char *slash = strrchr(path, '/');
    const char *dot = strrchr(slash ? slash : path, '.');

    if (dot)
        for (size_t i = 0; i < sizeof types / sizeof *types; i++)
            if (strcasecmp(dot + 1, types[i].ext) == 0)
                return types[i].type;
    return "application/octet-stream";
}

/*
 * Map a normalized request path to a canonical file path under the root.
 * realpath() resolves every symlink, so checking the result's prefix catches
 * links that point outside the root. Returns 0 or an HTTP status.
 */
static int resolve(const char *root, const char *rel, char real[PATH_MAX], struct stat *st)
{
    char full[PATH_MAX];
    size_t root_len = strlen(root);

    if (snprintf(full, sizeof full, "%s%s", root, rel) >= (int)sizeof full)
        return 404;
    if (!realpath(full, real) || stat(real, st) < 0)
        return errno == EACCES ? 403 : 404;
    if (strncmp(real, root, root_len) != 0 || (real[root_len] != '/' && real[root_len] != '\0'))
        return 403;
    return 0;
}

/* Weak comparison against an If-None-Match list (RFC 9110 §13.1.2). */
static bool etag_matches(struct http_str list, const char *etag)
{
    const char *p = list.p, *end = list.p + list.len;
    size_t etag_len = strlen(etag);

    while (p < end) {
        if (*p == ' ' || *p == '\t' || *p == ',') {
            p++;
            continue;
        }
        if (*p == '*')
            return true;
        if (end - p >= 2 && p[0] == 'W' && p[1] == '/')
            p += 2;
        if (p == end || *p != '"')
            return false;
        const char *quote = memchr(p + 1, '"', (size_t)(end - p - 1));
        if (!quote)
            return false;
        if ((size_t)(quote + 1 - p) == etag_len && memcmp(p, etag, etag_len) == 0)
            return true;
        p = quote + 1;
    }
    return false;
}

/* RFC 9110 §13.2.2: If-None-Match takes precedence over If-Modified-Since. */
static bool not_modified(const struct http_ctx *ctx, const char *etag, time_t mtime)
{
    const struct http_str *inm = http_header_get(ctx->req, "If-None-Match");
    const struct http_str *ims = http_header_get(ctx->req, "If-Modified-Since");
    time_t since;

    if (inm)
        return etag_matches(*inm, etag);
    return ims && http_parse_date(*ims, &since) && mtime <= since;
}

static void serve_file(struct http_ctx *ctx, const char *path)
{
    /* O_NONBLOCK keeps a FIFO from stalling the event loop in open(); it is
     * rejected right after by the S_ISREG check. */
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    struct stat st;

    if (fd < 0) {
        http_error(ctx, errno == EACCES ? 403 : 404);
        return;
    }
    if (fstat(fd, &st) < 0) {
        http_error(ctx, 500);
        close(fd);
        return;
    }
    if (!S_ISREG(st.st_mode)) {
        http_error(ctx, 403);
        close(fd);
        return;
    }

    char etag[64], modified[HTTP_DATE_SIZE];
    uint64_t size = (uint64_t)st.st_size, first = 0, last = size ? size - 1 : 0;
    snprintf(etag, sizeof etag, "\"%" PRIx64 "-%" PRIx64 "-%" PRIx64 "\"", (uint64_t)st.st_ino,
             size, (uint64_t)st.st_mtime);
    http_format_date(st.st_mtime, modified);

    if (not_modified(ctx, etag, st.st_mtime)) {
        close(fd);
        http_begin(ctx, 304);
        http_header(ctx, "ETag", "%s", etag);
        http_header(ctx, "Last-Modified", "%s", modified);
        http_end(ctx, 0);
        return;
    }

    /* Range applies to GET only (RFC 9110 §14.2). We don't evaluate If-Range,
     * so when it is present the safe answer is the full representation. */
    int status = 200;
    const struct http_str *range = http_header_get(ctx->req, "Range");
    if (range && !ctx->head && !http_header_get(ctx->req, "If-Range")) {
        switch (range_parse(*range, size, &first, &last)) {
        case RANGE_OK:
            status = 206;
            break;
        case RANGE_UNSATISFIABLE:
            close(fd);
            http_begin(ctx, 416);
            http_header(ctx, "Content-Range", "bytes */%" PRIu64, size);
            http_error_body(ctx);
            return;
        case RANGE_IGNORE:
            break;
        }
    }

    uint64_t len = status == 206 ? last - first + 1 : size;
    http_begin(ctx, status);
    http_header(ctx, "Content-Type", "%s", mime_type(path));
    http_header(ctx, "ETag", "%s", etag);
    http_header(ctx, "Last-Modified", "%s", modified);
    http_header(ctx, "Accept-Ranges", "bytes");
    if (status == 206)
        http_header(ctx, "Content-Range", "bytes %" PRIu64 "-%" PRIu64 "/%" PRIu64, first, last,
                    size);
    http_end(ctx, len);

    if (ctx->head || len == 0) {
        close(fd);
        return;
    }
    ctx->file_fd = fd;
    ctx->file_off = (off_t)first;
    ctx->file_len = len;
}

static void html_escape(struct buf *b, const char *s)
{
    for (; *s; s++) {
        switch (*s) {
        case '&': buf_puts(b, "&amp;"); break;
        case '<': buf_puts(b, "&lt;"); break;
        case '>': buf_puts(b, "&gt;"); break;
        case '"': buf_puts(b, "&quot;"); break;
        case '\'': buf_puts(b, "&#39;"); break;
        default: buf_append(b, s, 1);
        }
    }
}

/* Percent-encode everything but unreserved characters and '/', so a file
 * named "a:b" or "#x" still yields a relative link to itself. */
static void url_escape(struct buf *b, const char *s)
{
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            strchr("-._~/", c))
            buf_append(b, s, 1);
        else
            buf_printf(b, "%%%02X", c);
    }
}

static int compare_names(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void autoindex(struct http_ctx *ctx, const char *dir, const char *rel)
{
    DIR *d = opendir(dir);
    if (!d) {
        http_error(ctx, errno == EACCES ? 403 : 404);
        return;
    }

    char **names = NULL;
    size_t n = 0, cap = 0;
    bool ok = true;
    for (;;) {
        errno = 0;
        struct dirent *e = readdir(d);
        if (!e) {
            ok = errno == 0;
            break;
        }
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            char **grown = realloc(names, cap * sizeof *names);
            if (!grown) {
                ok = false;
                break;
            }
            names = grown;
        }
        struct stat st;
        size_t len = strlen(e->d_name);
        char *name = malloc(len + 2);
        if (!name) {
            ok = false;
            break;
        }
        memcpy(name, e->d_name, len);
        if (fstatat(dirfd(d), e->d_name, &st, 0) == 0 && S_ISDIR(st.st_mode))
            name[len++] = '/';
        name[len] = '\0';
        names[n++] = name;
    }
    if (closedir(d) < 0)
        ok = false;

    struct buf page = {0};
    if (ok) {
        if (n > 0)
            qsort(names, n, sizeof *names, compare_names);
        buf_puts(&page, "<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\"><title>Index of ");
        html_escape(&page, rel);
        buf_puts(&page, "</title></head>\n<body><h1>Index of ");
        html_escape(&page, rel);
        buf_puts(&page, "</h1>\n<ul>\n");
        if (strcmp(rel, "/") != 0)
            buf_puts(&page, "<li><a href=\"../\">../</a></li>\n");
        for (size_t i = 0; i < n; i++) {
            buf_puts(&page, "<li><a href=\"");
            url_escape(&page, names[i]);
            buf_puts(&page, "\">");
            html_escape(&page, names[i]);
            buf_puts(&page, "</a></li>\n");
        }
        buf_puts(&page, "</ul>\n</body></html>\n");
    }

    if (ok && !page.failed) {
        http_begin(ctx, 200);
        http_finish(ctx, "text/html; charset=utf-8", page.data, page.len);
    } else {
        http_error(ctx, 500);
    }
    buf_free(&page);
    for (size_t i = 0; i < n; i++)
        free(names[i]);
    free(names);
}

/* Directory URLs must end in '/' or relative links inside them break. */
static void redirect_to_dir(struct http_ctx *ctx)
{
    http_begin(ctx, 301);
    http_header(ctx, "Location", "%.*s/%s%.*s", (int)ctx->path.len, ctx->path.p,
                ctx->query.len ? "?" : "", (int)ctx->query.len, ctx->query.p);
    http_error_body(ctx);
}

void static_serve(struct http_ctx *ctx, const struct static_config *cfg)
{
    char rel[PATH_MAX], real[PATH_MAX];
    struct stat st;

    if (!http_is_get_or_head(ctx)) {
        http_reject_method(ctx, "GET, HEAD");
        return;
    }
    if (path_normalize(ctx->path.p, ctx->path.len, rel, sizeof rel) < 0) {
        http_error(ctx, 400);
        return;
    }

    int status = resolve(cfg->root, rel, real, &st);
    if (status == 0 && S_ISDIR(st.st_mode)) {
        char dir[PATH_MAX], index_path[PATH_MAX];
        if (rel[strlen(rel) - 1] != '/') {
            redirect_to_dir(ctx);
            return;
        }
        memcpy(dir, real, strlen(real) + 1);
        if (snprintf(index_path, sizeof index_path, "%sindex.html", rel) >= (int)sizeof index_path)
            status = 404;
        else
            status = resolve(cfg->root, index_path, real, &st);
        if (status == 404 && cfg->autoindex) {
            autoindex(ctx, dir, rel);
            return;
        }
        if (status == 404)
            status = 403; /* the directory exists; we just won't list it */
    }
    if (status != 0) {
        http_error(ctx, status);
        return;
    }
    serve_file(ctx, real);
}
