/*
 * Copyright (c) 2019 Fastly, Kazuho Oku
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700 /* required for glibc to use getaddrinfo, etc. */
#endif
#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <netdb.h>
#include <stdio.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <openssl/pem.h>
#include "picotls.h"
#include "picotls/openssl.h"
#include "quicly.h"
#include "quicly/streambuf.h"

/**
 * the QUIC context
 */
static quicly_context_t ctx;
/**
 * CID seed
 */
static quicly_cid_plaintext_t next_cid;

static int resolve_address(struct sockaddr *sa, socklen_t *salen, const char *host, const char *port, int family, int type,
                           int proto)
{
    struct addrinfo hints, *res;
    int err;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = family;
    hints.ai_socktype = type;
    hints.ai_protocol = proto;
    hints.ai_flags = AI_ADDRCONFIG | AI_NUMERICSERV | AI_PASSIVE;
    if ((err = getaddrinfo(host, port, &hints, &res)) != 0 || res == NULL) {
        fprintf(stderr, "failed to resolve address:%s:%s:%s\n", host, port,
                err != 0 ? gai_strerror(err) : "getaddrinfo returned NULL");
        return -1;
    }

    memcpy(sa, res->ai_addr, res->ai_addrlen);
    *salen = res->ai_addrlen;

    freeaddrinfo(res);
    return 0;
}

static void usage(const char *progname)
{
    printf("Usage: %s [options] [host]\n"
           "Options:\n"
           "  -c <file>    specifies the certificate chain file (PEM format)\n"
           "  -k <file>    specifies the private key file (PEM format)\n"
           "  -p <number>  specifies the port number (default: 4433)\n"
           "  -E           logs events to stderr\n"
           "  -h           prints this help\n"
           "\n"
           "When both `-c` and `-k` is specified, runs as a server.  Otherwise, runs as a\n"
           "client connecting to host:port.  If omitted, host defaults to 127.0.0.1.\n",
           progname);
    exit(0);
}

static int is_server(void)
{
    return ctx.tls->certificates.count != 0;
}

static int on_stop_sending(quicly_stream_t *stream, int err)
{
    fprintf(stderr, "received STOP_SENDING: %" PRIu16 "\n", QUICLY_ERROR_GET_ERROR_CODE(err));
    quicly_close(stream->conn, QUICLY_ERROR_FROM_APPLICATION_ERROR_CODE(0), "");
    return 0;
}

static int on_receive_reset(quicly_stream_t *stream, int err)
{
    fprintf(stderr, "received RESET_STREAM: %" PRIu16 "\n", QUICLY_ERROR_GET_ERROR_CODE(err));
    quicly_close(stream->conn, QUICLY_ERROR_FROM_APPLICATION_ERROR_CODE(0), "");
    return 0;
}

static int on_receive(quicly_stream_t *stream, size_t off, const void *src, size_t len)
{
    int ret;

    /* read input to receive buffer */
    if ((ret = quicly_streambuf_ingress_receive(stream, off, src, len)) != 0)
        return ret;

    /* obtain contiguous bytes from the receive buffer */
    ptls_iovec_t input = quicly_streambuf_ingress_get(stream);

    if (is_server()) {
        /* server: echo back to the client */
        if (quicly_sendstate_is_open(&stream->sendstate)) {
            quicly_streambuf_egress_write(stream, input.base, input.len);
            /* shutdown the stream after echoing all data */
            if (quicly_recvstate_transfer_complete(&stream->recvstate))
                quicly_streambuf_egress_shutdown(stream);
        }
    } else {
        /* client: print to stdout */
        fwrite(input.base, 1, input.len, stdout);
        fflush(stdout);
        /* initiate connection close after receiving all data */
        if (quicly_recvstate_transfer_complete(&stream->recvstate))
            quicly_close(stream->conn, 0, "");
    }

    /* remove used bytes from receive buffer */
    quicly_streambuf_ingress_shift(stream, input.len);

    return 0;
}

static void process_msg(quicly_conn_t **conn, struct msghdr *msg, size_t dgram_len)
{
    size_t off, packet_len;

    /* split UDP datagram into multiple QUIC packets */
    for (off = 0; off < dgram_len; off += packet_len) {
        quicly_decoded_packet_t decoded;
        if ((packet_len = quicly_decode_packet(&ctx, &decoded, msg->msg_iov[0].iov_base + off, dgram_len - off)) == SIZE_MAX)
            return;
        /* TODO match incoming packets to connections, handle version negotiation, rebinding, retry, etc. */
        if (*conn != NULL) {
            /* let the current connection handle ingress packets */
            quicly_receive(*conn, &decoded);
        } else {
            /* assume that the packet is a new connection */
            quicly_accept(conn, &ctx, msg->msg_name, msg->msg_namelen, &decoded, ptls_iovec_init(NULL, 0), &next_cid, NULL);
        }
    }
}

static int send_one(int fd, quicly_datagram_t *p)
{
    struct iovec vec = {.iov_base = p->data.base, .iov_len = p->data.len};
    struct msghdr mess = {.msg_name = &p->sa, .msg_namelen = p->salen, .msg_iov = &vec, .msg_iovlen = 1};
    int ret;

    while ((ret = (int)sendmsg(fd, &mess, 0)) == -1 && errno == EINTR)
        ;
    return ret;
}

static int run_loop(int fd, quicly_conn_t *conn, int (*stdin_read_cb)(quicly_conn_t *conn))
{
    while (1) {

        /* wait for sockets to become readable, or some event in the QUIC stack to fire */
        fd_set readfds;
        struct timeval *tv, tvbuf;
        do {
            if (conn != NULL) {
                int64_t timeout_msec = quicly_get_first_timeout(conn) - ctx.now->cb(ctx.now);
                if (timeout_msec <= 0) {
                    tvbuf.tv_sec = 0;
                    tvbuf.tv_usec = 0;
                } else {
                    tvbuf.tv_sec = timeout_msec / 1000;
                    tvbuf.tv_usec = timeout_msec % 1000 * 1000;
                }
                tv = &tvbuf;
            } else {
                tv = NULL;
            }
            FD_ZERO(&readfds);
            FD_SET(fd, &readfds);
            /* we want to read input from stdin */
            if (stdin_read_cb != NULL)
                FD_SET(0, &readfds);
        } while (select(fd + 1, &readfds, NULL, NULL, tv) == -1 && errno == EINTR);

        /* read the QUIC fd */
        if (FD_ISSET(fd, &readfds)) {
            uint8_t buf[4096];
            struct sockaddr_storage sa;
            struct iovec vec = {.iov_base = buf, .iov_len = sizeof(buf)};
            struct msghdr msg = {.msg_name = &sa, .msg_namelen = sizeof(sa), .msg_iov = &vec, .msg_iovlen = 1};
            ssize_t rret;
            while ((rret = recvmsg(fd, &msg, 0)) <= 0 && errno == EINTR)
                ;
            if (rret > 0)
                process_msg(&conn, &msg, rret);
        }

        /* read stdin, send the input to the active stram */
        if (FD_ISSET(0, &readfds)) {
            assert(stdin_read_cb != NULL);
            if (!(*stdin_read_cb)(conn))
                stdin_read_cb = NULL;
        }

        /* send QUIC packets, if any */
        if (conn != NULL) {
            quicly_datagram_t *dgrams[16];
            size_t num_dgrams = sizeof(dgrams) / sizeof(dgrams[0]);
            int ret = quicly_send(conn, dgrams, &num_dgrams);
            switch (ret) {
            case 0: {
                size_t i;
                for (i = 0; i != num_dgrams; ++i) {
                    send_one(fd, dgrams[i]);
                    ctx.packet_allocator->free_packet(ctx.packet_allocator, dgrams[i]);
                }
            } break;
            case QUICLY_ERROR_FREE_CONNECTION:
                /* connection has been closed, free, and exit when running as a client */
                quicly_free(conn);
                conn = NULL;
                if (!is_server())
                    return 0;
                break;
            default:
                fprintf(stderr, "quicly_send returned %d\n", ret);
                return 1;
            }
        }
    }

    return 0;
}

static int run_server(int fd, struct sockaddr *sa, socklen_t salen)
{
    /* enter the event loop without any connection object */
    return run_loop(fd, NULL, NULL);
}

static int read_stdin(quicly_conn_t *conn)
{
    quicly_stream_t *stream0;
    char buf[4096];
    size_t rret;

    if ((stream0 = quicly_get_stream(conn, 0)) == NULL || !quicly_sendstate_is_open(&stream0->sendstate))
        return 0;

    while ((rret = read(0, buf, sizeof(buf))) == -1 && errno == EINTR)
        ;
    if (rret == 0) {
        /* stdin closed, close the send-side of stream0 */
        quicly_streambuf_egress_shutdown(stream0);
        return 0;
    } else {
        /* write data to send buffer */
        quicly_streambuf_egress_write(stream0, buf, rret);
        return 1;
    }
}

static int run_client(int fd, const char *host, struct sockaddr *sa, socklen_t salen)
{
    quicly_conn_t *conn;
    int ret;

    /* initiate a connection, and open a stream */
    if ((ret = quicly_connect(&conn, &ctx, host, sa, salen, &next_cid, NULL, NULL)) != 0) {
        fprintf(stderr, "quicly_connect failed:%d\n", ret);
        return 1;
    }
    quicly_stream_t *stream; /* we retain the opened stream via the on_stream_open callback */
    quicly_open_stream(conn, &stream, 0);

    /* enter the event loop with a connection object */
    return run_loop(fd, conn, read_stdin);
}

static int on_stream_open(quicly_stream_open_t *self, quicly_stream_t *stream)
{
    static const quicly_stream_callbacks_t stream_callbacks = {
        quicly_streambuf_destroy, quicly_streambuf_egress_shift, quicly_streambuf_egress_emit, on_stop_sending, on_receive,
        on_receive_reset};
    int ret;

    if ((ret = quicly_streambuf_create(stream, sizeof(quicly_streambuf_t))) != 0)
        return ret;
    stream->callbacks = &stream_callbacks;
    return 0;
}

int main(int argc, char **argv)
{
    ptls_openssl_sign_certificate_t sign_certificate;
    ptls_context_t tlsctx = {
        .random_bytes = ptls_openssl_random_bytes,
        .get_time = &ptls_get_time,
        .key_exchanges = ptls_openssl_key_exchanges,
        .cipher_suites = ptls_openssl_cipher_suites,
    };
    quicly_stream_open_t stream_open = {on_stream_open};
    char *host = "127.0.0.1", *port = "4433";
    struct sockaddr_storage sa;
    socklen_t salen;
    int ch, fd;

    /* setup quic context */
    ctx = quicly_default_context;
    ctx.tls = &tlsctx;
    quicly_amend_ptls_context(ctx.tls);
    ctx.stream_open = &stream_open;

    /* resolve command line options and arguments */
    while ((ch = getopt(argc, argv, "c:k:p:Eh")) != -1) {
        switch (ch) {
        case 'c': /* load certificate chain */ {
            int ret;
            if ((ret = ptls_load_certificates(&tlsctx, optarg)) != 0) {
                fprintf(stderr, "failed to load certificates from file %s:%d\n", optarg, ret);
                exit(1);
            }
        } break;
        case 'k': /* load private key */ {
            FILE *fp;
            if ((fp = fopen(optarg, "r")) == NULL) {
                fprintf(stderr, "failed to open file:%s:%s\n", optarg, strerror(errno));
                exit(1);
            }
            EVP_PKEY *pkey = PEM_read_PrivateKey(fp, NULL, NULL, NULL);
            fclose(fp);
            if (pkey == NULL) {
                fprintf(stderr, "failed to load private key from file:%s\n", optarg);
                exit(1);
            }
            ptls_openssl_init_sign_certificate(&sign_certificate, pkey);
            EVP_PKEY_free(pkey);
            tlsctx.sign_certificate = &sign_certificate.super;
        } break;
        case 'p': /* port */
            port = optarg;
            break;
        case 'E': /* event logging */
            ctx.event_log.cb = quicly_new_default_event_logger(stderr);
            ctx.event_log.mask = UINT64_MAX;
            break;
        case 'h': /* help */
            usage(argv[0]);
            break;
        default:
            exit(1);
            break;
        }
    }
    if ((tlsctx.certificates.count != 0) != (tlsctx.sign_certificate != NULL)) {
        fprintf(stderr, "-c and -k options must be used together\n");
        exit(1);
    }
    argc -= optind;
    argv += optind;
    if (argc != 0)
        host = *argv++;
    if (resolve_address((struct sockaddr *)&sa, &salen, host, port, AF_INET, SOCK_DGRAM, 0) != 0)
        exit(1);

    /* open socket, on the specified port (as a server), or on any port (as a client) */
    if ((fd = socket(sa.ss_family, SOCK_DGRAM, 0)) == -1) {
        perror("socket(2) failed");
        exit(1);
    }
    if (is_server()) {
        int reuseaddr = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuseaddr, sizeof(reuseaddr));
        if (bind(fd, (struct sockaddr *)&sa, salen) != 0) {
            perror("bind(2) failed");
            exit(1);
        }
    } else {
        struct sockaddr_in local;
        memset(&local, 0, sizeof(local));
        if (bind(fd, (struct sockaddr *)&local, sizeof(local)) != 0) {
            perror("bind(2) failed");
            exit(1);
        }
    }

    return is_server() ? run_server(fd, (struct sockaddr *)&sa, salen) : run_client(fd, host, (struct sockaddr *)&sa, salen);
}
