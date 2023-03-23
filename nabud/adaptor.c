/*-
 * Copyright (c) 2022 Jason R. Thorpe.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * NABU Adaptor emulation.  This handles the communication with the
 * NABU PC.
 *
 * Protocol information and message details gleaned from NabuNetworkEmulator
 * (AdaptorEmulator.cs) by Nick Daniels, so the following notice from that
 * repository is included:
 */

/*
BSD 3-Clause License

Copyright (c) 2022, Nick Daniels

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define	NABU_PROTO_INLINES

#include "libnabud/crc16_genibus.h"
#include "libnabud/log.h"
#include "libnabud/nhacp_proto.h"

#include "adaptor.h"
#include "conn.h"
#include "image.h"
#include "nhacp.h"
#include "retronet.h"

static const uint8_t nabu_msg_ack[] = NABU_MSGSEQ_ACK;
static const uint8_t nabu_msg_finished[] = NABU_MSGSEQ_FINISHED;

/*
 * adaptor_escape_packet --
 *	Copy the provided buffer into the connection's pktbuf,
 *	escaping any byte that's the Escape value.
 */
static void
adaptor_escape_packet(struct nabu_connection *conn, const uint8_t *buf,
    size_t len)
{
	size_t i;

	conn->pktlen = 0;
	for (i = 0; i < len; i++) {
		if (buf[i] == NABU_MSG_ESCAPE) {
			conn->pktbuf[conn->pktlen++] = NABU_MSG_ESCAPE;
			conn->pktbuf[conn->pktlen++] = NABU_MSG_ESCAPE;
		} else {
			conn->pktbuf[conn->pktlen++] = buf[i];
		}
	}
}

/*
 * adaptor_expect_byte --
 *	Wait for an expected byte from the NABU.
 */
static bool
adaptor_expect_byte(struct nabu_connection *conn, uint8_t val)
{
	uint8_t c;

	if (! conn_recv_byte(conn, &c)) {
		log_error("[%s] Receive error.", conn_name(conn));
		return false;
	}

	log_debug(LOG_SUBSYS_ADAPTOR, "[%s] Expected 0x%02x, got 0x%02x (%s)",
	    conn_name(conn), val, c, val == c ? "success" : "fail");
	return val == c;
}

/*
 * adaptor_expect_sequence --
 *	Wait for a byte sequence from the NABU.
 */
static bool
adaptor_expect_sequence(struct nabu_connection *conn,
    const uint8_t *seq, size_t seqlen)
{
	size_t i;

	for (i = 0; i < seqlen; i++) {
		if (! adaptor_expect_byte(conn, seq[i])) {
			return false;
		}
	}
	return true;
}

/*
 * adaptor_expect_ack --
 *	Wait for an ACK from the NABU.
 */
static bool
adaptor_expect_ack(struct nabu_connection *conn)
{
	return adaptor_expect_sequence(conn, nabu_msg_ack,
	    sizeof(nabu_msg_ack));
}

/*
 * adaptor_send_ack --
 *	Send an ACK message to the NABU.
 */
static void
adaptor_send_ack(struct nabu_connection *conn)
{
	conn_send(conn, nabu_msg_ack, sizeof(nabu_msg_ack));
}

/*
 * adaptor_send_confirmed --
 *	Send a CONFIRMED message to the NABU.
 */
static void
adaptor_send_confirmed(struct nabu_connection *conn)
{
	conn_send_byte(conn, NABU_STATE_CONFIRMED);
}

/*
 * adaptor_send_unauthorized --
 *	Send an UNAUTHORIZED message to the NABU.
 */
static void
adaptor_send_unauthorized(struct nabu_connection *conn)
{
	log_debug(LOG_SUBSYS_ADAPTOR,
	    "[%s] Sending UNAUTHORIZED.", conn_name(conn));
	conn_send_byte(conn, NABU_SERVICE_UNAUTHORIZED);
	log_debug(LOG_SUBSYS_ADAPTOR,
	    "[%s] Waiting for NABU to ACK.", conn_name(conn));
	if (adaptor_expect_ack(conn)) {
		log_debug(LOG_SUBSYS_ADAPTOR,
		    "[%s] Received ACK.", conn_name(conn));
	} else {
		log_error("[%s] NABU failed to ACK.", conn_name(conn));
	}
}

/*
 * adaptor_send_packet --
 *	Send a packet to the NABU.  The buffer will be freed once the
 *	packet has been sent.
 */
static void
adaptor_send_packet(struct nabu_connection *conn, uint8_t *buf, size_t len)
{
	assert(len <= NABU_MAXPACKETSIZE);

	adaptor_escape_packet(conn, buf, len);
	log_debug(LOG_SUBSYS_ADAPTOR,
	    "[%s] Sending AUTHORIZED.", conn_name(conn));
	conn_send_byte(conn, NABU_SERVICE_AUTHORIZED);
	log_debug(LOG_SUBSYS_ADAPTOR,
	    "[%s] Waiting for NABU to ACK.", conn_name(conn));
	if (adaptor_expect_ack(conn)) {
		log_debug(LOG_SUBSYS_ADAPTOR,
		    "[%s] Received ACK, sending packet.", conn_name(conn));
		conn_send(conn, conn->pktbuf, conn->pktlen);
		conn_send(conn, nabu_msg_finished,
		    sizeof(nabu_msg_finished));
	} else {
		log_error("[%s] NABU failed to ACK.", conn_name(conn));
	}
	free(buf);
}

/*
 * adaptor_send_pak --
 *	Extract the specified segment from a pre-prepared image pak
 *	and send it to the NABU.
 */
static bool
adaptor_send_pak(struct nabu_connection *conn, uint32_t image,
    uint16_t segment, struct nabu_image *img)
{
	size_t len = NABU_TOTALPAYLOADSIZE;
	size_t off = (segment * len) + ((2 * segment) + 2);
	uint8_t *pktbuf;
	bool last = false;

	if (off >= img->length) {
		log_error(
		    "[%s] PAK %s: offset %zu exceeds pak size %zu",
		    conn_name(conn), img->name, off, img->length);
		adaptor_send_unauthorized(conn);
		return false;
	}

	if (off + len >= img->length) {
		len = img->length - off;
		last = true;
	}

	if (len < NABU_HEADERSIZE + NABU_FOOTERSIZE) {
		log_error(
		    "[%s] PAK %s: offset %zu length %zu is nonsensical",
		    conn_name(conn), img->name, off, len);
		adaptor_send_unauthorized(conn);
		return last;
	}

	pktbuf = malloc(len);
	if (pktbuf == NULL) {
		log_error("unable to allocate %zu byte packet buffer", len);
		adaptor_send_unauthorized(conn);
		return last;
	}

	memcpy(pktbuf, img->data + off, len);

	uint16_t crc = crc16_genibus_fini(crc16_genibus_update(pktbuf, len - 2,
	    crc16_genibus_init()));
	nabu_set_crc(&pktbuf[len - 2], crc);

	log_debug(LOG_SUBSYS_ADAPTOR,
	    "[%s] Sending segment %u of image %06X%s", conn_name(conn),
	    segment, image, last ? " (last segment)" : "");

	adaptor_send_packet(conn, pktbuf, len);
	return last;
}

/*
 * adaptor_send_image --
 *	Wrap the region specified by segment in the provided image
 *	buffer in a properly structured packet and send it to the NABU.
 */
static bool
adaptor_send_image(struct nabu_connection *conn, uint32_t image,
    uint16_t segment, struct nabu_image *img)
{
	size_t off = segment * NABU_MAXPAYLOADSIZE;
	size_t len = NABU_MAXPAYLOADSIZE;
	size_t pktlen, i;
	uint8_t *pktbuf;
	bool last = false;

	/*
	 * PAK images are pre-wrapped, so we have to process them a little
	 * differently.  Time packets don't have a channel, so check for
	 * that.
	 */
	if (img->channel != NULL && img->channel->type == IMAGE_CHANNEL_PAK) {
		return adaptor_send_pak(conn, image, segment, img);
	}

	if (off >= img->length) {
		log_error(
		    "image %u: segment %u offset %zu exceeds image size %zu",
		    image, segment, off, img->length);
		adaptor_send_unauthorized(conn);
		return false;
	}

	if (off + len >= img->length) {
		len = img->length - off;
		last = true;
	}

	pktlen = len + NABU_HEADERSIZE + NABU_FOOTERSIZE;
	pktbuf = malloc(pktlen);
	i = 0;

	if (pktbuf == NULL) {
		log_error("unable to allocate %zu byte packet buffer",
		    pktlen);
		adaptor_send_unauthorized(conn);
		return last;
	}

	/* 16 bytes of header */
	i += nabu_init_pkthdr(pktbuf, image, segment, off, last);

	memcpy(&pktbuf[i], img->data + off, len);	/* payload */
	i += len;

	uint16_t crc = crc16_genibus_fini(crc16_genibus_update(pktbuf, i,
	    crc16_genibus_init()));
	i += nabu_set_crc(&pktbuf[i], crc);
	if (i != pktlen) {
		log_fatal("internal packet length error");
	}

	log_debug(LOG_SUBSYS_ADAPTOR,
	    "[%s] Sending segment %u of image %06X%s", conn_name(conn),
	    segment, image, last ? " (last segment)" : "");
	adaptor_send_packet(conn, pktbuf, pktlen);
	return last;
}

/*
 * adaptor_send_time --
 *	Send a time packet to the NABU.
 */
static void
adaptor_send_time(struct nabu_connection *conn)
{
	static char time_image_name[] = "TimeImage";
	struct tm tm_store, *tm;
	time_t now;

	now = time(NULL);
	if (now == (time_t)-1) {
		log_error("unable to get current time: %s",
		    strerror(errno));
		memset(&tm_store, 0, sizeof(tm_store));
		tm = &tm_store;
	} else {
		tm = localtime_r(&now, &tm_store);
	}

	struct nabu_time t = {
		.mystery = {
			[0] = 0x02,
			[1] = 0x02,
		},
		.week_day  = tm->tm_wday + 1,
		.year      = 84,		/* as in 1984 */
		.month     = tm->tm_mon + 1,
		.month_day = tm->tm_mday,
		.hour      = tm->tm_hour,
		.minute    = tm->tm_min,
		.second    = tm->tm_sec,
	};

	struct nabu_image img = {
		.name = time_image_name,
		.data = (void *)&t,
		.length = sizeof(t),
		.number = NABU_IMAGE_TIME,
	};
	adaptor_send_image(conn, NABU_IMAGE_TIME, 0, &img);
}

/*
 * adaptor_msg_reset --
 *	Handle the RESET message.
 */
static void
adaptor_msg_reset(struct nabu_connection *conn)
{
	conn_reboot(conn);
	log_debug(LOG_SUBSYS_ADAPTOR,
	    "[%s] Sending NABU_MSGSEQ_ACK + NABU_STATE_CONFIRMED.",
	    conn_name(conn));
	adaptor_send_ack(conn);
	adaptor_send_confirmed(conn);
}

/*
 * adaptor_msg_mystery --
 *	Handle the mystery message.
 */
static void
adaptor_msg_mystery(struct nabu_connection *conn)
{
	uint8_t msg[2];

	log_debug(LOG_SUBSYS_ADAPTOR,
	    "[%s] Sending NABU_MSGSEQ_ACK.", conn_name(conn));
	adaptor_send_ack(conn);

	log_debug(LOG_SUBSYS_ADAPTOR,
	    "[%s] Expecting the NABU to send 2 bytes.", conn_name(conn));
	if (! conn_recv(conn, msg, sizeof(msg))) {
		log_error("[%s] Those two bytes never arrived.",
		    conn_name(conn));
	} else {
		log_debug(LOG_SUBSYS_ADAPTOR,
		    "[%s] msg[0] = 0x%02x msg[1] = 0x%02x",
		    conn_name(conn), msg[0], msg[1]);
	}
	log_debug(LOG_SUBSYS_ADAPTOR,
	    "[%s] Sending NABU_STATE_CONFIRMED.", conn_name(conn));
	adaptor_send_confirmed(conn);
}

/*
 * adaptor_msg_channel_status --
 *	Handle the CHANNEL_STATUS message.
 */
static void
adaptor_msg_channel_status(struct nabu_connection *conn)
{
	struct image_channel *chan = conn_get_channel(conn);

	if (chan != NULL) {
		log_debug(LOG_SUBSYS_ADAPTOR,
		    "[%s] Sending NABU_SIGNAL_STATUS_YES.",
		    conn_name(conn));
		conn_send_byte(conn, NABU_SIGNAL_STATUS_YES);
		conn_send(conn, nabu_msg_finished,
		    sizeof(nabu_msg_finished));
	} else {
		log_debug(LOG_SUBSYS_ADAPTOR,
		    "[%s] Sending NABU_SIGNAL_STATUS_NO.",
		    conn_name(conn));
		conn_send_byte(conn, NABU_SIGNAL_STATUS_NO);
		conn_send(conn, nabu_msg_finished,
		    sizeof(nabu_msg_finished));
	}
}

/*
 * adaptor_msg_transmit_status --
 *	Handle the TRANSMIT_STATUS message.
 */
static void
adaptor_msg_transmit_status(struct nabu_connection *conn)
{
	log_debug(LOG_SUBSYS_ADAPTOR,
	    "[%s] Sending MABU_MSGSEQ_FINISHED.", conn_name(conn));
	conn_send_byte(conn, NABU_SIGNAL_STATUS_YES);
	conn_send(conn, nabu_msg_finished, sizeof(nabu_msg_finished));
}

/*
 * adaptor_msg_get_status --
 *	Handle the GET_STATUS message.
 */
static void
adaptor_msg_get_status(struct nabu_connection *conn)
{
	uint8_t msg;

	log_debug(LOG_SUBSYS_ADAPTOR,
	    "[%s] Sending MABU_MSGSEQ_ACK.", conn_name(conn));
	adaptor_send_ack(conn);

	log_debug(LOG_SUBSYS_ADAPTOR,
	    "[%s] Expecting the NABU to send status type.",
	    conn_name(conn));
	if (! conn_recv_byte(conn, &msg)) {
		log_error("[%s] Status type never arrived.", conn_name(conn));
	} else {
		switch (msg) {
		case NABU_STATUS_SIGNAL:
			log_debug(LOG_SUBSYS_ADAPTOR,
			    "[%s] Channel status requested.", conn_name(conn));
			adaptor_msg_channel_status(conn);
			break;

		case NABU_STATUS_TRANSMIT:
			log_debug(LOG_SUBSYS_ADAPTOR,
			    "[%s] Transmit status requested.", conn_name(conn));
			adaptor_msg_transmit_status(conn);
			break;

		default:
			log_error("[%s] Unknown status type requsted: 0x%02x.",
			    conn_name(conn), msg);
			break;
		}
	}
}

/*
 * adaptor_msg_start_up --
 *	Handle the START_UP message.
 */
static void
adaptor_msg_start_up(struct nabu_connection *conn)
{
	log_debug(LOG_SUBSYS_ADAPTOR,
	    "[%s] Sending NABU_MSGSEQ_ACK + NABU_STATE_CONFIRMED.",
	    conn_name(conn));
	adaptor_send_ack(conn);
	adaptor_send_confirmed(conn);
}

/*
 * adaptor_msg_packet_request --
 *	Handle the PACKET_REQUEST message.
 */
static void
adaptor_msg_packet_request(struct nabu_connection *conn)
{
	uint8_t msg[4];

	log_debug(LOG_SUBSYS_ADAPTOR,
	    "[%s] Sending NABU_MSGSEQ_ACK.", conn_name(conn));
	adaptor_send_ack(conn);

	if (! conn_recv(conn, msg, sizeof(msg))) {
		log_error("[%s] NABU failed to send segment/image message.",
		    conn_name(conn));
		conn_set_state(conn, CONN_STATE_ABORTED);
		return;
	}

	uint16_t segment = msg[0];
	uint32_t image = nabu_get_uint24(&msg[1]);
	log_debug(LOG_SUBSYS_ADAPTOR,
	    "[%s] NABU requested segment %u of image %06X.",
	    conn_name(conn), segment, image);

	log_debug(LOG_SUBSYS_ADAPTOR,
	    "[%s] Sending NABU_STATE_CONFIRMED.", conn_name(conn));
	adaptor_send_confirmed(conn);

	if (image == NABU_IMAGE_TIME) {
		if (segment == 0) {
			log_debug(LOG_SUBSYS_ADAPTOR,
			    "[%s] Sending time packet.", conn_name(conn));
			adaptor_send_time(conn);
			return;
		}
		log_error(
		    "[%s] Unexpected request for segment %u of time image.",
		    conn_name(conn), segment);
		adaptor_send_unauthorized(conn);
		return;
	}

	struct nabu_image *img = image_load(conn, image);
	if (img == NULL) {
		log_error("[%s] Unable to load image %06X.",
		    conn_name(conn), image);
		adaptor_send_unauthorized(conn);
		return;
	}

	log_debug(LOG_SUBSYS_ADAPTOR,
	    "[%s] Sending segment %u of image %06X.",
	    conn_name(conn), segment, image);
	image_unload(conn, img,
	    adaptor_send_image(conn, image, segment, img));
}

/*
 * adaptor_msg_change_channel --
 *	Handle the CHANGE_CHANNEL message.
 */
static void
adaptor_msg_change_channel(struct nabu_connection *conn)
{
	uint8_t msg[2];

	log_debug(LOG_SUBSYS_ADAPTOR,
	    "[%s] Sending MABU_MSGSEQ_ACK.", conn_name(conn));
	adaptor_send_ack(conn);

	log_debug(LOG_SUBSYS_ADAPTOR,
	    "[%s] Wating for NABU to send channel code.",
	    conn_name(conn));
	if (! conn_recv(conn, msg, sizeof(msg))) {
		log_error("[%s] NABU failed to send channel code.",
		    conn_name(conn));
		conn_set_state(conn, CONN_STATE_ABORTED);
		return;
	}

	int16_t channel = (int16_t)nabu_get_uint16(msg);
	log_info("[%s] NABU selected channel 0x%04x.", conn_name(conn),
	    channel);

	image_channel_select(conn, channel);

	log_debug(LOG_SUBSYS_ADAPTOR,
	    "[%s] Sending NABU_STATE_CONFIRMED.", conn_name(conn));
	adaptor_send_confirmed(conn);
}

#define	HANDLER_INDEX(v)	((v) - NABU_MSG_CLASSIC_FIRST)
#define	HANDLER_ENTRY(v, n)						\
	[HANDLER_INDEX(v)] = {						\
		.handler    = adaptor_msg_ ## n ,			\
		.debug_desc = #v ,					\
	}

static const struct {
	void		(*handler)(struct nabu_connection *);
	const char	*debug_desc;
} adaptor_msg_types[] = {
	HANDLER_ENTRY(NABU_MSG_RESET,          reset),
	HANDLER_ENTRY(NABU_MSG_MYSTERY,        mystery),
	HANDLER_ENTRY(NABU_MSG_GET_STATUS,     get_status),
	HANDLER_ENTRY(NABU_MSG_START_UP,       start_up),
	HANDLER_ENTRY(NABU_MSG_PACKET_REQUEST, packet_request),
	HANDLER_ENTRY(NABU_MSG_CHANGE_CHANNEL, change_channel),
};
static const unsigned int adaptor_msg_type_count =
    sizeof(adaptor_msg_types) / sizeof(adaptor_msg_types[0]);

/*
 * adaptor_msg_classic --
 *	Check for and process a classic NABU message.
 */
static bool
adaptor_msg_classic(struct nabu_connection *conn, uint8_t msg)
{
	if (! NABU_MSG_IS_CLASSIC(msg)) {
		/* Not a classic NABU message. */
		return false;
	}

	uint8_t idx = HANDLER_INDEX(msg);
	if (idx > adaptor_msg_type_count ||
	    adaptor_msg_types[idx].handler == NULL) {
		log_error("[%s] Unknown classic message type 0x%02x.",
		    conn_name(conn), msg);
		return false;
	}

	log_debug(LOG_SUBSYS_ADAPTOR, 
	    "[%s] Got %s.", conn_name(conn), adaptor_msg_types[idx].debug_desc);
	(*adaptor_msg_types[idx].handler)(conn);
	return true;
}

/*
 * adaptor_event_loop --
 *	Main event loop for the Adaptor emulation.
 */
void
adaptor_event_loop(struct nabu_connection *conn)
{
	uint8_t msg;

	log_info("[%s] Connection starting.", conn_name(conn));

	for (;;) {
		/* We want to block "forever" waiting for requests. */
		conn_stop_watchdog(conn);

		log_debug(LOG_SUBSYS_ADAPTOR,
		    "[%s] Waiting for NABU.", conn_name(conn));
		if (! conn_recv_byte(conn, &msg)) {
			if (! conn_check_state(conn)) {
				/* Error already logged. */
				break;
			}
			log_debug(LOG_SUBSYS_ADAPTOR,
			    "[%s] conn_recv_byte() failed, "
			    "continuing event loop.", conn_name(conn));
			continue;
		}

		/*
		 * Now that we've got a request, we don't want any given
		 * I/O to take longer than 10 seconds.
		 */
		conn_start_watchdog(conn, 10);

		/* First check for a classic message. */
		if (adaptor_msg_classic(conn, msg)) {
			/* Yup! */
			continue;
		}

		/* Check for a RetroNet request. */
		if (retronet_request(conn, msg)) {
			/* Yup! */
			continue;
		}

		/* Check for NHACP mode. */
		if (nhacp_request(conn, msg)) {
			/* Yup! */
			continue;
		}

		log_error("[%s] Got unexpected message 0x%02x.",
		    conn_name(conn), msg);
	}
}
