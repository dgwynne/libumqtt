/*
 * MIT License
 *
 * Copyright (c) 2019 Jianhui Zhao <zhaojh329@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <sys/socket.h>
#include <sys/time.h>

#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <fcntl.h>

#include "utils.h"
#include "umqtt.h"

static inline uint64_t
TIMESPEC_TO_NSEC(const struct timespec *ts)
{
	if (ts->tv_sec > (UINT64_MAX - ts->tv_nsec) / 1000000000ULL)
		return UINT64_MAX;
	return ts->tv_sec * 1000000000ULL + ts->tv_nsec;
}

static int64_t
clock_now(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) == -1)
		abort(); /* XXX */

	return (TIMESPEC_TO_NSEC(&now));
}

static const char *
umqtt_packet_type_to_string(int type)
{
	switch (type) {
	case UMQTT_CONNECT:
		return ("CONNECT");
	case UMQTT_CONNACK:
		return ("CONNACK");
	case UMQTT_PUBLISH:
		return ("PUBLISH");
	case UMQTT_PUBACK:
		return ("PUBACK");
	case UMQTT_PUBREC:
		return ("PUBREC");
	case UMQTT_PUBREL:
		return ("PUBREL");
	case UMQTT_PUBCOMP:
		return ("PUBCOMP");
	case UMQTT_SUBSCRIBE:
		return ("SUBSCRIBE");
	case UMQTT_SUBACK:
		return ("SUBACK");
	case UMQTT_UNSUBSCRIBE:
		return ("UNSUBSCRIBE");
	case UMQTT_UNSUBACK:
		return ("UNSUBACK");
	case UMQTT_PINGREQ:
		return ("PINGREQ");
	case UMQTT_PINGRESP:
		return ("PINGRESP");
	case UMQTT_DISCONNECT:
		return ("DISCONNECT");
	default:
		return ("(Unknown packet type)");
	}
}

static void
umqtt_free(struct umqtt_client *cl)
{
	event_del(&cl->ior);
	event_del(&cl->iow);
	buffer_free(&cl->rb);
	buffer_free(&cl->wb);

	if (cl->sock > 0)
	close(cl->sock);

	evtimer_del(&cl->timer);
}

static inline void
umqtt_error(struct umqtt_client *cl, int err, const char *msg)
{
	umqtt_free(cl);

	if (cl->on_error) {
		if (!msg)
			msg = "";
		cl->on_error(cl, err, msg);
	}
}

static inline void
umqtt_put_string(struct buffer *b, const char *str)
{
	size_t len = strlen(str);
	buffer_put_u16(b, htons(len));
	buffer_put_data(b, str, len);
}

static void
free_mid(struct umqtt_client *cl, uint16_t mid)
{
	cl->mid[mid] = false;
}

static void
store_mid(struct umqtt_client *cl, uint16_t mid)
{
	cl->mid[mid] = true;
}

static uint16_t get_unused_mid(struct umqtt_client *cl)
{
	int i;

	for (i = 1; i < sizeof(cl->mid); i++) {
		if (!cl->mid[i]) {
			cl->mid[i] = true;
			return i;
		}
	}

	/* Not found */
	return 0;
}

static bool
is_pending_message(struct umqtt_client *cl, uint16_t mid)
{
	return (cl->mid[mid]);
}

static void
send_pub(struct umqtt_client *cl, uint8_t type, uint8_t flags, uint16_t mid)
{
	struct buffer *wb = &cl->wb;

	buffer_put_u8(wb, (type << 4) | flags);
	buffer_put_u8(wb, 0x02);
	buffer_put_u16(wb, htons(mid));

	event_add(&cl->iow, NULL);

	umqtt_log_debug("send %s: mid(%d)\n",
	    umqtt_packet_type_to_string(type), mid);
}

static inline void
send_puback(struct umqtt_client *cl, uint16_t mid)
{
	send_pub(cl, UMQTT_PUBACK, 0, mid);
}

static inline void
send_pubrec(struct umqtt_client *cl, uint16_t mid)
{
	send_pub(cl, UMQTT_PUBREC, 0, mid);
}

static inline void
send_pubrel(struct umqtt_client *cl, uint16_t mid)
{
	send_pub(cl, UMQTT_PUBREL, 0x02, mid);
}

static inline void
send_pubcomp(struct umqtt_client *cl, uint16_t mid)
{
	send_pub(cl, UMQTT_PUBCOMP, 0, mid);
}

static void
handle_conack(struct umqtt_client *cl)
{
	struct buffer *rb = &cl->rb;
	bool sp = buffer_get_u8(rb, 0) & 0x01;    /* Session Present */
	int return_code =  buffer_get_u8(rb, 1);

	umqtt_log_debug("Received CONACK: sp(%d), return_code(%d)\n",
	    sp, return_code);

	if (cl->on_conack)
		cl->on_conack(cl, sp, return_code);

	cl->connection_accepted = true;
}

static void
handle_pubackcomp(struct umqtt_client *cl)
{
	uint16_t mid = ntohs(buffer_get_u16(&cl->rb, 0));

	umqtt_log_debug("Received PUBACK: mid(%d)\n", mid);

	if (is_pending_message(cl, mid))
		free_mid(cl, mid);
}

static void
handle_pubrec(struct umqtt_client *cl)
{
	uint16_t mid = ntohs(buffer_get_u16(&cl->rb, 0));

	umqtt_log_debug("Received PUBREC: mid(%d)\n", mid);

	send_pubrel(cl, mid);
}

static void
handle_pubrel(struct umqtt_client *cl)
{
	uint16_t mid = ntohs(buffer_get_u16(&cl->rb, 0));

	umqtt_log_debug("Received PUBREL: mid(%d)\n", mid);

	send_pubcomp(cl, mid);

	if (is_pending_message(cl, mid))
		free_mid(cl, mid);
}

static void
handle_suback(struct umqtt_client *cl)
{
	struct umqtt_packet *pkt = &cl->pkt;
	struct buffer *rb = &cl->rb;
	uint16_t mid = ntohs(buffer_get_u16(rb, 0));

	if (!is_pending_message(cl, mid)) {
		umqtt_log_err("Unknown 'suback' message "
		    "due to unknown message id\n");
		return;
	}

	umqtt_log_debug("Received SUBACK: mid(%d)\n", mid);

	free_mid(cl, mid);

	if (cl->on_suback)        
		cl->on_suback(cl, buffer_data(rb) + 2, pkt->remlen - 2);
}

static void
handle_unsuback(struct umqtt_client *cl)
{
	uint16_t mid = ntohs(buffer_get_u16(&cl->rb, 0));

	if (!is_pending_message(cl, mid)) {
		umqtt_log_err("Unknown 'unsuback' message "
		    "due to unknown message id\n");
		return;
	}

	umqtt_log_debug("Received UNSUBACK: mid(%d)\n", mid);

	free_mid(cl, mid);

	if (cl->on_unsuback)
		cl->on_unsuback(cl);
}

static void
handle_publish(struct umqtt_client *cl)
{
	struct umqtt_packet *pkt = &cl->pkt;
	uint8_t qos = (pkt->flags > 1) & 0x03;
	bool dup = (pkt->flags > 3) & 0x1;
	struct buffer *rb = &cl->rb;
	uint16_t mid = 0;
	int payloadlen;
	uint8_t *payload;
	const char *topic;
	int topic_len;
    
	topic_len = ntohs(buffer_get_u16(rb, 0));
	topic = (char *)buffer_data(rb) + 2;
    
	payload = (uint8_t *)topic + topic_len;
	payloadlen = pkt->remlen - 2 - topic_len;

	if (qos > 0) {
		mid = ntohs(buffer_get_u16(rb, 2 + topic_len));

		if (is_pending_message(cl, mid)) {
			umqtt_log_err("Duplicate PUBLISH received:"
			    "(q%d, m%d, '%.*s')\n", qos, mid, topic_len, topic);
			return;
		}

		payloadlen -= 2;
		payload += 2;
	}

	umqtt_log_debug("Received PUBLISH: "
	    "%sqos(%d), mid(%d), '%s', (%d bytes)\n", dup ? "dup, " : "",
	    qos, mid, topic, payloadlen);

	if (cl->on_publish)
		cl->on_publish(cl, topic, topic_len, payload, payloadlen);

	if (qos == UMQTT_QOS1) {
		send_puback(cl, mid);
	} else if (qos == UMQTT_QOS2) {
		store_mid(cl, mid);
		send_pubrec(cl, mid);
	}
}

static void
handle_pingresp(struct umqtt_client *cl)
{
	cl->wait_pingresp = false;

	umqtt_log_debug("Received PINGRESP\n");

	if (cl->on_pingresp)
		cl->on_pingresp(cl);
}

static int
umqtt_remlen_bytes(uint32_t remlen)
{
	int len = 0;

	do {
		remlen /= 128;
		len++;
	} while (remlen > 0);

	return len;
}

static void
umqtt_encode_remlen(uint32_t remlen, struct buffer *wb)
{
	uint8_t *p;

	do {
		p = buffer_put(wb, 1);
		*p = remlen % 128;
		remlen /= 128;
		if (remlen)
			*p |= 128;
	} while (remlen > 0);
}

static int
umqtt_connect(struct umqtt_client *cl, struct umqtt_connect_opts *opts)
{
	struct buffer *wb = &cl->wb;
	char client_id[128] = "";
	uint32_t remlen = 10;
	uint8_t flags = 0;
	bool will = false;

	if (opts->client_id)
		strncpy(client_id, opts->client_id, sizeof(client_id) - 1);
	else
		sprintf(client_id, "umqtt-%" PRId64, clock_now());

	cl->keep_alive = opts->keep_alive > 0 ?
	    opts->keep_alive : UMQTT_KEEP_ALIVE_DEFAULT;

	remlen += strlen(client_id) + 2;

	flags |= opts->clean_session << 1;

	if (opts->will_topic && opts->will_message) {
		will = true;

		remlen += strlen(opts->will_topic) + 2;
		remlen += strlen(opts->will_message) + 2;

		flags |= 1 << 2;
		flags |= (opts->will_qos & 0x3) << 3;
		flags |= opts->will_retain << 5;
	}

	if (opts->username) {
		remlen += strlen(opts->username) + 2;
		flags |= 1 << 7;

		if (opts->password) {
			remlen += strlen(opts->password) + 2;
			flags |= 1 << 6;
		}
	}

	if (remlen > UMQTT_MAX_REMLEN) {
		umqtt_log_err("remaining length overflow\n");
		return -1;
	}

	buffer_put_u8(wb, (UMQTT_CONNECT << 4) | 0x00);
	umqtt_encode_remlen(remlen, wb);

	umqtt_put_string(wb, "MQTT");   /* Protocol Name */
	buffer_put_u8(wb, 0x04);        /* Protocol Level: 3.1.1 */

	buffer_put_u8(wb, flags);
	buffer_put_u16(wb, htons(opts->keep_alive));
	umqtt_put_string(wb, client_id);

	if (will) {
		umqtt_put_string(wb, opts->will_topic);
		umqtt_put_string(wb, opts->will_message);
	}

	if (opts->username) {
		umqtt_put_string(wb, opts->username);
		if (opts->password)
			umqtt_put_string(wb, opts->password);
	}

	event_add(&cl->iow, NULL);

	return 0;
}

int
umqtt_subscribe(struct umqtt_client *cl, struct umqtt_topic *topics, int num)
{
	struct buffer *wb = &cl->wb;
	uint32_t remlen = 2;
	uint16_t mid;
	int i;

	for (i = 0; i < num; i++)
		remlen += 2 + strlen(topics[i].topic) + 1;

	if (remlen > UMQTT_MAX_REMLEN) {
		umqtt_log_err("remaining length overflow\n");
		return -1;
	}

	buffer_put_u8(wb, (UMQTT_SUBSCRIBE << 4) | 0x02);
	umqtt_encode_remlen(remlen, wb);

	mid = get_unused_mid(cl);
	buffer_put_u16(wb, htons(mid));

	for (i = 0; i < num; i++) {
		umqtt_put_string(wb, topics[i].topic);
		buffer_put_u8(wb, topics[i].qos);
	}

	event_add(&cl->iow, NULL);

	return 0;
}

int
umqtt_unsubscribe(struct umqtt_client *cl, const char **topics, int num)
{
	struct buffer *wb = &cl->wb;
	uint32_t remlen = 2;
	uint16_t mid;
	int i;

	for (i = 0; i < num; i++)
		remlen += 2 + strlen(topics[i]);

	if (remlen > UMQTT_MAX_REMLEN) {
		umqtt_log_err("remaining length overflow\n");
		return -1;
	}

	buffer_put_u8(wb, (UMQTT_UNSUBSCRIBE << 4) | 0x02);
	umqtt_encode_remlen(remlen, wb);

	mid = get_unused_mid(cl);
	buffer_put_u16(wb, htons(mid));

	for (i = 0; i < num; i++)
		umqtt_put_string(wb, topics[i]);

	event_add(&cl->iow, NULL);

	return 0;
}

static int
__umqtt_publish(struct umqtt_client *cl, uint16_t mid,
    const char *topic, uint32_t payloadlen, const void *payload, uint8_t qos,
    bool retain, bool dup)
{
	struct buffer *wb = &cl->wb;
	uint32_t remlen = UMQTT_PKT_HDR_LEN + UMQTT_PKT_TOPIC_LEN +
	    strlen(topic) + payloadlen;
	uint32_t remlen_bytes = umqtt_remlen_bytes(remlen);

	remlen += remlen_bytes;

	if (qos > 0)
		remlen += UMQTT_PKT_MID_LEN;

	if (remlen > UMQTT_MAX_REMLEN) {
		umqtt_log_err("remaining length overflow\n");
		return -1;
	}

	buffer_put_u8(wb, (UMQTT_PUBLISH << 4) | (dup & 0x1 << 3) |
	    (qos << 1) | retain);
	umqtt_encode_remlen(remlen - remlen_bytes - UMQTT_PKT_HDR_LEN, wb);

	umqtt_put_string(wb, topic);

	if (qos > 0)
		buffer_put_u16(wb, htons(mid));

	buffer_put_data(wb, payload, payloadlen);

	event_add(&cl->iow, NULL);

	umqtt_log_debug("send PUBLISH(%sq%d, m%d, '%s', (%d bytes)\n",
	    dup ? "dup, " : "", qos, mid, topic, payloadlen);

	return 0;
}

int
umqtt_publish(struct umqtt_client *cl, const char *topic,
    const void *payload, uint32_t payloadlen, uint8_t qos, bool retain)
{
	uint16_t mid = 0;

	if (qos > 0)
		mid = get_unused_mid(cl);

	if (__umqtt_publish(cl, mid, topic, payloadlen, payload,
	    qos, retain, false) < 0)
		return -1;

	return 0;
}

static void
umqtt_disconnect(struct umqtt_client *cl)
{
	uint8_t buf[] = {0xE0, 0x00};

	buffer_put_data(&cl->wb, buf, 2);
	event_add(&cl->iow, NULL);
}

static void
umqtt_ping(struct umqtt_client *cl)
{
	uint8_t buf[] = {0xC0, 0x00};

	buffer_put_data(&cl->wb, buf, 2);
	event_add(&cl->iow, NULL);

	umqtt_log_debug("Send: PINGREQ\n");
}

static void
umqtt_timer_cb(int nil, short events, void *arg)
{
	struct umqtt_client *cl = arg;
	int64_t now = clock_now();
	static const struct timeval tick = { 0, 200000 };

	evtimer_add(&cl->timer, &tick);

	if (cl->state < UMQTT_STATE_PARSE_FH) {
		if (now - cl->start_time >
		    UMQTT_MAX_CONNECT_TIME * 1000000000ULL) {
			umqtt_error(cl, UMQTT_ERROR_CONNECT, "Connect timeout");
			return;
		}
	}

	if (!cl->connection_accepted)
		return;

	if (cl->wait_pingresp) {
		if (now - cl->last_ping < 3 * 1000000000ULL)
			return;

		cl->wait_pingresp = false;
		umqtt_log_err("ping timeout %d\n", ++cl->ntimeout);
		if (cl->ntimeout > 2) {
			umqtt_error(cl, UMQTT_ERROR_PING_TIMEOUT,
			    "ping timeout");
			return;
		}
	} else {
		cl->ntimeout = 0;
	}

	if (now - cl->last_ping < cl->keep_alive * 1000000000ULL)
		return;

	cl->ping(cl);
	cl->last_ping = now;
	cl->wait_pingresp = true;
}

static int
handle_packet(struct umqtt_client *cl)
{
	struct umqtt_packet *pkt = &cl->pkt;
	struct buffer *rb = &cl->rb;

	if (buffer_length(rb) < pkt->remlen)
		return UMQTT_PARSE_PEND;

	switch (pkt->type) {
	case UMQTT_CONNACK:
		handle_conack(cl);
		break;
	case UMQTT_PUBACK:
	case UMQTT_PUBCOMP:
		handle_pubackcomp(cl);
		break;
	case UMQTT_PUBREC:
		handle_pubrec(cl);
		break;
	case UMQTT_PUBREL:
		handle_pubrel(cl);
		break;
	case UMQTT_SUBACK:
		handle_suback(cl);
		break;
	case UMQTT_UNSUBACK:
		handle_unsuback(cl);
		break;
	case UMQTT_PUBLISH:
		handle_publish(cl);
		break;
	case UMQTT_PINGRESP:
		handle_pingresp(cl);
		break;
	default:
		break;
	}

	cl->state = UMQTT_STATE_PARSE_FH;
	buffer_pull(rb, NULL, pkt->remlen);
	return UMQTT_PARSE_OK;
}

static int
parse_fixed_header(struct umqtt_client *cl)
{
	struct umqtt_packet *pkt = &cl->pkt;
	struct buffer *rb = &cl->rb;
	uint8_t *data = buffer_data(rb);
	bool more_remlen;

	if (buffer_length(rb) < 2)
		return UMQTT_PARSE_PEND;

	memset(pkt, 0, sizeof(*pkt));

	pkt->type = data[0] >> 4;
	pkt->flags = data[0] & 0xF;

	pkt->remlen = data[1] & 0x7F;
	more_remlen = data[1] & 0x80;

	if (more_remlen)
		cl->state = UMQTT_STATE_PARSE_REMLEN;
	else
		cl->state = UMQTT_STATE_HANDLE_PACKET;

	switch (pkt->type) {
	case UMQTT_CONNACK:
	case UMQTT_PUBACK:
	case UMQTT_PUBREL:
	case UMQTT_PUBCOMP:
		if (more_remlen || pkt->remlen != 2)
			return -UMQTT_REMAINING_LENGTH_MISMATCH;
		break;
	case UMQTT_PINGRESP:
		if (more_remlen || pkt->remlen)
			return -UMQTT_REMAINING_LENGTH_MISMATCH;
		break;
	default:
		break;
	}

	buffer_pull(rb, NULL, 2);

	if (cl->state == UMQTT_STATE_HANDLE_PACKET)
		return handle_packet(cl);

	return UMQTT_PARSE_OK;
}

static int
parse_remaining_length(struct umqtt_client *cl)
{
	struct umqtt_packet *pkt = &cl->pkt;
	struct buffer *rb = &cl->rb;
	uint8_t *data = buffer_data(rb);
	uint32_t parsed = 0;

	uint32_t mul = 1;
	while (parsed < UMQTT_MAX_REMLEN_BYTES) {
		mul = mul << 7;
		pkt->remlen += mul * (data[parsed] & 0x7F);
		if ((data[parsed++] & 0x80) == 0) {
			cl->state = UMQTT_STATE_HANDLE_PACKET;
			break;
		}

		if (pkt->remlen > UMQTT_MAX_REMLEN) {
			return -UMQTT_REMAINING_LENGTH_OVERFLOW;
		}
	}

	buffer_pull(rb, NULL, parsed);
	return UMQTT_PARSE_OK;
}

/*
 * UMQTT_PARSE_PEND(0): Need more data
 * UMQTT_PARSE_OK(1): OK
 * -1: Error, a negative err code
 */
static int (*parse_cbs[])(struct umqtt_client *cl) = {
	[UMQTT_STATE_PARSE_FH]		= parse_fixed_header,
	[UMQTT_STATE_PARSE_REMLEN]	= parse_remaining_length,
	[UMQTT_STATE_HANDLE_PACKET]	= handle_packet
};

static void
umqtt_parse(struct umqtt_client *cl)
{   
	int err = 0;

	do {
		if (buffer_length(&cl->rb) == 0)
			return;

		if (cl->state >= ARRAY_SIZE(parse_cbs) ||
		    !parse_cbs[cl->state])
			return;

		err = parse_cbs[cl->state](cl);
	} while(err > UMQTT_PARSE_PEND);

	if (err < 0)
		umqtt_error(cl, -err, "Invalid frame");
}

static int
check_socket_state(struct umqtt_client *cl)
{
	int err;
	socklen_t optlen = sizeof(err);

	getsockopt(cl->sock, SOL_SOCKET, SO_ERROR, &err, &optlen);
	if (err) {
		umqtt_error(cl, UMQTT_ERROR_CONNECT, strerror(err));
		return -1;
	}

	cl->state = UMQTT_STATE_PARSE_FH;

	if (cl->state == UMQTT_STATE_PARSE_FH && cl->on_net_connected)
		cl->on_net_connected(cl);
	return 0;
}

static void
umqtt_io_read_cb(int sock, short revents, void *arg)
{
	struct umqtt_client *cl = arg;
	struct buffer *rb = &cl->rb;
	bool eof;
	int ret;

	if (cl->state == UMQTT_STATE_CONNECTING) {
		if (check_socket_state(cl) < 0)
			return;
	}

	ret = buffer_put_fd(rb, sock, -1, &eof);
	if (ret < 0) {
		umqtt_error(cl, UMQTT_ERROR_IO, strerror(errno));
		return;
	}

	if (eof) {
		umqtt_free(cl);

		if (cl->on_close)
			cl->on_close(cl);
		return;
	}

	umqtt_parse(cl);
}

static void
umqtt_io_write_cb(int sock, short revents, void *arg)
{
	struct umqtt_client *cl = arg;
	int ret;

	if (cl->state == UMQTT_STATE_CONNECTING) {
		if (check_socket_state(cl) < 0)
			return;
	}

	ret = buffer_pull_to_fd(&cl->wb, sock, buffer_length(&cl->wb));
	if (ret < 0) {
		umqtt_error(cl, UMQTT_ERROR_IO, "write error");
		return;
	}

	if (buffer_length(&cl->wb) < 1)
		event_del(&cl->iow);
}

int
umqtt_init(struct umqtt_client *cl, struct event_base *base,
    const char *host, const char *port)
{
	int sock = -1;
	int eai;
	static const struct timeval onesec = { 1, 0 };

	memset(cl, 0, sizeof(struct umqtt_client));

	sock = tcp_connect(host, port, O_NONBLOCK | O_CLOEXEC, NULL, &eai);
	if (sock < 0) {
		umqtt_log_err("tcp_connect failed: %s\n", strerror(errno));
		return -1;
	} else if (sock == 0) {
		umqtt_log_err("tcp_connect failed: %s\n", gai_strerror(eai));
		return -1;
	}

	cl->base = base;

	cl->sock = sock;
	cl->connect = umqtt_connect;
	cl->subscribe = umqtt_subscribe;
	cl->unsubscribe = umqtt_unsubscribe;
	cl->publish = umqtt_publish;
	cl->ping = umqtt_ping;
	cl->disconnect = umqtt_disconnect;
	cl->free = umqtt_free;
	cl->start_time = clock_now();

	event_set(&cl->iow, sock, EV_WRITE, umqtt_io_write_cb, cl);
	event_base_set(cl->base, &cl->iow);
	event_add(&cl->iow, NULL); /* XXX */

	event_set(&cl->ior, sock, EV_READ | EV_PERSIST, umqtt_io_read_cb, cl);
	event_base_set(cl->base, &cl->ior);
	event_add(&cl->ior, NULL);

	evtimer_set(&cl->timer, umqtt_timer_cb, cl);
	event_base_set(cl->base, &cl->timer);
	evtimer_add(&cl->timer, &onesec);

	return 0;
}

struct umqtt_client *
umqtt_new(struct event_base *base,
    const char *host, const char *port)
{
	struct umqtt_client *cl;

	cl = malloc(sizeof(struct umqtt_client));
	if (!cl) {
		umqtt_log_err("malloc failed: %s\n", strerror(errno));
		return NULL;
	}

	if (umqtt_init(cl, base, host, port) < 0) {
		free(cl);
		return NULL;
	}

	return cl;
}
