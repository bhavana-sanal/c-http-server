#ifndef HTTP_PARSER_H
#define HTTP_PARSER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HTTP_MAX_HEAD       8192 /* request line + header section; 431 beyond */
#define HTTP_MAX_TARGET     4096 /* 414 beyond */
#define HTTP_MAX_METHOD       32 /* 501 beyond: no method we know is that long */
#define HTTP_MAX_HEADERS      64 /* 431 beyond */
#define HTTP_MAX_BODY       8192 /* decoded request content; 413 beyond */
#define HTTP_MAX_CHUNK_LINE  256 /* chunk-size line incl. extensions; 400 beyond */

/* A slice of the connection buffer. Never NUL-terminated. */
struct http_str {
    const char *p;
    size_t len;
};

struct http_header {
    struct http_str name, value;
};

struct http_request {
    struct http_str method, target;
    int minor; /* HTTP/1.<minor>; -1 until the request line has been parsed */
    struct http_header headers[HTTP_MAX_HEADERS];
    size_t n_headers;
    struct http_str body;
    bool keep_alive;
    bool expect_continue;
};

enum http_parse_result {
    HTTP_PARSE_INCOMPLETE,
    HTTP_PARSE_DONE,
    HTTP_PARSE_ERROR,
};

/*
 * Incremental request parser. Call http_parse() with the same buffer each
 * time more bytes have been appended to it; parsing resumes where it stopped,
 * so a request may be split at any byte. Parsed fields point into the buffer.
 *
 * A chunked body is decoded in place: the chunk framing is squeezed out of
 * the buffer (hence the non-const buf and *len update), leaving the decoded
 * content directly after the head.
 *
 * After HTTP_PARSE_DONE, `pos` is the number of bytes at the start of buf
 * that belong to this request; anything after it is the next pipelined one.
 * After HTTP_PARSE_ERROR, `status` holds the HTTP status code to answer with.
 */
struct http_parser {
    struct http_request *req;
    int state;
    int status;
    bool chunked;
    bool trailers;
    size_t pos;       /* next byte to examine */
    size_t mark;      /* start of the token being scanned */
    size_t value_end; /* end of the field value, minus trailing whitespace */
    size_t head_len;  /* 0 until the empty line ending the head is seen */
    size_t dst;       /* chunked: end of the decoded body */
    size_t budget;    /* bytes left for the current chunk line or trailers */
    uint64_t remaining;
};

void http_parser_init(struct http_parser *p, struct http_request *req);
enum http_parse_result http_parse(struct http_parser *p, char *buf, size_t *len);

const struct http_str *http_header_get(const struct http_request *req, const char *name);
bool http_str_eq(struct http_str s, const char *lit);
bool http_str_ieq(struct http_str s, const char *lit);
struct http_str http_str_trim(struct http_str s);
int http_hex_value(char c);

#endif
