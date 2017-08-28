/*
 * Copyright (c) 2017 Fastly, Kazuho Oku
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
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include "khash.h"
#include "quicly.h"
#include "quicly/ack.h"
#include "quicly/frame.h"

#define QUICLY_PROTOCOL_VERSION 0xff000005

#define QUICLY_PACKET_TYPE_VERSION_NEGOTIATION 1
#define QUICLY_PACKET_TYPE_CLIENT_INITIAL 2
#define QUICLY_PACKET_TYPE_SERVER_STATELESS_RETRY 3
#define QUICLY_PACKET_TYPE_SERVER_CLEARTEXT 4
#define QUICLY_PACKET_TYPE_CLIENT_CLEARTEXT 5
#define QUICLY_PACKET_TYPE_0RTT_PROTECTED 6
#define QUICLY_PACKET_TYPE_1RTT_KEY_PHASE_0 7
#define QUICLY_PACKET_TYPE_1RTT_KEY_PHASE_1 8
#define QUICLY_PACKET_TYPE_PUBLIC_RESET 8
#define QUICLY_PACKET_TYPE_IS_VALID(type) ((uint8_t)(type)-1 < QUICLY_PACKET_TYPE_PUBLIC_RESET)

#define QUICLY_TLS_EXTENSION_TYPE_TRANSPORT_PARAMETERS 26
#define QUICLY_TRANSPORT_PARAMETER_ID_INITIAL_MAX_STREAM_DATA 0
#define QUICLY_TRANSPORT_PARAMETER_ID_INITIAL_MAX_DATA 1
#define QUICLY_TRANSPORT_PARAMETER_ID_INITIAL_MAX_STREAM_ID 2
#define QUICLY_TRANSPORT_PARAMETER_ID_IDLE_TIMEOUT 3
#define QUICLY_TRANSPORT_PARAMETER_ID_TRUNCATE_CONNECTION_ID 4

#define GET_TYPE_FROM_PACKET_HEADER(p) (*(uint8_t *)(p)&0x1f)

KHASH_MAP_INIT_INT(quicly_stream_t, quicly_stream_t *)

#define QUICLY_DEBUG_LOG(conn, stream_id, ...)                                                                                     \
    if (0) {                                                                                                                       \
        quicly_conn_t *c = (conn);                                                                                                 \
        char buf[1024];                                                                                                            \
        snprintf(buf, sizeof(buf), __VA_ARGS__);                                                                                   \
        fprintf(stderr, "%s:%p,%" PRIu32 ": %s\n", quicly_is_client(c) ? "client" : "server", (c), (stream_id), buf);              \
    }

struct st_quicly_packet_protection_t {
    uint64_t packet_number;
    struct {
        ptls_aead_context_t *early_data;
        ptls_aead_context_t *key_phase0;
        ptls_aead_context_t *key_phase1;
    } aead;
    uint8_t secret[PTLS_MAX_DIGEST_SIZE];
};

struct st_quicly_conn_t {
    struct _st_quicly_conn_public_t super;
    /**
     * hashtable of streams
     */
    khash_t(quicly_stream_t) * streams;
    struct {
        /**
         * crypto parameters
         */
        struct st_quicly_packet_protection_t pp;
        /**
         * acks to be sent to peer
         */
        quicly_ranges_t ack_queue;
        /**
         *
         */
        struct {
            __uint128_t bytes_consumed;
            quicly_maxsender_t sender;
        } max_data;
    } ingress;
    struct {
        /**
         * crypto parameters
         */
        struct st_quicly_packet_protection_t pp;
        /**
         * contains actions that needs to be performed when an ack is being received
         */
        quicly_acks_t acks;
        /**
         *
         */
        uint64_t packet_number;
        /**
         *
         */
        struct {
            __uint128_t permitted;
            __uint128_t sent;
        } max_data;
        /**
         *
         */
        unsigned acks_require_encryption : 1;
    } egress;
};

struct st_quicly_crypto_stream_data_t {
    quicly_conn_t *conn;
    ptls_t *tls;
    ptls_handshake_properties_t handshake_properties;
    struct {
        ptls_raw_extension_t ext[2];
        ptls_buffer_t buf;
    } transport_parameters;
};

static const quicly_transport_parameters_t transport_params_before_handshake = {8192, 16, 100, 60, 0};

#define FNV1A_OFFSET_BASIS ((uint64_t)14695981039346656037u)

static uint64_t fnv1a(uint64_t hash, const uint8_t *p, const uint8_t *end)
{
    while (p != end) {
        hash = hash ^ (uint64_t)*p++;
        hash *= 1099511628211u;
    }

    return hash;
}

static int verify_cleartext_packet(quicly_decoded_packet_t *packet)
{
    uint64_t calced, received;
    const uint8_t *p;

    if (packet->payload.len < 8)
        return 0;
    packet->payload.len -= 8;

    calced = fnv1a(FNV1A_OFFSET_BASIS, packet->header.base, packet->header.base + packet->header.len);
    calced = fnv1a(calced, packet->payload.base, packet->payload.base + packet->payload.len);

    p = packet->payload.base + packet->payload.len;
    received = quicly_decode64(&p);

    return calced == received;
}

static void free_packet_protection(struct st_quicly_packet_protection_t *pp)
{
    if (pp->aead.early_data != NULL)
        ptls_aead_free(pp->aead.early_data);
    if (pp->aead.key_phase0 != NULL)
        ptls_aead_free(pp->aead.key_phase0);
    if (pp->aead.key_phase1 != NULL)
        ptls_aead_free(pp->aead.key_phase1);
}

int quicly_decode_packet(quicly_decoded_packet_t *packet, const uint8_t *src, size_t len)
{
    if (len < 2)
        return QUICLY_ERROR_INVALID_PACKET_HEADER;

    packet->header.base = (void *)src;

    const uint8_t *src_end = src + len;
    uint8_t first_byte = *src++;

    if ((first_byte & 0x80) != 0) {
        /* long header */
        packet->type = first_byte & 0x7f;
        packet->is_long_header = 1;
        packet->has_connection_id = 1;
        if (!QUICLY_PACKET_TYPE_IS_VALID(packet->type))
            return QUICLY_ERROR_INVALID_PACKET_HEADER;
        if (src_end - src < 16)
            return QUICLY_ERROR_INVALID_PACKET_HEADER;
        packet->connection_id = quicly_decode64(&src);
        packet->packet_number = quicly_decode32(&src);
        packet->version = quicly_decode32(&src);
    } else {
        /* short header */
        packet->type = (first_byte & 0x20) != 0 ? QUICLY_PACKET_TYPE_1RTT_KEY_PHASE_1 : QUICLY_PACKET_TYPE_1RTT_KEY_PHASE_0;
        packet->is_long_header = 0;
        if ((first_byte & 0x40) != 0) {
            packet->has_connection_id = 1;
            if (src_end - src < 8)
                return QUICLY_ERROR_INVALID_PACKET_HEADER;
            packet->connection_id = quicly_decode64(&src);
        } else {
            packet->has_connection_id = 0;
        }
        unsigned type = first_byte & 0x1f, packet_number_size;
        switch (type) {
        case 1:
        case 2:
        case 3:
            packet_number_size = 1 << (type - 1);
            break;
        default:
            return QUICLY_ERROR_INVALID_PACKET_HEADER;
        }
        if (src_end - src < packet_number_size)
            return QUICLY_ERROR_INVALID_PACKET_HEADER;
        packet->packet_number = (uint32_t)quicly_decodev(&src, packet_number_size);
    }

    packet->header.len = src - packet->header.base;
    packet->payload = ptls_iovec_init(src, src_end - src);
    return 0;
}

static uint8_t *emit_long_header(quicly_conn_t *conn, uint8_t *dst, uint8_t type, uint64_t connection_id,
                                 uint32_t rounded_packet_number)
{
    *dst++ = 0x80 | type;
    dst = quicly_encode64(dst, connection_id);
    dst = quicly_encode32(dst, rounded_packet_number);
    dst = quicly_encode32(dst, QUICLY_PROTOCOL_VERSION);
    return dst;
}

static int set_peeraddr(quicly_conn_t *conn, struct sockaddr *addr, socklen_t addrlen)
{
    int ret;

    if (conn->super.peer.salen != addrlen) {
        struct sockaddr *newsa;
        if ((newsa = malloc(addrlen)) == NULL) {
            ret = PTLS_ERROR_NO_MEMORY;
            goto Exit;
        }
        free(conn->super.peer.sa);
        conn->super.peer.sa = newsa;
        conn->super.peer.salen = addrlen;
    }

    memcpy(conn->super.peer.sa, addr, addrlen);
    ret = 0;

Exit:
    return ret;
}

static void on_sendbuf_change(quicly_sendbuf_t *buf, int err)
{
    quicly_stream_t *stream = (void *)((char *)buf - offsetof(quicly_stream_t, sendbuf));
    assert(stream->stream_id != 0 || buf->eos == UINT64_MAX);
}

static void on_recvbuf_change(quicly_recvbuf_t *buf, int err, size_t shift_amount)
{
    quicly_stream_t *stream = (void *)((char *)buf - offsetof(quicly_stream_t, recvbuf));
    quicly_conn_t *conn = stream->conn;

    if (shift_amount != 0 && stream->stream_id != 0)
        conn->ingress.max_data.bytes_consumed += shift_amount;
}

static quicly_stream_t *open_stream(quicly_conn_t *conn, uint32_t stream_id)
{
    quicly_stream_t *stream;

    if ((stream = malloc(sizeof(*stream))) == NULL)
        return NULL;

    stream->conn = conn;
    stream->stream_id = stream_id;
    quicly_sendbuf_init(&stream->sendbuf, on_sendbuf_change);
    quicly_recvbuf_init(&stream->recvbuf, on_recvbuf_change);
    stream->data = NULL;
    stream->on_update = NULL;

    stream->_send_aux.max_stream_data = conn->super.peer.transport_params.initial_max_stream_data;
    stream->_send_aux.max_sent = 0;
    stream->_send_aux.stop_sending.sender_state = QUICLY_SENDER_STATE_NONE;
    stream->_send_aux.stop_sending.reason = 0;
    stream->_send_aux.rst.sender_state = QUICLY_SENDER_STATE_NONE;
    stream->_send_aux.rst.reason = 0;
    quicly_maxsender_init(&stream->_send_aux.max_stream_data_sender, conn->super.ctx->transport_params.initial_max_stream_data);

    stream->_recv_aux.window = conn->super.ctx->transport_params.initial_max_stream_data;
    stream->_recv_aux.rst_reason = QUICLY_ERROR_FIN_CLOSED;

    stream->_close_called = 0;

    int r;
    khiter_t iter = kh_put(quicly_stream_t, conn->streams, stream_id, &r);
    assert(iter != kh_end(conn->streams));
    kh_val(conn->streams, iter) = stream;

    return stream;
}

static void destroy_stream(quicly_stream_t *stream)
{
    quicly_conn_t *conn = stream->conn;
    khiter_t iter = kh_get(quicly_stream_t, conn->streams, stream->stream_id);
    assert(iter != kh_end(conn->streams));
    kh_del(quicly_stream_t, conn->streams, iter);

    quicly_sendbuf_dispose(&stream->sendbuf);
    quicly_recvbuf_dispose(&stream->recvbuf);
    quicly_maxsender_dispose(&stream->_send_aux.max_stream_data_sender);

    if (stream->stream_id != 0) {
        if (quicly_is_client(conn) == stream->stream_id % 2) {
            --conn->super.host.num_streams;
        } else {
            --conn->super.peer.num_streams;
        }
    }
    free(stream);
}

static inline int destroy_stream_if_unneeded(quicly_stream_t *stream)
{
    if (!stream->_close_called)
        return 0;
    if (!(quicly_sendbuf_transfer_complete(&stream->sendbuf) || stream->_send_aux.rst.sender_state == QUICLY_SENDER_STATE_ACKED))
        return 0;
    if (!quicly_recvbuf_transfer_complete(&stream->recvbuf))
        return 0;

    destroy_stream(stream);
    return 1;
}

quicly_stream_t *quicly_get_stream(quicly_conn_t *conn, uint32_t stream_id)
{
    khiter_t iter = kh_get(quicly_stream_t, conn->streams, stream_id);
    if (iter != kh_end(conn->streams))
        return kh_val(conn->streams, iter);
    return NULL;
}

void quicly_free(quicly_conn_t *conn)
{
    quicly_stream_t *stream;

    free_packet_protection(&conn->ingress.pp);
    quicly_ranges_dispose(&conn->ingress.ack_queue);
    quicly_maxsender_dispose(&conn->ingress.max_data.sender);
    free_packet_protection(&conn->egress.pp);
    quicly_acks_dispose(&conn->egress.acks);

    kh_foreach_value(conn->streams, stream, { destroy_stream(stream); });
    kh_destroy(quicly_stream_t, conn->streams);

    free(conn->super.peer.sa);
    free(conn);
}

static int setup_1rtt_secret(struct st_quicly_packet_protection_t *pp, ptls_t *tls, const char *label, int is_enc)
{
    ptls_cipher_suite_t *cipher = ptls_get_cipher(tls);
    int ret;

    if ((ret = ptls_export_secret(tls, pp->secret, cipher->hash->digest_size, label, ptls_iovec_init(NULL, 0))) != 0)
        return ret;
    if ((pp->aead.key_phase0 = ptls_aead_new(cipher->aead, cipher->hash, is_enc, pp->secret)) == NULL)
        return PTLS_ERROR_NO_MEMORY;
    return 0;
}

static int setup_1rtt(quicly_conn_t *conn, ptls_t *tls)
{
    static const char *labels[2] = {"EXPORTER-QUIC client 1-RTT Secret", "EXPORTER-QUIC server 1-RTT Secret"};
    int ret;

    if ((ret = setup_1rtt_secret(&conn->ingress.pp, tls, labels[quicly_is_client(conn)], 0)) != 0)
        goto Exit;
    if ((ret = setup_1rtt_secret(&conn->egress.pp, tls, labels[!quicly_is_client(conn)], 1)) != 0)
        goto Exit;

    conn->super.state = QUICLY_STATE_1RTT_ENCRYPTED;

Exit:
    return 0;
}

static void senddata_free(struct st_quicly_buffer_vec_t *vec)
{
    free(vec->p);
    free(vec);
}

static void write_tlsbuf(quicly_stream_t *stream, ptls_buffer_t *tlsbuf)
{
    if (tlsbuf->off != 0) {
        assert(tlsbuf->is_allocated);
        quicly_sendbuf_write(&stream->sendbuf, tlsbuf->base, tlsbuf->off, senddata_free);
        ptls_buffer_init(tlsbuf, "", 0);
    } else {
        assert(!tlsbuf->is_allocated);
    }
}

static int crypto_stream_receive_handshake(quicly_stream_t *stream)
{
    struct st_quicly_crypto_stream_data_t *data = stream->data;
    ptls_iovec_t input;
    ptls_buffer_t buf;
    int ret = PTLS_ERROR_IN_PROGRESS;

    ptls_buffer_init(&buf, "", 0);
    while (ret == PTLS_ERROR_IN_PROGRESS && (input = quicly_recvbuf_get(&stream->recvbuf)).len != 0) {
        ret = ptls_handshake(data->tls, &buf, input.base, &input.len, &data->handshake_properties);
        quicly_recvbuf_shift(&stream->recvbuf, input.len);
    }
    write_tlsbuf(stream, &buf);

    switch (ret) {
    case 0:
        QUICLY_DEBUG_LOG(stream->conn, 0, "handshake complete");
        /* state is 1RTT_ENCRYPTED when handling ClientFinished */
        if (stream->conn->super.state < QUICLY_STATE_1RTT_ENCRYPTED) {
            stream->conn->egress.max_data.permitted =
                (__uint128_t)stream->conn->super.peer.transport_params.initial_max_data_kb * 1024;
            if ((ret = setup_1rtt(stream->conn, data->tls)) != 0)
                goto Exit;
        }
        break;
    case PTLS_ERROR_IN_PROGRESS:
        if (stream->conn->super.state == QUICLY_STATE_BEFORE_SH)
            stream->conn->super.state = QUICLY_STATE_BEFORE_SF;
        ret = 0;
        break;
    default:
        break;
    }

Exit:
    return ret;
}

static int do_apply_stream_frame(quicly_conn_t *conn, quicly_stream_t *stream, uint64_t off, ptls_iovec_t data)
{
    int ret;

    /* make adjustments for retransmit */
    if (off < stream->recvbuf.data_off) {
        if (off + data.len <= stream->recvbuf.data_off)
            return 0;
        size_t delta = stream->recvbuf.data_off - off;
        off = stream->recvbuf.data_off;
        data.base += delta;
        data.len -= delta;
    }

    /* try the fast (copyless) path */
    if (stream->recvbuf.data_off == off && stream->recvbuf.data.len == 0) {
        struct st_quicly_buffer_vec_t vec = {NULL};
        assert(stream->recvbuf.received.num_ranges == 1);
        assert(stream->recvbuf.received.ranges[0].end == stream->recvbuf.data_off);

        if (data.len != 0) {
            stream->recvbuf.received.ranges[0].end += data.len;
            quicly_buffer_set_fast_external(&stream->recvbuf.data, &vec, data.base, data.len);
        }
        if ((ret = stream->on_update(stream)) != 0)
            return ret;
        /* stream might have been destroyed; in such case vec.len would be zero (see quicly_buffer_dispose) */
        if (vec.len != 0 && stream->recvbuf.data.len != 0) {
            size_t keeplen = stream->recvbuf.data.len;
            quicly_buffer_init(&stream->recvbuf.data);
            if ((ret = quicly_buffer_push(&stream->recvbuf.data, data.base + data.len - keeplen, keeplen, NULL)) != 0)
                return ret;
        }
        return 0;
    }

    uint64_t prev_end = stream->recvbuf.received.ranges[0].end;
    if ((ret = quicly_recvbuf_write(&stream->recvbuf, off, data.base, data.len)) != 0)
        return ret;
    if (prev_end != stream->recvbuf.received.ranges[0].end || prev_end == stream->recvbuf.eos)
        ret = stream->on_update(stream);
    return ret;
}

static int apply_stream_frame(quicly_conn_t *conn, quicly_stream_t *stream, quicly_stream_frame_t *frame)
{
    int ret;

    QUICLY_DEBUG_LOG(conn, stream->stream_id, "received; off=%" PRIu64 ",len=%zu", frame->offset, frame->data.len);

    if (frame->is_fin && (ret = quicly_recvbuf_mark_eos(&stream->recvbuf, frame->offset + frame->data.len)) != 0)
        return ret;

    return do_apply_stream_frame(conn, stream, frame->offset, frame->data);
}

#define PUSH_TRANSPORT_PARAMETER(buf, id, block)                                                                                   \
    do {                                                                                                                           \
        ptls_buffer_push16((buf), (id));                                                                                           \
        ptls_buffer_push_block((buf), 2, block);                                                                                   \
    } while (0)

static int encode_transport_parameter_list(quicly_transport_parameters_t *params, ptls_buffer_t *buf)
{
    int ret;

    ptls_buffer_push_block(buf, 2, {
        PUSH_TRANSPORT_PARAMETER(buf, QUICLY_TRANSPORT_PARAMETER_ID_INITIAL_MAX_STREAM_DATA,
                                 { ptls_buffer_push32(buf, params->initial_max_stream_data); });
        PUSH_TRANSPORT_PARAMETER(buf, QUICLY_TRANSPORT_PARAMETER_ID_INITIAL_MAX_DATA,
                                 { ptls_buffer_push32(buf, params->initial_max_data_kb); });
        PUSH_TRANSPORT_PARAMETER(buf, QUICLY_TRANSPORT_PARAMETER_ID_INITIAL_MAX_STREAM_ID,
                                 { ptls_buffer_push32(buf, params->initial_max_stream_id); });
        PUSH_TRANSPORT_PARAMETER(buf, QUICLY_TRANSPORT_PARAMETER_ID_IDLE_TIMEOUT,
                                 { ptls_buffer_push16(buf, params->idle_timeout); });
        if (params->truncate_connection_id)
            PUSH_TRANSPORT_PARAMETER(buf, QUICLY_TRANSPORT_PARAMETER_ID_TRUNCATE_CONNECTION_ID, {});
    });
    ret = 0;
Exit:
    return ret;
}

static int decode_transport_parameter_list(quicly_transport_parameters_t *params, const uint8_t *src, const uint8_t *end)
{
#define ID_TO_BIT(id) ((uint64_t)1 << (id))

    uint64_t found_id_bits = 0,
             must_found_id_bits = ID_TO_BIT(QUICLY_TRANSPORT_PARAMETER_ID_INITIAL_MAX_STREAM_DATA) |
                                  ID_TO_BIT(QUICLY_TRANSPORT_PARAMETER_ID_INITIAL_MAX_DATA) |
                                  ID_TO_BIT(QUICLY_TRANSPORT_PARAMETER_ID_INITIAL_MAX_STREAM_ID) |
                                  ID_TO_BIT(QUICLY_TRANSPORT_PARAMETER_ID_IDLE_TIMEOUT);
    int ret;

    /* set optional parameters to their default values */
    params->truncate_connection_id = 0;

    /* decode the parameters block */
    ptls_decode_block(src, end, 2, {
        while (src != end) {
            uint16_t id;
            if ((ret = ptls_decode16(&id, &src, end)) != 0)
                goto Exit;
            if (id < sizeof(found_id_bits) * 8) {
                if ((found_id_bits & ID_TO_BIT(id)) != 0) {
                    ret = QUICLY_ERROR_INVALID_STREAM_DATA; /* FIXME error code */
                    goto Exit;
                }
                found_id_bits |= ID_TO_BIT(id);
            }
            found_id_bits |= ID_TO_BIT(id);
            ptls_decode_open_block(src, end, 2, {
                switch (id) {
                case QUICLY_TRANSPORT_PARAMETER_ID_INITIAL_MAX_STREAM_DATA:
                    if ((ret = ptls_decode32(&params->initial_max_stream_data, &src, end)) != 0)
                        goto Exit;
                    break;
                case QUICLY_TRANSPORT_PARAMETER_ID_INITIAL_MAX_DATA:
                    if ((ret = ptls_decode32(&params->initial_max_data_kb, &src, end)) != 0)
                        goto Exit;
                    break;
                case QUICLY_TRANSPORT_PARAMETER_ID_INITIAL_MAX_STREAM_ID:
                    if ((ret = ptls_decode32(&params->initial_max_stream_id, &src, end)) != 0)
                        goto Exit;
                    break;
                case QUICLY_TRANSPORT_PARAMETER_ID_IDLE_TIMEOUT:
                    if ((ret = ptls_decode16(&params->idle_timeout, &src, end)) != 0)
                        goto Exit;
                    break;
                case QUICLY_TRANSPORT_PARAMETER_ID_TRUNCATE_CONNECTION_ID:
                    params->truncate_connection_id = 1;
                    break;
                default:
                    src = end;
                    break;
                }
            });
        }
    });

    /* check that we have found all the required parameters */
    if ((found_id_bits & must_found_id_bits) != must_found_id_bits) {
        ret = QUICLY_ERROR_INVALID_STREAM_DATA; /* FIXME error code */
        goto Exit;
    }

    ret = 0;
Exit:
    return ret;

#undef ID_TO_BIT
}

static int collect_transport_parameters(ptls_t *tls, struct st_ptls_handshake_properties_t *properties, uint16_t type)
{
    return type == QUICLY_TLS_EXTENSION_TYPE_TRANSPORT_PARAMETERS;
}

static quicly_conn_t *create_connection(quicly_context_t *ctx, const char *server_name, struct sockaddr *sa, socklen_t salen,
                                        ptls_handshake_properties_t *handshake_properties, quicly_stream_t **crypto_stream)
{
    quicly_conn_t *conn;
    struct st_quicly_crypto_stream_data_t *crypto_data;

    *crypto_stream = NULL;

    if ((conn = malloc(sizeof(*conn))) == NULL)
        return NULL;

    memset(conn, 0, sizeof(*conn));
    conn->super.ctx = ctx;
    conn->super.peer.transport_params = transport_params_before_handshake;
    conn->streams = kh_init(quicly_stream_t);
    quicly_ranges_init(&conn->ingress.ack_queue);
    quicly_maxsender_init(&conn->ingress.max_data.sender, conn->super.ctx->transport_params.initial_max_data_kb);
    quicly_acks_init(&conn->egress.acks);

    if (set_peeraddr(conn, sa, salen) != 0)
        goto Fail;

    /* instantiate the crypto stream */
    if ((*crypto_stream = open_stream(conn, 0)) == NULL)
        goto Fail;
    if ((crypto_data = malloc(sizeof(*crypto_data))) == NULL)
        goto Fail;
    (*crypto_stream)->data = crypto_data;
    (*crypto_stream)->on_update = crypto_stream_receive_handshake;
    crypto_data->conn = conn;
    if ((crypto_data->tls = ptls_new(ctx->tls, server_name == NULL)) == NULL)
        goto Fail;
    if (server_name != NULL && ptls_set_server_name(crypto_data->tls, server_name, strlen(server_name)) != 0)
        goto Fail;
    if (handshake_properties != NULL) {
        assert(handshake_properties->additional_extensions == NULL);
        assert(handshake_properties->collect_extension == NULL);
        assert(handshake_properties->collected_extensions == NULL);
        crypto_data->handshake_properties = *handshake_properties;
    } else {
        crypto_data->handshake_properties = (ptls_handshake_properties_t){{{{NULL}}}};
    }
    crypto_data->handshake_properties.collect_extension = collect_transport_parameters;
    if (server_name != NULL) {
        conn->super.host.next_stream_id = 1;
        conn->super.peer.next_stream_id = 2;
    } else {
        conn->super.host.next_stream_id = 2;
        conn->super.peer.next_stream_id = 1;
    }

    return conn;
Fail:
    if (conn != NULL)
        quicly_free(conn);
    return NULL;
}

static int client_collected_extensions(ptls_t *tls, ptls_handshake_properties_t *properties, ptls_raw_extension_t *slots)
{
    struct st_quicly_crypto_stream_data_t *crypto_data =
        (void *)((char *)properties - offsetof(struct st_quicly_crypto_stream_data_t, handshake_properties));
    int ret;

    if (slots[0].type == UINT16_MAX) {
        ret = 0; // allow abcense of the extension for the time being PTLS_ALERT_MISSING_EXTENSION;
        goto Exit;
    }
    assert(slots[0].type == QUICLY_TLS_EXTENSION_TYPE_TRANSPORT_PARAMETERS);
    assert(slots[1].type == UINT16_MAX);

    const uint8_t *src = slots[0].data.base, *end = src + slots[0].data.len;

    ptls_decode_open_block(src, end, 1, {
        int found_negotiated_version = 0;
        do {
            uint32_t supported_version;
            if ((ret = ptls_decode32(&supported_version, &src, end)) != 0)
                goto Exit;
            if (supported_version == QUICLY_PROTOCOL_VERSION)
                found_negotiated_version = 1;
        } while (src != end);
        if (!found_negotiated_version) {
            ret = PTLS_ALERT_ILLEGAL_PARAMETER; /* FIXME is this the correct error code? */
            goto Exit;
        }
    });
    ret = decode_transport_parameter_list(&crypto_data->conn->super.peer.transport_params, src, end);

Exit:
    return ret;
}

int quicly_connect(quicly_conn_t **_conn, quicly_context_t *ctx, const char *server_name, struct sockaddr *sa, socklen_t salen,
                   ptls_handshake_properties_t *handshake_properties)
{
    quicly_conn_t *conn;
    quicly_stream_t *crypto_stream;
    struct st_quicly_crypto_stream_data_t *crypto_data;
    ptls_buffer_t buf;
    int ret;

    if ((conn = create_connection(ctx, server_name, sa, salen, handshake_properties, &crypto_stream)) == NULL) {
        ret = PTLS_ERROR_NO_MEMORY;
        goto Exit;
    }
    crypto_data = crypto_stream->data;

    /* handshake */
    ptls_buffer_init(&crypto_data->transport_parameters.buf, "", 0);
    ptls_buffer_push32(&crypto_data->transport_parameters.buf, QUICLY_PROTOCOL_VERSION);
    ptls_buffer_push32(&crypto_data->transport_parameters.buf, QUICLY_PROTOCOL_VERSION);
    if ((ret = encode_transport_parameter_list(&ctx->transport_params, &crypto_data->transport_parameters.buf)) != 0)
        goto Exit;
    crypto_data->transport_parameters.ext[0] =
        (ptls_raw_extension_t){QUICLY_TLS_EXTENSION_TYPE_TRANSPORT_PARAMETERS,
                               {crypto_data->transport_parameters.buf.base, crypto_data->transport_parameters.buf.off}};
    crypto_data->transport_parameters.ext[1] = (ptls_raw_extension_t){UINT16_MAX};
    crypto_data->handshake_properties.additional_extensions = crypto_data->transport_parameters.ext;
    crypto_data->handshake_properties.collected_extensions = client_collected_extensions;

    ptls_buffer_init(&buf, "", 0);
    if ((ret = ptls_handshake(crypto_data->tls, &buf, NULL, 0, &crypto_data->handshake_properties)) != PTLS_ERROR_IN_PROGRESS)
        goto Exit;
    write_tlsbuf(crypto_stream, &buf);

    *_conn = conn;
    ret = 0;

Exit:
    if (ret != 0) {
        if (conn != NULL)
            quicly_free(conn);
    }
    return ret;
}

static int server_collected_extensions(ptls_t *tls, ptls_handshake_properties_t *properties, ptls_raw_extension_t *slots)
{
    struct st_quicly_crypto_stream_data_t *crypto_data =
        (void *)((char *)properties - offsetof(struct st_quicly_crypto_stream_data_t, handshake_properties));
    int ret;

    if (slots[0].type == UINT16_MAX) {
        ret = 0; // allow abcense of the extension for the time being PTLS_ALERT_MISSING_EXTENSION;
        goto Exit;
    }
    assert(slots[0].type == QUICLY_TLS_EXTENSION_TYPE_TRANSPORT_PARAMETERS);
    assert(slots[1].type == UINT16_MAX);

    { /* decode transport_parameters extension */
        const uint8_t *src = slots[0].data.base, *end = src + slots[0].data.len;
        uint32_t negotiated_version, initial_version;
        if ((ret = ptls_decode32(&negotiated_version, &src, end)) != 0)
            goto Exit;
        if ((ret = ptls_decode32(&initial_version, &src, end)) != 0)
            goto Exit;
        if (!(negotiated_version == QUICLY_PROTOCOL_VERSION && initial_version == QUICLY_PROTOCOL_VERSION)) {
            ret = QUICLY_ERROR_VERSION_NEGOTIATION_MISMATCH;
            goto Exit;
        }
        if ((ret = decode_transport_parameter_list(&crypto_data->conn->super.peer.transport_params, src, end)) != 0)
            goto Exit;
    }

    /* set transport_parameters extension to be sent in EE */
    assert(properties->additional_extensions == NULL);
    ptls_buffer_init(&crypto_data->transport_parameters.buf, "", 0);
    ptls_buffer_push_block(&crypto_data->transport_parameters.buf, 1,
                           { ptls_buffer_push32(&crypto_data->transport_parameters.buf, QUICLY_PROTOCOL_VERSION); });
    if ((ret = encode_transport_parameter_list(&crypto_data->conn->super.ctx->transport_params,
                                               &crypto_data->transport_parameters.buf)) != 0)
        goto Exit;
    properties->additional_extensions = crypto_data->transport_parameters.ext;
    crypto_data->transport_parameters.ext[0] =
        (ptls_raw_extension_t){QUICLY_TLS_EXTENSION_TYPE_TRANSPORT_PARAMETERS,
                               {crypto_data->transport_parameters.buf.base, crypto_data->transport_parameters.buf.off}};
    crypto_data->transport_parameters.ext[1] = (ptls_raw_extension_t){UINT16_MAX};
    crypto_data->handshake_properties.additional_extensions = crypto_data->transport_parameters.ext;

    ret = 0;

Exit:
    return ret;
}

int quicly_accept(quicly_conn_t **_conn, quicly_context_t *ctx, struct sockaddr *sa, socklen_t salen,
                  ptls_handshake_properties_t *handshake_properties, quicly_decoded_packet_t *packet)
{
    quicly_conn_t *conn = NULL;
    quicly_stream_t *crypto_stream;
    struct st_quicly_crypto_stream_data_t *crypto_data;
    quicly_stream_frame_t frame;
    int ret;

    /* ignore any packet that does not  */
    if (packet->type != QUICLY_PACKET_TYPE_CLIENT_INITIAL) {
        ret = QUICLY_ERROR_PACKET_IGNORED;
        goto Exit;
    }
    if (!verify_cleartext_packet(packet)) {
        ret = QUICLY_ERROR_DECRYPTION_FAILURE;
        goto Exit;
    }
    {
        const uint8_t *src = packet->payload.base, *end = src + packet->payload.len;
        uint8_t type_flags;
        for (; src < end; ++src) {
            if (*src != QUICLY_FRAME_TYPE_PADDING)
                break;
        }
        if (src == end || (type_flags = *src++) < QUICLY_FRAME_TYPE_STREAM) {
            ret = QUICLY_ERROR_TBD;
            goto Exit;
        }
        if ((ret = quicly_decode_stream_frame(type_flags, &src, end, &frame)) != 0)
            goto Exit;
        if (!(frame.stream_id == 0 && frame.offset == 0)) {
            ret = QUICLY_ERROR_INVALID_STREAM_DATA;
            goto Exit;
        }
        /* FIXME check packet size */
        for (; src < end; ++src) {
            if (*src != QUICLY_FRAME_TYPE_PADDING) {
                ret = QUICLY_ERROR_TBD;
                goto Exit;
            }
        }
    }

    if ((conn = create_connection(ctx, NULL, sa, salen, handshake_properties, &crypto_stream)) == NULL)
        return PTLS_ERROR_NO_MEMORY;
    crypto_data = crypto_stream->data;
    crypto_data->handshake_properties.collected_extensions = server_collected_extensions;

    if ((ret = quicly_ranges_update(&conn->ingress.ack_queue, packet->packet_number, packet->packet_number + 1)) != 0)
        goto Exit;

    if ((ret = apply_stream_frame(conn, crypto_stream, &frame)) != 0)
        goto Exit;
    if (crypto_stream->recvbuf.data_off != frame.data.len) {
        /* garbage after clienthello? */
        ret = QUICLY_ERROR_TBD;
        goto Exit;
    }

    *_conn = conn;

Exit:
    if (!(ret == 0 || ret == PTLS_ERROR_IN_PROGRESS)) {
        if (conn != NULL)
            quicly_free(conn);
    }
    return ret;
}

static void reset_sender(quicly_stream_t *stream, uint32_t reason)
{
    /* do nothing if we have sent all data (maybe we haven't sent FIN yet, but we can send it instead of an RST_STREAM) */
    if (stream->_send_aux.max_sent == stream->sendbuf.eos)
        return;

    /* close the sender and mark the eos as the only byte that's not confirmed */
    assert(!quicly_sendbuf_transfer_complete(&stream->sendbuf));
    quicly_sendbuf_shutdown(&stream->sendbuf);
    quicly_sendbuf_ackargs_t ackargs = {0, stream->sendbuf.eos};
    quicly_sendbuf_acked(&stream->sendbuf, &ackargs);

    /* setup the sender */
    stream->_send_aux.rst.sender_state = QUICLY_SENDER_STATE_SEND;
    stream->_send_aux.rst.reason = reason;
}

static int on_ack_stream(quicly_conn_t *conn, int acked, quicly_ack_t *ack)
{
    quicly_stream_t *stream;

    QUICLY_DEBUG_LOG(conn, ack->data.stream.stream_id, "%s; off=%" PRIu64 ",len=%zu", acked ? "acked" : "lost",
                     ack->data.stream.args.start, (size_t)(ack->data.stream.args.end - ack->data.stream.args.start));

    /* TODO cache pointer to stream (using a generation counter?) */
    if ((stream = quicly_get_stream(conn, ack->data.stream.stream_id)) == NULL)
        return 0;

    if (acked) {
        int ret;
        if ((ret = quicly_sendbuf_acked(&stream->sendbuf, &ack->data.stream.args)) != 0)
            return ret;
        destroy_stream_if_unneeded(stream);
        return 0;
    } else {
        /* FIXME handle rto error */
        return quicly_sendbuf_lost(&stream->sendbuf, &ack->data.stream.args);
    }
}

static int on_ack_max_stream_data(quicly_conn_t *conn, int acked, quicly_ack_t *ack)
{
    quicly_stream_t *stream;

    /* TODO cache pointer to stream (using a generation counter?) */
    if ((stream = quicly_get_stream(conn, ack->data.stream.stream_id)) != NULL) {
        if (acked) {
            quicly_maxsender_acked(&stream->_send_aux.max_stream_data_sender, &ack->data.max_stream_data.args);
        } else {
            quicly_maxsender_lost(&stream->_send_aux.max_stream_data_sender, &ack->data.max_stream_data.args);
        }
    }

    return 0;
}

static int on_ack_max_data(quicly_conn_t *conn, int acked, quicly_ack_t *ack)
{
    if (acked) {
        quicly_maxsender_acked(&conn->ingress.max_data.sender, &ack->data.max_data.args);
    } else {
        quicly_maxsender_lost(&conn->ingress.max_data.sender, &ack->data.max_data.args);
    }

    return 0;
}

static int on_ack_stream_state_sender(quicly_conn_t *conn, int acked, quicly_ack_t *ack)
{
    quicly_stream_t *stream;

    if ((stream = quicly_get_stream(conn, ack->data.stream_state_sender.stream_id)) == NULL)
        return 0;

    quicly_sender_state_t *sender_state = (void *)((char *)stream + ack->data.stream_state_sender.sender_state_offset);
    if (acked) {
        *sender_state = QUICLY_SENDER_STATE_ACKED;
        destroy_stream_if_unneeded(stream);
    } else {
        *sender_state = QUICLY_SENDER_STATE_SEND;
    }

    return 0;
}

struct st_quicly_send_context_t {
    uint8_t packet_type;
    ptls_aead_context_t *aead;
    int64_t now;
    quicly_raw_packet_t **packets;
    size_t max_packets;
    size_t num_packets;
    quicly_raw_packet_t *target;
    uint8_t *dst;
    uint8_t *dst_end;
    uint8_t *dst_unencrypted_from;
};

static inline void encrypt_packet(struct st_quicly_send_context_t *s)
{
    ptls_aead_encrypt_update(s->aead, s->dst_unencrypted_from, s->dst_unencrypted_from, s->dst - s->dst_unencrypted_from);
    s->dst_unencrypted_from = s->dst;
}

static void commit_send_packet(quicly_conn_t *conn, struct st_quicly_send_context_t *s)
{
    if (s->aead != NULL) {
        if (s->dst != s->dst_unencrypted_from)
            encrypt_packet(s);
        s->dst += ptls_aead_encrypt_final(s->aead, s->dst);
    } else {
        uint64_t hash = fnv1a(FNV1A_OFFSET_BASIS, s->target->data.base, s->dst);
        s->dst = quicly_encode64(s->dst, hash);
    }
    s->target->data.len = s->dst - s->target->data.base;
    s->packets[s->num_packets++] = s->target;
    ++conn->egress.packet_number;

    s->target = NULL;
    s->dst = NULL;
    s->dst_end = NULL;
    s->dst_unencrypted_from = NULL;
}

static int prepare_packet(quicly_conn_t *conn, struct st_quicly_send_context_t *s, size_t min_space)
{
    /* allocate and setup the new packet if necessary */
    if (s->dst_end - s->dst < min_space || GET_TYPE_FROM_PACKET_HEADER(s->target->data.base) != s->packet_type) {
        if (s->target != NULL) {
            while (s->dst != s->dst_end)
                *s->dst++ = QUICLY_FRAME_TYPE_PADDING;
            commit_send_packet(conn, s);
        }
        if (s->num_packets >= s->max_packets)
            return 0;
        if ((s->target =
                 conn->super.ctx->alloc_packet(conn->super.ctx, conn->super.peer.salen, conn->super.ctx->max_packet_size)) == NULL)
            return PTLS_ERROR_NO_MEMORY;
        s->target->salen = conn->super.peer.salen;
        memcpy(&s->target->sa, conn->super.peer.sa, conn->super.peer.salen);
        s->dst = s->target->data.base;
        s->dst_end = s->target->data.base + conn->super.ctx->max_packet_size;
        s->dst = emit_long_header(conn, s->dst, s->packet_type, conn->super.connection_id, (uint32_t)conn->egress.packet_number);
        s->dst_unencrypted_from = s->dst;
        if (s->aead != NULL) {
            s->dst_end -= s->aead->algo->tag_size;
            ptls_aead_encrypt_init(s->aead, conn->egress.packet_number, s->target->data.base, s->dst - s->target->data.base);
        } else {
            s->dst_end -= 8; /* space for fnv1a-64 */
        }
        assert(s->dst < s->dst_end);
    }

    return 0;
}

static int send_ack(quicly_conn_t *conn, struct st_quicly_send_context_t *s)
{
    quicly_ack_frame_encode_params_t encode_params;
    size_t range_index;
    int ret;

    if (conn->ingress.ack_queue.num_ranges == 0)
        return 0;

    quicly_determine_encode_ack_frame_params(&conn->ingress.ack_queue, &encode_params);

    range_index = conn->ingress.ack_queue.num_ranges - 1;
    do {
        if ((ret = prepare_packet(conn, s, quicly_ack_frame_get_minimum_capacity(&encode_params, range_index))) != 0)
            break;
        if (s->dst == NULL)
            break;
        s->dst = quicly_encode_ack_frame(s->dst, s->dst_end, &conn->ingress.ack_queue, &range_index, &encode_params);
    } while (range_index != SIZE_MAX);

    quicly_ranges_clear(&conn->ingress.ack_queue);
    return ret;
}

static int send_max_data_frame(quicly_conn_t *conn, struct st_quicly_send_context_t *s)
{
    quicly_ack_t *ack;
    uint64_t new_value;
    int ret;

    /* parepare */
    if ((ret = prepare_packet(conn, s, QUICLY_MAX_DATA_FRAME_SIZE)) != 0)
        return ret;
    if (s->dst == NULL)
        return 0;
    if ((ack = quicly_acks_allocate(&conn->egress.acks, conn->egress.packet_number, s->now, on_ack_max_data)) == NULL)
        return PTLS_ERROR_NO_MEMORY;

    /* calculate new value */
    new_value = (uint64_t)(conn->ingress.max_data.bytes_consumed / 1024) + conn->super.ctx->transport_params.initial_max_data_kb;

    /* send */
    s->dst = quicly_encode_max_data_frame(s->dst, new_value);

    /* register ack */
    quicly_maxsender_record(&conn->ingress.max_data.sender, new_value, &ack->data.max_data.args);

    return 0;
}

static int send_max_stream_data_frame(quicly_stream_t *stream, struct st_quicly_send_context_t *s)
{
    quicly_ack_t *ack;
    uint64_t new_value;
    int ret;

    /* prepare */
    if ((ret = prepare_packet(stream->conn, s, QUICLY_MAX_STREAM_DATA_FRAME_SIZE)) != 0)
        return ret;
    if (s->dst == NULL)
        return 0;
    if ((ack = quicly_acks_allocate(&stream->conn->egress.acks, stream->conn->egress.packet_number, s->now,
                                    on_ack_max_stream_data)) == NULL)
        return PTLS_ERROR_NO_MEMORY;

    /* calculate new value */
    new_value = stream->recvbuf.data_off + stream->_recv_aux.window;

    /* send */
    s->dst = quicly_encode_max_stream_data_frame(s->dst, stream->stream_id, new_value);

    /* register ack */
    ack->data.max_stream_data.stream_id = stream->stream_id;
    quicly_maxsender_record(&stream->_send_aux.max_stream_data_sender, new_value, &ack->data.max_stream_data.args);

    return 0;

#undef FRAME_SIZE
}

static int send_stream_frame(quicly_stream_t *stream, struct st_quicly_send_context_t *s, quicly_sendbuf_dataiter_t *iter,
                             size_t max_bytes)
{
    size_t stream_id_length, offset_length;
    int ret;

    quicly_determine_stream_frame_field_lengths(stream->stream_id, iter->stream_off, &stream_id_length, &offset_length);

    if ((ret = prepare_packet(stream->conn, s, 1 + stream_id_length + offset_length + (iter->stream_off != stream->sendbuf.eos))) !=
        0)
        return ret;
    if (s->dst == NULL)
        return 0;

    size_t capacity = s->dst_end - s->dst - (1 + stream_id_length + offset_length);
    size_t avail = max_bytes - (iter->stream_off + max_bytes > stream->sendbuf.eos);
    size_t copysize = capacity <= avail ? capacity : avail;

    s->dst = quicly_encode_stream_frame_header(s->dst, iter->stream_off + copysize >= stream->sendbuf.eos, stream->stream_id,
                                               stream_id_length, iter->stream_off, offset_length,
                                               copysize + 2 < capacity ? copysize : SIZE_MAX);

    if (s->aead != NULL)
        encrypt_packet(s);

    QUICLY_DEBUG_LOG(stream->conn, stream->stream_id, "sending; off=%" PRIu64 ",len=%zu", iter->stream_off, copysize);

    /* prepare ack storage */
    quicly_ack_t *ack;
    if ((ack = quicly_acks_allocate(&stream->conn->egress.acks, stream->conn->egress.packet_number, s->now, on_ack_stream)) == NULL)
        return PTLS_ERROR_NO_MEMORY;
    ack->data.stream.stream_id = stream->stream_id;

    /* adjust remaining send window */
    if (stream->_send_aux.max_sent < iter->stream_off + copysize) {
        if (stream->stream_id != 0) {
            uint64_t delta = iter->stream_off + copysize - stream->_send_aux.max_sent;
            assert(stream->conn->egress.max_data.sent + delta <= stream->conn->egress.max_data.permitted);
            stream->conn->egress.max_data.sent += delta;
        }
        stream->_send_aux.max_sent = iter->stream_off + copysize;
    }

    /* send */
    quicly_sendbuf_emit(&stream->sendbuf, iter, copysize, s->dst, &ack->data.stream.args, s->aead);
    s->dst += copysize;
    s->dst_unencrypted_from = s->dst;

    return 0;
}

static int send_stream_frames(quicly_stream_t *stream, struct st_quicly_send_context_t *s)
{
    quicly_sendbuf_dataiter_t iter;
    uint64_t max_stream_data;
    size_t i;

    if (stream->_send_aux.max_sent + 1 >= stream->sendbuf.eos) {
        max_stream_data = stream->sendbuf.eos + 1;
    } else {
        uint64_t delta = stream->_send_aux.max_stream_data - stream->_send_aux.max_sent;
        if (stream->stream_id != 0 && stream->conn->egress.max_data.permitted - stream->conn->egress.max_data.sent < delta)
            delta = (uint64_t)(stream->conn->egress.max_data.permitted - stream->conn->egress.max_data.sent);
        max_stream_data = stream->_send_aux.max_sent + delta;
        if (max_stream_data == stream->sendbuf.eos)
            ++max_stream_data;
    }

    quicly_sendbuf_init_dataiter(&stream->sendbuf, &iter);

    for (i = 0; i != stream->sendbuf.pending.num_ranges; ++i) {
        uint64_t start = stream->sendbuf.pending.ranges[i].start, end = stream->sendbuf.pending.ranges[i].end;
        if (max_stream_data <= start)
            goto ShrinkRanges;
        if (max_stream_data < end)
            end = max_stream_data;

        if (iter.stream_off != start) {
            assert(iter.stream_off <= start);
            quicly_sendbuf_advance_dataiter(&iter, start - iter.stream_off);
        }
        /* when end == eos, iter.stream_off becomes end+1 after calling send_steram_frame; hence `<` is used */
        while (iter.stream_off < end) {
            int ret = send_stream_frame(stream, s, &iter, end - iter.stream_off);
            if (ret != 0)
                return ret;
            if (s->dst == NULL)
                goto ShrinkToIter;
        }

        if (iter.stream_off < stream->sendbuf.pending.ranges[i].end)
            goto ShrinkToIter;
    }

    quicly_ranges_clear(&stream->sendbuf.pending);
    return 0;

ShrinkToIter:
    stream->sendbuf.pending.ranges[i].start = iter.stream_off;
ShrinkRanges:
    quicly_ranges_shrink(&stream->sendbuf.pending, 0, i);
    return 0;
}

static int prepare_stream_state_sender(quicly_stream_t *stream, quicly_sender_state_t *sender, struct st_quicly_send_context_t *s,
                                       size_t min_space)
{
    quicly_ack_t *ack;
    int ret;

    if ((ret = prepare_packet(stream->conn, s, QUICLY_STOP_SENDING_FRAME_SIZE)) != 0 || s->dst == NULL)
        return ret;

    if ((ack = quicly_acks_allocate(&stream->conn->egress.acks, stream->conn->egress.packet_number, s->now,
                                    on_ack_stream_state_sender)) == NULL)
        return PTLS_ERROR_NO_MEMORY;
    ack->data.stream_state_sender.stream_id = stream->stream_id;
    ack->data.stream_state_sender.sender_state_offset = (char *)sender - (char *)stream;
    *sender = QUICLY_SENDER_STATE_UNACKED;

    return 0;
}

static int send_stream(quicly_stream_t *stream, struct st_quicly_send_context_t *s)
{
    int ret = 0;

    if (destroy_stream_if_unneeded(stream))
        return 0;

    /* send STOP_SENDING if necessray */
    if (stream->_send_aux.stop_sending.sender_state == QUICLY_SENDER_STATE_SEND) {
        if ((ret = prepare_stream_state_sender(stream, &stream->_send_aux.stop_sending.sender_state, s,
                                               QUICLY_STOP_SENDING_FRAME_SIZE)) != 0 ||
            s->dst == NULL)
            return ret;
        s->dst = quicly_encode_stop_sending_frame(s->dst, stream->stream_id, stream->_send_aux.stop_sending.reason);
    }

    /* send RST_STREAM if necessary */
    if (stream->_send_aux.rst.sender_state == QUICLY_SENDER_STATE_SEND) {
        if ((ret = prepare_stream_state_sender(stream, &stream->_send_aux.rst.sender_state, s, QUICLY_RST_FRAME_SIZE)) != 0 ||
            s->dst == NULL)
            return ret;
        s->dst =
            quicly_encode_rst_stream_frame(s->dst, stream->stream_id, stream->_send_aux.rst.reason, stream->_send_aux.max_sent);
        return 0;
    }

    /* send MAX_STREAM_DATA if necessary */
    if (quicly_maxsender_should_update(&stream->_send_aux.max_stream_data_sender, stream->recvbuf.data_off,
                                       stream->_recv_aux.window, 512)) {
        if ((ret = send_max_stream_data_frame(stream, s)) != 0)
            return ret;
    }

    return send_stream_frames(stream, s);
}

static int handle_timeouts(quicly_conn_t *conn, int64_t now)
{
    quicly_acks_iter_t iter;
    quicly_ack_t *ack;
    int ret;
    int64_t sent_before = now - conn->super.ctx->initial_rto;

    for (quicly_acks_init_iter(&conn->egress.acks, &iter); (ack = quicly_acks_get(&iter)) != NULL; quicly_acks_next(&iter)) {
        if (sent_before < ack->sent_at)
            break;
        if ((ret = ack->acked(conn, 0, ack)) != 0)
            return ret;
        quicly_acks_release(&conn->egress.acks, &iter);
    }

    return 0;
}

int quicly_send(quicly_conn_t *conn, quicly_raw_packet_t **packets, size_t *num_packets)
{
    quicly_stream_t *stream;
    struct st_quicly_send_context_t s = {UINT8_MAX, NULL, conn->super.ctx->now(conn->super.ctx), packets, *num_packets};
    int ret;

    /* handle timeouts */
    if ((ret = handle_timeouts(conn, s.now)) != 0)
        goto Exit;

    /* send cleartext frames */
    if (quicly_is_client(conn)) {
        if (quicly_get_state(conn) == QUICLY_STATE_BEFORE_SH) {
            s.packet_type = QUICLY_PACKET_TYPE_CLIENT_INITIAL;
        } else {
            s.packet_type = QUICLY_PACKET_TYPE_CLIENT_CLEARTEXT;
        }
    } else {
        s.packet_type = QUICLY_PACKET_TYPE_SERVER_CLEARTEXT;
    }
    s.aead = NULL;
    if (!conn->egress.acks_require_encryption && s.packet_type != QUICLY_PACKET_TYPE_CLIENT_INITIAL) {
        if ((ret = send_ack(conn, &s)) != 0)
            goto Exit;
    }
    if ((ret = send_stream(quicly_get_stream(conn, 0), &s)) != 0)
        goto Exit;
    if (s.target != NULL) {
        if (s.packet_type == QUICLY_PACKET_TYPE_CLIENT_INITIAL) {
            if (s.num_packets != 0)
                return QUICLY_ERROR_HANDSHAKE_TOO_LARGE;
            const size_t max_size = 1272; /* max UDP packet size excluding fnv1a hash */
            assert(s.dst - s.target->data.base <= max_size);
            memset(s.dst, 0, s.target->data.base + max_size - s.dst);
            s.dst = s.target->data.base + max_size;
        }
        commit_send_packet(conn, &s);
    }

    /* send encrypted frames */
    if (quicly_get_state(conn) == QUICLY_STATE_1RTT_ENCRYPTED) {
        s.packet_type = QUICLY_PACKET_TYPE_1RTT_KEY_PHASE_0;
        s.aead = conn->egress.pp.aead.key_phase0;
        if ((ret = send_ack(conn, &s)) != 0)
            goto Exit;
        if (quicly_maxsender_should_update(&conn->ingress.max_data.sender, (uint64_t)(conn->ingress.max_data.bytes_consumed / 1024),
                                           conn->super.ctx->transport_params.initial_max_data_kb, 512)) {
            if ((ret = send_max_data_frame(conn, &s)) != 0)
                goto Exit;
        }
        kh_foreach_value(conn->streams, stream, {
            if (stream->stream_id != 0) {
                if ((ret = send_stream(stream, &s)) != 0)
                    goto Exit;
            }
        });
        if (s.target != NULL)
            commit_send_packet(conn, &s);
    }

    *num_packets = s.num_packets;
    ret = 0;
Exit:
    return ret;
}

static int get_stream_or_open_if_new(quicly_conn_t *conn, uint32_t stream_id, quicly_stream_t **stream)
{
    int ret = 0;

    if ((*stream = quicly_get_stream(conn, stream_id)) != NULL)
        goto Exit;

    if (stream_id % 2 != quicly_is_client(conn) && conn->super.peer.next_stream_id != 0 &&
        conn->super.peer.next_stream_id <= stream_id) {
        /* open new streams upto given id */
        do {
            if ((*stream = open_stream(conn, conn->super.peer.next_stream_id)) == NULL) {
                ret = PTLS_ERROR_NO_MEMORY;
                goto Exit;
            }
            if ((ret = conn->super.ctx->on_stream_open(*stream)) != 0) {
                *stream = NULL;
                goto Exit;
            }
            ++conn->super.peer.num_streams;
            conn->super.peer.next_stream_id += 2;
        } while (stream_id != (*stream)->stream_id);
        /* disallow opening new streams if the number has overlapped */
        if (conn->super.peer.next_stream_id < 2)
            conn->super.peer.next_stream_id = 0;
    }

Exit:
    return ret;
}

static int handle_stream_frame(quicly_conn_t *conn, quicly_stream_frame_t *frame)
{
    quicly_stream_t *stream;
    int ret;

    if ((ret = get_stream_or_open_if_new(conn, frame->stream_id, &stream)) != 0 || stream == NULL)
        return ret;
    return apply_stream_frame(conn, stream, frame);
}

static int handle_rst_stream_frame(quicly_conn_t *conn, quicly_rst_stream_frame_t *frame)
{
    quicly_stream_t *stream;
    int ret;

    if ((ret = get_stream_or_open_if_new(conn, frame->stream_id, &stream)) != 0 || stream == NULL)
        return ret;

    if (stream->recvbuf.eos == UINT64_MAX) {
        if (frame->final_offset < stream->recvbuf.received.ranges[stream->recvbuf.received.num_ranges - 1].end)
            return QUICLY_ERROR_TBD;
        quicly_recvbuf_mark_eos(&stream->recvbuf, frame->final_offset);
        stream->_recv_aux.rst_reason = frame->reason;
        if ((ret = stream->on_update(stream)) != 0)
            return ret;
    } else {
        if (frame->final_offset != stream->recvbuf.received.ranges[stream->recvbuf.received.num_ranges - 1].end)
            return QUICLY_ERROR_TBD;
    }

    destroy_stream_if_unneeded(stream);
    return 0;
}

static int handle_ack_frame(quicly_conn_t *conn, quicly_ack_frame_t *frame)
{
    quicly_acks_iter_t iter;
    uint64_t packet_number = frame->smallest_acknowledged;
    int ret;

    quicly_acks_init_iter(&conn->egress.acks, &iter);
    if (quicly_acks_get(&iter) == NULL)
        return 0;

    size_t gap_index = frame->num_gaps;
    while (1) {
        uint64_t block_length = frame->ack_block_lengths[gap_index];
        if (block_length != 0) {
            while (quicly_acks_get(&iter)->packet_number < packet_number) {
                quicly_acks_next(&iter);
                if (quicly_acks_get(&iter) == NULL)
                    goto Exit;
            }
            do {
                int found_active = 0;
                while (quicly_acks_get(&iter)->packet_number == packet_number) {
                    found_active = 1;
                    quicly_ack_t *ack = quicly_acks_get(&iter);
                    if ((ret = ack->acked(conn, 1, ack)) != 0)
                        return ret;
                    quicly_acks_release(&conn->egress.acks, &iter);
                    quicly_acks_next(&iter);
                    if (quicly_acks_get(&iter) == NULL)
                        break;
                }
                if (!found_active)
                    QUICLY_DEBUG_LOG(conn, 0, "dupack");
                if (quicly_acks_get(&iter) == NULL)
                    goto Exit;
            } while (++packet_number, --block_length != 0);
        }
        if (gap_index-- == 0)
            break;
        packet_number += frame->gaps[gap_index];
    }

Exit:
    return 0;
}

static int handle_max_stream_data_frame(quicly_conn_t *conn, quicly_max_stream_data_frame_t *frame)
{
    quicly_stream_t *stream = quicly_get_stream(conn, frame->stream_id);

    if (stream == NULL)
        return 0;

    if (frame->max_stream_data < stream->_send_aux.max_stream_data)
        return QUICLY_ERROR_TBD; /* FLOW_CONTROL_ERROR */
    stream->_send_aux.max_stream_data = frame->max_stream_data;

    /* TODO schedule for delivery */
    return 0;
}

static int handle_stop_sending_frame(quicly_conn_t *conn, quicly_stop_sending_frame_t *frame)
{
    quicly_stream_t *stream;
    int ret;

    if ((ret = get_stream_or_open_if_new(conn, frame->stream_id, &stream)) != 0 || stream == NULL)
        return ret;

    reset_sender(stream, QUICLY_ERROR_TBD);
    return 0;
}

static int handle_max_data_frame(quicly_conn_t *conn, quicly_max_data_frame_t *frame)
{
    __uint128_t new_value = (__uint128_t)frame->max_data_kb * 1024;

    if (new_value < conn->egress.max_data.permitted)
        return QUICLY_ERROR_TBD; /* FLOW_CONTROL_ERROR */
    conn->egress.max_data.permitted = new_value;

    /* TODO schedule for delivery */
    return 0;
}

int quicly_receive(quicly_conn_t *conn, quicly_decoded_packet_t *packet)
{
    ptls_aead_context_t *aead = NULL;
    int ret;

    /* FIXME check peer address */
    conn->super.connection_id = packet->connection_id;

    /* ignore packets having wrong connection id */
    if (packet->connection_id != conn->super.connection_id) {
        ret = QUICLY_ERROR_PACKET_IGNORED;
        goto Exit;
    }

    if (!packet->is_long_header && conn->super.state != QUICLY_STATE_1RTT_ENCRYPTED) {
        ret = QUICLY_ERROR_INVALID_PACKET_HEADER;
        goto Exit;
    }

    switch (packet->type) {
    case QUICLY_PACKET_TYPE_CLIENT_CLEARTEXT:
        if (quicly_is_client(conn)) {
            ret = QUICLY_ERROR_INVALID_PACKET_HEADER;
            goto Exit;
        }
        break;
    case QUICLY_PACKET_TYPE_SERVER_CLEARTEXT:
        if (!quicly_is_client(conn)) {
            ret = QUICLY_ERROR_INVALID_PACKET_HEADER;
            goto Exit;
        }
        break;
    case QUICLY_PACKET_TYPE_0RTT_PROTECTED:
        if (quicly_is_client(conn) || (aead = conn->ingress.pp.aead.early_data) == NULL) {
            ret = QUICLY_ERROR_INVALID_PACKET_HEADER;
            goto Exit;
        }
        break;
    case QUICLY_PACKET_TYPE_1RTT_KEY_PHASE_0:
        if ((aead = conn->ingress.pp.aead.key_phase0) == NULL) {
            /* drop 1rtt-encrypted packets received prior to handshake completion (due to loss of the packet carrying the
             * latter) */
            ret = quicly_get_state(conn) == QUICLY_STATE_1RTT_ENCRYPTED ? QUICLY_ERROR_INVALID_PACKET_HEADER : 0;
            goto Exit;
        }
        break;
    case QUICLY_PACKET_TYPE_1RTT_KEY_PHASE_1:
        if ((aead = conn->ingress.pp.aead.key_phase1) == NULL) {
            ret = QUICLY_ERROR_INVALID_PACKET_HEADER;
            goto Exit;
        }
        break;
    case QUICLY_PACKET_TYPE_CLIENT_INITIAL:
        /* FIXME ignore for time being */
        ret = 0;
        goto Exit;
    default:
        ret = QUICLY_ERROR_INVALID_PACKET_HEADER;
        goto Exit;
    }

    if (aead != NULL) {
        if ((packet->payload.len = ptls_aead_decrypt(aead, packet->payload.base, packet->payload.base, packet->payload.len,
                                                     packet->packet_number, packet->header.base, packet->header.len)) == SIZE_MAX) {
            ret = QUICLY_ERROR_DECRYPTION_FAILURE;
            goto Exit;
        }
    } else {
        if (!verify_cleartext_packet(packet)) {
            ret = QUICLY_ERROR_DECRYPTION_FAILURE;
            goto Exit;
        }
    }

    if (packet->payload.len == 0) {
        ret = QUICLY_ERROR_INVALID_FRAME_DATA;
        goto Exit;
    }

    const uint8_t *src = packet->payload.base, *end = src + packet->payload.len;
    int should_ack = 0;
    do {
        uint8_t type_flags = *src++;
        if (type_flags >= QUICLY_FRAME_TYPE_STREAM) {
            quicly_stream_frame_t frame;
            if ((ret = quicly_decode_stream_frame(type_flags, &src, end, &frame)) != 0)
                goto Exit;
            if ((ret = handle_stream_frame(conn, &frame)) != 0)
                goto Exit;
            should_ack = 1;
        } else if (type_flags >= QUICLY_FRAME_TYPE_ACK) {
            quicly_ack_frame_t frame;
            if ((ret = quicly_decode_ack_frame(type_flags, &src, end, &frame)) != 0)
                goto Exit;
            if ((ret = handle_ack_frame(conn, &frame)) != 0)
                goto Exit;
        } else {
            switch (type_flags) {
            case QUICLY_FRAME_TYPE_PADDING:
                ret = 0;
                break;
            case QUICLY_FRAME_TYPE_RST_STREAM: {
                quicly_rst_stream_frame_t frame;
                if ((ret = quicly_decode_rst_stream_frame(&src, end, &frame)) != 0)
                    goto Exit;
                if ((ret = handle_rst_stream_frame(conn, &frame)) != 0)
                    goto Exit;
            } break;
            case QUICLY_FRAME_TYPE_MAX_DATA: {
                quicly_max_data_frame_t frame;
                if ((ret = quicly_decode_max_data_frame(&src, end, &frame)) != 0)
                    goto Exit;
                if ((ret = handle_max_data_frame(conn, &frame)) != 0)
                    goto Exit;
            } break;
            case QUICLY_FRAME_TYPE_MAX_STREAM_DATA: {
                quicly_max_stream_data_frame_t frame;
                if ((ret = quicly_decode_max_stream_data_frame(&src, end, &frame)) != 0)
                    goto Exit;
                if ((ret = handle_max_stream_data_frame(conn, &frame)) != 0)
                    goto Exit;
            } break;
            case QUICLY_FRAME_TYPE_STOP_SENDING: {
                quicly_stop_sending_frame_t frame;
                if ((ret = quicly_decode_stop_sending_frame(&src, end, &frame)) != 0)
                    goto Exit;
                if ((ret = handle_stop_sending_frame(conn, &frame)) != 0)
                    goto Exit;
            } break;
            default:
                assert(!"FIXME");
                break;
            }
            should_ack = 1;
        }
    } while (src != end);
    if (should_ack) {
        if ((ret = quicly_ranges_update(&conn->ingress.ack_queue, packet->packet_number, packet->packet_number + 1)) != 0)
            goto Exit;
        if (aead != NULL)
            conn->egress.acks_require_encryption = 1;
    }

Exit:
    return ret;
}

int quicly_open_stream(quicly_conn_t *conn, quicly_stream_t **stream)
{
    if (conn->super.host.next_stream_id == 0)
        return QUICLY_ERROR_TOO_MANY_OPEN_STREAMS;

    if ((*stream = open_stream(conn, conn->super.host.next_stream_id)) == NULL)
        return PTLS_ERROR_NO_MEMORY;

    ++conn->super.host.num_streams;
    if ((conn->super.host.next_stream_id += 2) < 2)
        conn->super.host.next_stream_id = 0;

    return 0;
}

int quicly_close_stream(quicly_stream_t *stream)
{
    assert(!stream->_close_called);
    stream->_close_called = 1;

    if (stream->sendbuf.eos == UINT64_MAX)
        quicly_sendbuf_shutdown(&stream->sendbuf);
    if (stream->recvbuf.eos == UINT64_MAX) {
        stream->_send_aux.stop_sending.sender_state = QUICLY_SENDER_STATE_SEND;
        stream->_send_aux.stop_sending.reason = 0; /* FIXME */
    }

    destroy_stream_if_unneeded(stream);
    return 0;
}

quicly_raw_packet_t *quicly_default_alloc_packet(quicly_context_t *ctx, socklen_t salen, size_t payloadsize)
{
    quicly_raw_packet_t *packet;

    if ((packet = malloc(offsetof(quicly_raw_packet_t, sa) + salen + payloadsize)) == NULL)
        return NULL;
    packet->salen = salen;
    packet->data.base = (uint8_t *)packet + offsetof(quicly_raw_packet_t, sa) + salen;

    return packet;
}

void quicly_default_free_packet(quicly_context_t *ctx, quicly_raw_packet_t *packet)
{
    free(packet);
}