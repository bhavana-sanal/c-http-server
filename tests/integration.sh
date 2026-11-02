#!/bin/sh
# End-to-end tests: run the server on an ephemeral port and talk to it with
# curl and the raw socket client from tests/rawclient.c.
#
#   tests/integration.sh SERVER_BINARY RAWCLIENT_BINARY
set -u

BIN=$1
RAW=$2
WORK=$(mktemp -d)
ROOT=$WORK/root
PASS=0
FAIL=0
PID=

cleanup() {
    [ -n "$PID" ] && kill "$PID" 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT

check() {
    name=$1
    shift
    if "$@"; then
        PASS=$((PASS + 1))
        echo "ok   $name"
    else
        FAIL=$((FAIL + 1))
        echo "FAIL $name"
    fi
}

sha() {
    if command -v sha256sum >/dev/null; then sha256sum <"$1"; else shasum -a 256 <"$1"; fi |
        cut -d' ' -f1
}

start_server() {
    "$BIN" -p 0 -r "$ROOT" "$@" >"$WORK/access.log" 2>"$WORK/server.err" &
    PID=$!
    i=0
    while [ $i -lt 100 ]; do
        PORT=$(sed -n 's/^listening on http:\/\/.*:\([0-9][0-9]*\) .*/\1/p' "$WORK/server.err")
        if [ -n "$PORT" ]; then
            URL=http://127.0.0.1:$PORT
            return 0
        fi
        sleep 0.1
        i=$((i + 1))
    done
    echo "server did not start:"
    cat "$WORK/server.err"
    exit 1
}

# SIGTERM the server and succeed if it exits cleanly (under ASan on Linux,
# that also means LeakSanitizer found nothing).
stop_server() {
    kill -TERM "$PID"
    wait "$PID"
    status=$?
    PID=
    [ "$status" = 0 ]
}

code() { curl -s -o /dev/null -w '%{http_code}' "$@"; }
raw() { "$RAW" "$PORT"; }
header() { tr -d '\r' | awk -v h="$1" -F': ' '$1 == h { print $2 }'; }

mkdir -p "$ROOT/sub" "$ROOT/empty"
printf '<h1>home</h1>\n' >"$ROOT/index.html"
printf 'hello\n' >"$ROOT/hello.txt"
printf 'x' >"$ROOT/sub/<b>&.txt"
head -c 16777216 /dev/urandom >"$ROOT/big.bin"
ln -s hello.txt "$ROOT/link.txt"
ln -s /etc "$ROOT/escape"

# ---------------------------------------------------------------- basics

t_index() { curl -sf "$URL/" | cmp -s - "$ROOT/index.html"; }
t_404() { [ "$(code "$URL/nope")" = 404 ]; }
t_healthz() { [ "$(curl -s "$URL/healthz")" = ok ]; }
t_head() {
    out=$(printf 'HEAD /hello.txt HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n' | raw | tr -d '\r')
    echo "$out" | grep -q '^Content-Length: 6$' && ! echo "$out" | grep -q '^hello'
}
t_echo() {
    out=$(curl -s -d 'hi "there"' "$URL/echo?q=1")
    echo "$out" | grep -qF '"method":"POST","target":"/echo?q=1","path":"/echo","query":"q=1"' &&
        echo "$out" | grep -qF '"body":"hi \"there\""'
}
t_absolute_form() { [ "$(curl -s --request-target "http://example.test/hello.txt" "$URL/")" = hello ]; }
t_mime() { [ "$(curl -sI "$URL/hello.txt" | header Content-Type)" = "text/plain; charset=utf-8" ]; }

# ---------------------------------------------------------------- connections

t_keepalive() {
    [ "$(curl -s -o /dev/null -o /dev/null -w '%{num_connects} ' "$URL/hello.txt" "$URL/hello.txt")" = "1 0 " ]
}
t_pipeline() {
    out=$(printf 'GET /healthz HTTP/1.1\r\nHost: t\r\n\r\nGET /hello.txt HTTP/1.1\r\nHost: t\r\n\r\nGET /echo?n=3 HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n' |
        raw | tr -d '\r')
    [ "$(echo "$out" | grep -c '^HTTP/1.1 200 OK$')" = 3 ] &&
        [ "$(echo "$out" | grep -E '^(ok|hello|\{)' | cut -c1-12 | tr '\n' ' ')" = 'ok hello {"method":"G ' ]
}
t_split() {
    out=$( (printf 'GET /hello.txt HT'; sleep 0.2; printf 'TP/1.1\r\nHo'; sleep 0.2
        printf 'st: t\r\nConnection: close\r\n\r\n') | raw | tr -d '\r')
    echo "$out" | grep -q '^HTTP/1.1 200 OK$' && echo "$out" | grep -q '^hello$'
}
t_http10_closes() {
    out=$(printf 'GET /hello.txt HTTP/1.0\r\n\r\n' | "$RAW" -k "$PORT" | tr -d '\r')
    echo "$out" | grep -q '^Connection: close$' && echo "$out" | grep -q '^hello$'
}
t_idle_timeout() {
    start=$(date +%s)
    "$RAW" -k "$PORT" </dev/null >/dev/null || return 1
    elapsed=$(($(date +%s) - start))
    [ "$elapsed" -ge 1 ] && [ "$elapsed" -le 5 ]
}

# ---------------------------------------------------------------- caching and ranges

t_etag() {
    etag=$(curl -sI "$URL/hello.txt" | header ETag)
    [ -n "$etag" ] && [ "$(code -H "If-None-Match: $etag" "$URL/hello.txt")" = 304 ] &&
        [ "$(code -H "If-None-Match: \"other\", W/$etag" "$URL/hello.txt")" = 304 ] &&
        [ "$(code -H 'If-None-Match: "other"' "$URL/hello.txt")" = 200 ]
}
t_if_modified_since() {
    modified=$(curl -sI "$URL/hello.txt" | header Last-Modified)
    [ -n "$modified" ] && [ "$(code -H "If-Modified-Since: $modified" "$URL/hello.txt")" = 304 ] &&
        [ "$(code -H 'If-Modified-Since: Sun, 06 Nov 1994 08:49:37 GMT' "$URL/hello.txt")" = 200 ]
}
t_range() {
    [ "$(curl -s -r 1-3 "$URL/hello.txt")" = ell ] &&
        [ "$(code -r 1-3 "$URL/hello.txt")" = 206 ] &&
        [ "$(curl -s -r 1-3 -D - -o /dev/null "$URL/hello.txt" | header Content-Range)" = "bytes 1-3/6" ]
}
t_suffix_range() { [ "$(curl -s -H 'Range: bytes=-3' "$URL/hello.txt")" = lo ]; }
t_416() {
    [ "$(code -r 100-200 "$URL/hello.txt")" = 416 ] &&
        [ "$(curl -s -r 100-200 -D - -o /dev/null "$URL/hello.txt" | header Content-Range)" = "bytes */6" ]
}
t_big_file() {
    curl -s -o "$WORK/big.dl" "$URL/big.bin" && [ "$(sha "$WORK/big.dl")" = "$(sha "$ROOT/big.bin")" ]
}
t_big_range() {
    curl -s -r 1000000-2999999 -o "$WORK/part.dl" "$URL/big.bin" &&
        tail -c +1000001 "$ROOT/big.bin" | head -c 2000000 >"$WORK/part.want" &&
        cmp -s "$WORK/part.dl" "$WORK/part.want"
}

# ---------------------------------------------------------------- paths

t_traversal() { [ "$(code --path-as-is "$URL/../../../../etc/passwd")" = 400 ]; }
t_traversal_encoded() { [ "$(code --path-as-is "$URL/%2e%2e/%2e%2e/%2e%2e/etc/passwd")" = 400 ]; }
t_symlink_escape() { [ "$(code "$URL/escape/hosts")" = 403 ]; }
t_symlink_inside() { [ "$(curl -s "$URL/link.txt")" = hello ]; }
t_dir_redirect() {
    [ "$(curl -s -o /dev/null -w '%{http_code} %{redirect_url}' "$URL/sub?x=1")" = "301 $URL/sub/?x=1" ]
}
t_autoindex() {
    out=$(curl -s "$URL/sub/")
    echo "$out" | grep -qF '>&lt;b&gt;&amp;.txt</a>' && echo "$out" | grep -qF 'href="%3Cb%3E%26.txt"'
}

# ---------------------------------------------------------------- request bodies and errors

t_chunked_body() {
    printf 'abc%.0s' $(seq 400) >"$WORK/chunk.txt"
    curl -s -H 'Transfer-Encoding: chunked' --data-binary @"$WORK/chunk.txt" "$URL/echo" |
        grep -qF "\"body\":\"$(cat "$WORK/chunk.txt")\""
}
t_chunked_trailers() {
    printf 'POST /echo HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n5;ext=1\r\nhello\r\n0\r\nX-Trailer: y\r\n\r\n' |
        raw | grep -qF '"body":"hello"'
}
t_100_continue() {
    head -c 4000 /dev/zero | tr '\0' a >"$WORK/a.txt"
    t=$(curl -s -o "$WORK/echo.json" -w '%{time_total}' --data-binary @"$WORK/a.txt" "$URL/echo") &&
        grep -qF "\"body\":\"$(cat "$WORK/a.txt")\"" "$WORK/echo.json" &&
        awk -v t="$t" 'BEGIN { exit !(t < 0.9) }'
}
t_413() { [ "$(head -c 20000 /dev/zero | code --data-binary @- "$URL/echo")" = 413 ]; }
t_414() { [ "$(code "$URL/$(head -c 5000 /dev/zero | tr '\0' a)")" = 414 ]; }
t_431() { [ "$(code -H "X-Big: $(head -c 10000 /dev/zero | tr '\0' a)" "$URL/")" = 431 ]; }
t_405() {
    [ "$(code -X DELETE "$URL/hello.txt")" = 405 ] &&
        [ "$(curl -s -X DELETE -D - -o /dev/null "$URL/hello.txt" | header Allow)" = "GET, HEAD" ]
}
t_501() { [ "$(code -X BREW "$URL/")" = 501 ]; }
t_505() { printf 'GET / HTTP/2.0\r\nHost: t\r\n\r\n' | raw | grep -q '^HTTP/1.1 505 '; }
t_space_before_colon() { printf 'GET / HTTP/1.1\r\nHost : t\r\n\r\n' | raw | grep -q '^HTTP/1.1 400 '; }
t_bare_lf() { printf 'GET / HTTP/1.1\nHost: t\n\n' | raw | grep -q '^HTTP/1.1 400 '; }
t_missing_host() { printf 'GET / HTTP/1.1\r\n\r\n' | raw | grep -q '^HTTP/1.1 400 '; }
t_smuggling() {
    printf 'POST /echo HTTP/1.1\r\nHost: t\r\nContent-Length: 4\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n' |
        raw | grep -q '^HTTP/1.1 400 '
}
t_access_log() {
    grep -qE '^127\.0\.0\.1 - - \[[0-9]{2}/[A-Z][a-z]{2}/[0-9]{4}(:[0-9]{2}){3} [+-][0-9]{4}\] "GET /healthz HTTP/1\.1" 200 3$' \
        "$WORK/access.log"
}

# ---------------------------------------------------------------- shutdown

t_graceful_shutdown() {
    curl -s --limit-rate 4M -o "$WORK/drain.dl" "$URL/big.bin" &
    curl_pid=$!
    sleep 1
    kill -TERM "$PID"
    sleep 0.3
    refused=$(code "$URL/hello.txt")
    wait "$curl_pid"
    curl_status=$?
    wait "$PID"
    server_status=$?
    PID=
    [ "$curl_status" = 0 ] && [ "$server_status" = 0 ] && [ "$refused" = 000 ] &&
        grep -q 'draining 1 connection$' "$WORK/server.err" &&
        [ "$(sha "$WORK/drain.dl")" = "$(sha "$ROOT/big.bin")" ]
}

t_workers() {
    for _ in $(seq 30); do
        [ "$(code "$URL/hello.txt")" = 200 ] || return 1
    done
}
t_no_index_no_listing() { [ "$(code "$URL/empty/")" = 403 ]; }

start_server -t 2 -a
for t in index 404 healthz head echo absolute_form mime \
    keepalive pipeline split http10_closes idle_timeout \
    etag if_modified_since range suffix_range 416 big_file big_range \
    traversal traversal_encoded symlink_escape symlink_inside dir_redirect autoindex \
    chunked_body chunked_trailers 100_continue 413 414 431 405 501 505 \
    space_before_colon bare_lf missing_host smuggling access_log graceful_shutdown; do
    check "$t" "t_$t"
done

start_server -w 3
check workers t_workers
check no_index_no_listing t_no_index_no_listing
check workers_stop_cleanly stop_server

echo "integration: $PASS passed, $FAIL failed"
[ "$FAIL" = 0 ]
