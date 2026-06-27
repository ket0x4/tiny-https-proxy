#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define BUF_SIZE 8192

static int read_line(int fd, char *buf, int max) {
    int i = 0;

    while (i < max - 1) {
        int n = read(fd, buf + i, 1);
        if (n <= 0)
            break;
        if (buf[i] == '\n') {
            i++;
            break;
        }
        i++;
    }
    buf[i] = '\0';
    return i;
}

static int copy_data(int from_fd, int to_fd) {
    char buf[BUF_SIZE];
    int n = read(from_fd, buf, BUF_SIZE);

    if (n <= 0)
        return -1;

    int pos = 0;
    while (pos < n) {
        int written = write(to_fd, buf + pos, n - pos);
        if (written <= 0)
            return -1;
        pos += written;
    }
    return 0;
}

static int connect_to(const char *host, const char *port) {
    struct addrinfo hints;
    struct addrinfo *list;
    struct addrinfo *rp;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int err = getaddrinfo(host, port, &hints, &list);
    if (err != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(err));
        return -1;
    }

    int fd = -1;
    rp = list;
    while (rp != NULL) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd >= 0) {
            if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0)
                break;
            close(fd);
            fd = -1;
        }
        rp = rp->ai_next;
    }
    freeaddrinfo(list);
    return fd;
}

static void tunnel(int remote_fd) {
    while (1) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(0, &read_fds);
        FD_SET(remote_fd, &read_fds);

        int nfds = (remote_fd > 0 ? remote_fd : 0) + 1;

        if (select(nfds, &read_fds, NULL, NULL, NULL) < 0)
            break;

        if (FD_ISSET(0, &read_fds)) {
            if (copy_data(0, remote_fd) < 0)
                break;
        }
        if (FD_ISSET(remote_fd, &read_fds)) {
            if (copy_data(remote_fd, 1) < 0)
                break;
        }
    }
}

static char * parse_host_port(char *url, char *host, int host_size,
                char *port, int port_size) {
    char *p = url;
    int has_scheme = 0;

    /* skip http:// or https:// if present */
    if (strncmp(p, "http://", 7) == 0) {
        p += 7;
        has_scheme = 1;
    } else if (strncmp(p, "https://", 8) == 0) {
        p += 8;
        has_scheme = 1;
    }

    if (!has_scheme) {
        /* no scheme: treat entire url as host:port */
        size_t slen = strlen(url);
        if (slen >= (size_t)host_size) slen = host_size - 1;
        memcpy(host, url, slen);
        host[slen] = '\0';
        return "/";
    }

    /* find where the path starts */
    char *slash = strchr(p, '/');
    char *path;

    if (slash != NULL) {
        int auth_len = slash - p;
        if (auth_len >= host_size)
            auth_len = host_size - 1;
        memcpy(host, p, auth_len);
        host[auth_len] = '\0';
        path = slash;
    } else {
        size_t slen = strlen(p);
        if (slen >= (size_t)host_size) slen = host_size - 1;
        memcpy(host, p, slen);
        host[slen] = '\0';
        path = "/";
    }

    /* split off port number if present */
    char *colon = strchr(host, ':');
    if (colon != NULL) {
        *colon = '\0';
        strncpy(port, colon + 1, port_size - 1);
        port[port_size - 1] = '\0';
    } else {
        strncpy(port, "80", port_size - 1);
    }

    return path;
}

int main(void) {
    char line[BUF_SIZE];
    char method[64];
    char url[BUF_SIZE];
    char host[256];
    char port[16];
    char headers[BUF_SIZE];
    int  header_len = 0;

    /* read the request line: "METHOD URL ..." */
    int n = read_line(0, line, BUF_SIZE);
    if (n <= 0)
        return 1;

    if (sscanf(line, "%63s %1023s", method, url) < 2)
        return 1;

    /* read and store the header lines */
    headers[0] = '\0';
    while (1) {
        n = read_line(0, line, BUF_SIZE);
        if (n <= 0)
            break;
        if (line[0] == '\r' || line[0] == '\0')
            break;
        if (header_len + n < (int)sizeof(headers)) {
            memcpy(headers + header_len, line, n);
            header_len += n;
        }
    }

    /* ---------- CONNECT method (HTTPS tunnel) ---------- */
    if (strcmp(method, "CONNECT") == 0) {
        /* URL is "host:port" */
        if (sscanf(url, "%255[^:]:%15s", host, port) < 2)
            return 1;

        fprintf(stderr, "Connect: %s:%s\n", host, port);
        int remote = connect_to(host, port);
        if (remote < 0) {
            write(1, "HTTP/1.0 502 Bad Gateway\r\n\r\n", 28);
            return 1;
        }

        write(1, "HTTP/1.0 200 Connection established\r\n\r\n", 39);
        tunnel(remote);
        close(remote);
        fprintf(stderr, "Disconnect: %s:%s\n", host, port);
        return 0;
    }

    /* ---------- Regular HTTP proxy ---------- */
    char *path = parse_host_port(url, host, sizeof(host),
                                 port, sizeof(port));

    int remote = connect_to(host, port);
    if (remote < 0) {
        write(1, "HTTP/1.0 502 Bad Gateway\r\n\r\n", 28);
        return 1;
    }

    /* rewrite request line with origin-form path */ {
        char request[BUF_SIZE];
        int len = snprintf(request, sizeof(request),
                           "%s %s HTTP/1.0\r\n", method, path);
        write(remote, request, len);
    }

    /* forward stored headers */
    if (header_len > 0)
        write(remote, headers, header_len);

    /* empty line marks end of headers */
    write(remote, "\r\n", 2);

    /* relay request body and response */
    tunnel(remote);
    close(remote);
    return 0;
}
