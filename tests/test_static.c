#include <string.h>

#include "check.h"
#include "http.h"
#include "static.h"

static void test_path_normalize(void)
{
    static const struct {
        const char *in, *want; /* want == NULL: must be rejected */
    } cases[] = {
        {"/", "/"},
        {"/a/b", "/a/b"},
        {"/a/b/", "/a/b/"},
        {"//a///b", "/a/b"},
        {"/a/./b/../c", "/a/c"},
        {"/a/..", "/"},
        {"/a/.", "/a/"},
        {"/a/b/..", "/a/"},
        {"/...", "/..."},
        {"/..a/b..", "/..a/b.."},
        {"/a%20b", "/a b"},
        {"/a%2fb", "/a/b"},
        {"/%61%62%63", "/abc"},
        {"/%2e/x", "/x"},
        {"/..", NULL},
        {"/../", NULL},
        {"/a/../..", NULL},
        {"/a/../../etc/passwd", NULL},
        {"/%2e%2e/etc/passwd", NULL},
        {"/%2E%2E%2Fetc", NULL},
        {"/a/%2e%2e/%2e%2e/x", NULL},
        {"/.%2e/x", NULL},
        {"/%00", NULL},
        {"/a%0", NULL},
        {"/a%", NULL},
        {"/%zz", NULL},
        {"", NULL},
        {"a/b", NULL},
        {"%2fa", NULL},
    };
    char out[256];

    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        int n = path_normalize(cases[i].in, strlen(cases[i].in), out, sizeof out);
        if (cases[i].want)
            CHECK(n >= 0 && (size_t)n == strlen(cases[i].want) && strcmp(out, cases[i].want) == 0,
                  cases[i].in);
        else
            CHECK(n < 0, cases[i].in);
    }
    CHECK(path_normalize("/abcdef", 7, out, 7) < 0, "output buffer too small");
}

static void test_range_parse(void)
{
    static const struct {
        const char *value;
        uint64_t size;
        enum range_result want;
        uint64_t first, last;
    } cases[] = {
        {"bytes=0-9", 100, RANGE_OK, 0, 9},
        {"bytes=10-", 100, RANGE_OK, 10, 99},
        {"bytes=-10", 100, RANGE_OK, 90, 99},
        {"bytes=-1000", 100, RANGE_OK, 0, 99},
        {"bytes=50-1000", 100, RANGE_OK, 50, 99},
        {"bytes=99-99", 100, RANGE_OK, 99, 99},
        {"Bytes= 2-4 ", 100, RANGE_OK, 2, 4},
        {"bytes=100-", 100, RANGE_UNSATISFIABLE, 0, 0},
        {"bytes=100-200", 100, RANGE_UNSATISFIABLE, 0, 0},
        {"bytes=-0", 100, RANGE_UNSATISFIABLE, 0, 0},
        {"bytes=0-", 0, RANGE_UNSATISFIABLE, 0, 0},
        {"bytes=-5", 0, RANGE_IGNORE, 0, 0},
        {"bytes=5-2", 100, RANGE_IGNORE, 0, 0},
        {"bytes=0-1,5-6", 100, RANGE_IGNORE, 0, 0},
        {"bytes=", 100, RANGE_IGNORE, 0, 0},
        {"bytes=-", 100, RANGE_IGNORE, 0, 0},
        {"bytes=a-b", 100, RANGE_IGNORE, 0, 0},
        {"bytes=1-2x", 100, RANGE_IGNORE, 0, 0},
        {"bytes=99999999999999999999-", 100, RANGE_IGNORE, 0, 0},
        {"items=0-9", 100, RANGE_IGNORE, 0, 0},
        {"bytes", 100, RANGE_IGNORE, 0, 0},
    };

    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        uint64_t first = 0, last = 0;
        struct http_str v = {cases[i].value, strlen(cases[i].value)};
        enum range_result r = range_parse(v, cases[i].size, &first, &last);
        CHECK(r == cases[i].want, cases[i].value);
        if (r == RANGE_OK && cases[i].want == RANGE_OK)
            CHECK(first == cases[i].first && last == cases[i].last, cases[i].value);
    }
}

static void test_mime_type(void)
{
    CHECK(strcmp(mime_type("/a/index.html"), "text/html; charset=utf-8") == 0, "html");
    CHECK(strcmp(mime_type("/A.PNG"), "image/png") == 0, "extension case");
    CHECK(strcmp(mime_type("/archive.tar.gz"), "application/gzip") == 0, "last extension wins");
    CHECK(strcmp(mime_type("/v1.2/README"), "application/octet-stream") == 0, "dot in dir");
    CHECK(strcmp(mime_type("/noext"), "application/octet-stream") == 0, "no extension");
}

static void test_http_date(void)
{
    /* The three equivalent forms from RFC 9110 §5.6.7. */
    static const char *const forms[] = {
        "Sun, 06 Nov 1994 08:49:37 GMT",
        "Sunday, 06-Nov-94 08:49:37 GMT",
        "Sun Nov  6 08:49:37 1994",
    };
    char out[HTTP_DATE_SIZE];
    time_t t;

    for (size_t i = 0; i < sizeof forms / sizeof *forms; i++) {
        struct http_str s = {forms[i], strlen(forms[i])};
        CHECK(http_parse_date(s, &t) && t == 784111777, forms[i]);
    }
    http_format_date(784111777, out);
    CHECK(strcmp(out, forms[0]) == 0, "format IMF-fixdate");

    static const char *const bad[] = {"", "yesterday", "Sun, 06 Nov 1994 08:49:37 GMT junk"};
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++)
        CHECK(!http_parse_date((struct http_str){bad[i], strlen(bad[i])}, &t), bad[i]);
}

int main(void)
{
    test_path_normalize();
    test_range_parse();
    test_mime_type();
    test_http_date();
    return check_summary("test_static");
}
