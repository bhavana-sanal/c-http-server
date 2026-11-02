/*
 * Tiny TCP client for the integration tests: forwards stdin to 127.0.0.1:PORT
 * as it arrives (so pauses in the input become separate TCP segments), then
 * prints everything the server sends until it closes the connection.
 *
 *   rawclient [-k] PORT
 *
 * By default the write side is shut down after stdin ends; -k keeps it open,
 * so the client only finishes when the server decides to close.
 * Exits 0 on a clean close, 3 if the server sends nothing for 10 seconds,
 * 1 on any error (including a connection reset).
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static int send_all(int fd, const char *p, size_t n)
{
    while (n > 0) {
        ssize_t w = send(fd, p, n, 0);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

int main(int argc, char **argv)
{
    int keep_open = argc == 3 && strcmp(argv[1], "-k") == 0;
    struct sockaddr_in addr = {.sin_family = AF_INET};
    struct timeval timeout = {.tv_sec = 10};
    char buf[65536];

    if (argc != 2 + keep_open) {
        fputs("usage: rawclient [-k] PORT\n", stderr);
        return 2;
    }
    addr.sin_port = htons((unsigned short)atoi(argv[argc - 1]));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0 ||
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout) < 0) {
        perror("rawclient");
        return 1;
    }

    for (;;) {
        ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            perror("rawclient: stdin");
            return 1;
        }
        if (n == 0)
            break;
        if (send_all(fd, buf, (size_t)n) < 0) {
            perror("rawclient: send");
            return 1;
        }
    }
    if (!keep_open && shutdown(fd, SHUT_WR) < 0) {
        perror("rawclient: shutdown");
        return 1;
    }

    for (;;) {
        ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n > 0) {
            if (fwrite(buf, 1, (size_t)n, stdout) != (size_t)n)
                return 1;
            continue;
        }
        if (n == 0)
            break;
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 3;
        perror("rawclient: recv");
        return 1;
    }
    close(fd);
    return 0;
}
