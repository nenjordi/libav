/*
 * TCP protocol
 * Copyright (c) 2002 Fabrice Bellard
 *
 * This file is part of Libav.
 *
 * Libav is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * Libav is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with Libav; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */
#include "avformat.h"
#include "libavutil/parseutils.h"
#include "internal.h"
#include "network.h"
#include "os_support.h"
#include "url.h"
#if HAVE_POLL_H
#include <poll.h>
#endif

#define MAX_TCP_INCOMING_CONNECTION_QUEUE_SIZE 10

typedef struct TCPContext {
    int fd;
    int accept_flag;
    int *serversocks;
    int nb_serversocks;
} TCPContext;

/* return non zero if error */
static int tcp_open(URLContext *h, const char *uri, int flags)
{
    struct addrinfo hints = { 0 }, *ai, *cur_ai;
    int port, fd = -1;
    TCPContext *s = h->priv_data;
    int listen_socket = 0;
    const char *p;
    char buf[256];
    int ret;
    socklen_t optlen;
    int timeout = 100, listen_timeout = -1;
    char hostname[1024],proto[1024],path[1024];
    char portstr[10];
    int sockidx = 0;

    av_url_split(proto, sizeof(proto), NULL, 0, hostname, sizeof(hostname),
        &port, path, sizeof(path), uri);
    if (strcmp(proto, "tcp"))
        return AVERROR(EINVAL);
    if (port <= 0 || port >= 65536) {
        av_log(h, AV_LOG_ERROR, "Port missing in uri\n");
        return AVERROR(EINVAL);
    }
    p = strchr(uri, '?');
    if (p) {
        if (av_find_info_tag(buf, sizeof(buf), "listen", p))
            listen_socket = 1;
        if (av_find_info_tag(buf, sizeof(buf), "timeout", p)) {
            timeout = strtol(buf, NULL, 10);
        }
        if (av_find_info_tag(buf, sizeof(buf), "listen_timeout", p)) {
            listen_timeout = strtol(buf, NULL, 10);
        }
        if (av_find_info_tag(buf, sizeof(buf), "accept", p)) {
            listen_socket  = 1;
            s->accept_flag = 1;
        }
    }
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(portstr, sizeof(portstr), "%d", port);
    if (listen_socket)
        hints.ai_flags |= AI_PASSIVE;
    if (!hostname[0])
        ret = getaddrinfo(NULL, portstr, &hints, &ai);
    else
        ret = getaddrinfo(hostname, portstr, &hints, &ai);
    if (ret) {
        av_log(h, AV_LOG_ERROR,
               "Failed to resolve hostname %s: %s\n",
               hostname, gai_strerror(ret));
        return AVERROR(EIO);
    }

    if (s->accept_flag) {
        int reuse;
        int isblocking = h->flags & AVIO_FLAG_NONBLOCK;
        s->nb_serversocks = 0;
        cur_ai = ai;
        do {
            s->nb_serversocks++;
            cur_ai = cur_ai->ai_next;
        } while (cur_ai);
        if (!s->nb_serversocks)
            return AVERROR(EIO);
        s->serversocks = av_malloc(sizeof(int) * s->nb_serversocks);
        if (!s->serversocks)
            return AVERROR(ENOMEM);
        // Calculate number of addresses and initialize one socket per address
        cur_ai  = ai;
        sockidx = 0;
        do {
            fd = socket(cur_ai->ai_family, cur_ai->ai_socktype | isblocking,
                        cur_ai->ai_protocol);
            if (fd < 0)
                goto check_next_acceptable;
            setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
            ret = bind(fd, cur_ai->ai_addr, cur_ai->ai_addrlen);
            if (ret) {
                ret = ff_neterrno();
                goto failacceptable;
            }
            s->serversocks[sockidx] = fd;
            sockidx++;
        check_next_acceptable:
            cur_ai = cur_ai->ai_next;
        } while (cur_ai);
        freeaddrinfo(ai);
        s->nb_serversocks = sockidx; // Number of binded connections
        return 0;
    failacceptable:
        av_log(h, AV_LOG_ERROR, "Open Acceptable tcp socket failed");
        for (sockidx = 0; sockidx < s->nb_serversocks; sockidx++) {
            close(s->serversocks[sockidx]);
        }
        av_free(s->serversocks);
        freeaddrinfo(ai);
        return ret;
    }
    cur_ai = ai;

 restart:
    ret = AVERROR(EIO);
    fd = socket(cur_ai->ai_family, cur_ai->ai_socktype, cur_ai->ai_protocol);
    if (fd < 0)
        goto fail;

    if (listen_socket) {
        int fd1;
        int reuse = 1;
        struct pollfd lp = { fd, POLLIN, 0 };
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        ret = bind(fd, cur_ai->ai_addr, cur_ai->ai_addrlen);
        if (ret) {
            ret = ff_neterrno();
            goto fail1;
        }
        ret = listen(fd, 1);
        if (ret) {
            ret = ff_neterrno();
            goto fail1;
        }
        ret = poll(&lp, 1, listen_timeout >= 0 ? listen_timeout : -1);
        if (ret <= 0) {
            ret = AVERROR(ETIMEDOUT);
            goto fail1;
        }
        fd1 = accept(fd, NULL, NULL);
        if (fd1 < 0) {
            ret = ff_neterrno();
            goto fail1;
        }
        closesocket(fd);
        fd = fd1;
        ff_socket_nonblock(fd, 1);
        s->fd = fd;
        return 0;
    } else {
 redo:
        ff_socket_nonblock(fd, 1);
        ret = connect(fd, cur_ai->ai_addr, cur_ai->ai_addrlen);
    }

    if (ret < 0) {
        struct pollfd p = {fd, POLLOUT, 0};
        ret = ff_neterrno();
        if (ret == AVERROR(EINTR)) {
            if (ff_check_interrupt(&h->interrupt_callback)) {
                ret = AVERROR_EXIT;
                goto fail1;
            }
            goto redo;
        }
        if (ret != AVERROR(EINPROGRESS) &&
            ret != AVERROR(EAGAIN))
            goto fail;

        /* wait until we are connected or until abort */
        while(timeout--) {
            if (ff_check_interrupt(&h->interrupt_callback)) {
                ret = AVERROR_EXIT;
                goto fail1;
            }
            ret = poll(&p, 1, 100);
            if (ret > 0)
                break;
        }
        if (ret <= 0) {
            ret = AVERROR(ETIMEDOUT);
            goto fail;
        }
        /* test error */
        optlen = sizeof(ret);
        if (getsockopt (fd, SOL_SOCKET, SO_ERROR, &ret, &optlen))
            ret = AVUNERROR(ff_neterrno());
        if (ret != 0) {
            char errbuf[100];
            ret = AVERROR(ret);
            av_strerror(ret, errbuf, sizeof(errbuf));
            av_log(h, AV_LOG_ERROR,
                   "TCP connection to %s:%d failed: %s\n",
                   hostname, port, errbuf);
            goto fail;
        }
    }
    h->is_streamed = 1;
    s->fd = fd;
    freeaddrinfo(ai);
    return 0;

 fail:
    if (cur_ai->ai_next) {
        /* Retry with the next sockaddr */
        cur_ai = cur_ai->ai_next;
        if (fd >= 0)
            closesocket(fd);
        goto restart;
    }
 fail1:
    if (fd >= 0)
        closesocket(fd);
    freeaddrinfo(ai);
    return ret;
}

static int tcp_listen(URLContext *srvctx, const char *uri, int flags)
{
    TCPContext *s      = srvctx->priv_data;
    int ret            = AVERROR_BUG;
    int listen_timeout = -1;
    int listen_socket  = 0;
    char hostname[1024], proto[1024], path[1024];
    const char *p;
    char buf[256];
    int port;
    av_url_split(proto, sizeof(proto), NULL, 0, hostname, sizeof(hostname),
        &port, path, sizeof(path), uri);
    if (strcmp(proto, "tcp") || port <= 0 || port >= 65536)
        return AVERROR(EINVAL);

    p = strchr(uri, '?');
    if (p) {
        if (av_find_info_tag(buf, sizeof(buf), "listen", p))
            listen_socket = 1;
        if (av_find_info_tag(buf, sizeof(buf), "listen_timeout", p)) {
            listen_timeout = strtol(buf, NULL, 10);
        }
        if (av_find_info_tag(buf, sizeof(buf), "accept", p)) {
            listen_socket  = 1;
            s->accept_flag = 1;
        }
    }
    if (s->accept_flag) {
        int sockidx = 0;
        int ret;
        if (!s->nb_serversocks) {
            // Auto call tcp_open
            tcp_open(srvctx, uri, flags);
        }
        for (;sockidx < s->nb_serversocks; sockidx++) {
            if (!s->serversocks[sockidx]) {
                av_log(s, AV_LOG_ERROR, "Invalid socket to listen\n");
                return AVERROR(ENOTSOCK);
            }
            ret = listen(s->serversocks[sockidx],
                         MAX_TCP_INCOMING_CONNECTION_QUEUE_SIZE);
            if (ret)
                goto listenerror;
        }
    } else if (listen_socket) {
        int reuse = 1;
        int fd    = 0;
        struct pollfd lp = { fd, POLLIN, 0 };
        struct addrinfo hints = { 0 }, *ai, *cur_ai;
        char portstr[10];


        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        snprintf(portstr, sizeof(portstr), "%d", port);
        hints.ai_flags |= AI_PASSIVE;
        if (!hostname[0])
            ret = getaddrinfo(NULL, portstr, &hints, &ai);
        else
            ret = getaddrinfo(hostname, portstr, &hints, &ai);

        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        cur_ai = ai;
    retry:
        ret = bind(fd, cur_ai->ai_addr, cur_ai->ai_addrlen);
        if (ret) {
            ret = ff_neterrno();
            goto listen1error;
        }
        ret = listen(fd, 1);
        if (ret) {
            ret = ff_neterrno();
            goto listen1error;
        }
        ret = poll(&lp, 1, listen_timeout >= 0 ? listen_timeout : -1);
        if (ret <= 0) {
            ret = AVERROR(ETIMEDOUT);
            goto listen1error;
        }
        s->fd = fd;
        return 0;
    listen1error:
        if (cur_ai->ai_next) {
            cur_ai = cur_ai->ai_next;
            goto retry;
        }
        goto listenerror;
    }
    return 0;
listenerror:
    if (s->fd >= 0)
        closesocket(s->fd);
    return ret;
}

static int tcp_accept(URLContext *srvctx, URLContext **clctx, int timeout)
{
    TCPContext *atcpctx = srvctx->priv_data;
    TCPContext *s;
    int serversock;
    int serversockidx = 0;
    int tout          = timeout;
    int fd;
    int ret;

    struct pollfd *pollst = NULL;
    if (!atcpctx->accept_flag) {
        av_log(srvctx, AV_LOG_ERROR, "Usage of tcp_accept() without having"
               " set accept flag\n");
        return AVERROR(ENOSYS);
    }

    if (srvctx->flags & AVIO_FLAG_NONBLOCK)
        tout = 0;

    pollst = av_malloc(sizeof(struct pollfd *)
                       * atcpctx->nb_serversocks);
     for (serversockidx = 0; serversockidx < atcpctx->nb_serversocks;
          serversockidx++) {
         pollst[serversockidx].fd =
             atcpctx->serversocks[serversockidx];
         pollst[serversockidx].events = POLLIN | POLLPRI;
     }

     if ((ret = poll(pollst, atcpctx->nb_serversocks, tout)) > 0) {
         int pollidx = 0;
         for (; pollidx < atcpctx->nb_serversocks; pollidx++) {
             if (pollst[pollidx].revents & (POLLIN | POLLPRI)) {
                 serversock     = pollst[pollidx].fd;
                 fd             = accept(serversock, NULL, NULL);
                 if (fd < 0) {
                     ret = fd;
                     goto accept_error;
                 }
                 ret = ffurl_alloc(clctx, srvctx->filename, 0, NULL);
                 if (ret)
                     goto accept_error;
                 s = (*clctx)->priv_data;
                 s->fd                 = fd;
                 (*clctx)->is_streamed = 1;
                 (*clctx)->flags       = srvctx->flags;
                 av_free(pollst);
                 return 0;
             }
         }
     }
     if (!ret) // poll timed out
         return 0;
accept_error:
     av_free(pollst);
     av_log(atcpctx, AV_LOG_ERROR, "Unable to Accept\n");
     return AVERROR(ret);
}

static int tcp_read(URLContext *h, uint8_t *buf, int size)
{
    TCPContext *s = h->priv_data;
    int ret;

    if (!(h->flags & AVIO_FLAG_NONBLOCK)) {
        ret = ff_network_wait_fd(s->fd, 0);
        if (ret < 0)
            return ret;
    }
    ret = recv(s->fd, buf, size, 0);
    return ret < 0 ? ff_neterrno() : ret;
}

static int tcp_write(URLContext *h, const uint8_t *buf, int size)
{
    TCPContext *s = h->priv_data;
    int ret;

    if (!(h->flags & AVIO_FLAG_NONBLOCK)) {
        ret = ff_network_wait_fd(s->fd, 1);
        if (ret < 0)
            return ret;
    }
    ret = send(s->fd, buf, size, 0);
    return ret < 0 ? ff_neterrno() : ret;
}

static int tcp_shutdown(URLContext *h, int flags)
{
    TCPContext *s = h->priv_data;
    int how;

    if (flags & AVIO_FLAG_WRITE && flags & AVIO_FLAG_READ) {
        how = SHUT_RDWR;
    } else if (flags & AVIO_FLAG_WRITE) {
        how = SHUT_WR;
    } else {
        how = SHUT_RD;
    }

    return shutdown(s->fd, how);
}

static int tcp_close(URLContext *h)
{
    TCPContext *s = h->priv_data;
    closesocket(s->fd);
    av_free(s->serversocks);
    return 0;
}

static int tcp_get_file_handle(URLContext *h)
{
    TCPContext *s = h->priv_data;
    return s->fd;
}

URLProtocol ff_tcp_protocol = {
    .name                = "tcp",
    .url_open            = tcp_open,
    .url_read            = tcp_read,
    .url_write           = tcp_write,
    .url_close           = tcp_close,
    .url_get_file_handle = tcp_get_file_handle,
    .url_shutdown        = tcp_shutdown,
    .url_accept          = tcp_accept,
    .url_listen          = tcp_listen,
    .priv_data_size      = sizeof(TCPContext),
    .flags               = URL_PROTOCOL_FLAG_NETWORK,
};
