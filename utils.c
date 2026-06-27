#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "utils.h"

int read_line(int fd, char *buf, int max) {
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

int copy_data(int from_fd, int to_fd) {
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

int connect_to(const char *host, const char *port) {
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

void tunnel(int remote_fd) {
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

char *parse_host_port(char *url, char *host, int host_size,
                char *port, int port_size) {
    char *p = url;
    int has_scheme = 0;

    if (strncmp(p, "http://", 7) == 0) {
        p += 7;
        has_scheme = 1;
    } else if (strncmp(p, "https://", 8) == 0) {
        p += 8;
        has_scheme = 1;
    }

    if (!has_scheme) {
        size_t slen = strlen(url);
        if (slen >= (size_t)host_size) slen = host_size - 1;
        memcpy(host, url, slen);
        host[slen] = '\0';
        return "/";
    }

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

void base64_encode(const unsigned char *in, int in_len, char *out, int out_max) {
    static const char *tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int i, j = 0;
    for (i = 0; i < in_len; i += 3) {
        int remaining = in_len - i;
        unsigned int b = (in[i] << 16) | (i+1 < in_len ? in[i+1] << 8 : 0) | (i+2 < in_len ? in[i+2] : 0);
        if (j < out_max) out[j++] = tbl[(b >> 18) & 0x3F];
        if (j < out_max) out[j++] = tbl[(b >> 12) & 0x3F];
        if (j < out_max) {
            if (remaining >= 2) out[j++] = tbl[(b >> 6) & 0x3F];
            else out[j++] = '=';
        }
        if (j < out_max) {
            if (remaining >= 3) out[j++] = tbl[b & 0x3F];
            else out[j++] = '=';
        }
    }
    if (j < out_max) out[j] = '\0';
}

int check_auth(const char *val, const char *expected_b64) {
    const char *p = val;
    while (*p == ' ') p++;
    if (strncmp(p, "Basic ", 6) != 0)
        return 0;
    p += 6;
    while (*p == ' ') p++;
    return strcmp(p, expected_b64) == 0;
}
