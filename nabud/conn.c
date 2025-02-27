/*-
 * Copyright (c) 2022, 2023 Jason R. Thorpe.
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
 * Connection abstraction.
 *
 * Connections can be either over a serial interface to a real NABU
 * or over a TCP socket to support NABU emulators.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>

#include "libnabud/log.h"
#include "libnabud/missing.h"

#include "adaptor.h"
#include "conn.h"
#ifdef HAVE_LINUX_TERMIOS2
#include "conn_linux.h"
#endif
#include "image.h"
#include "retronet.h"
#include "nhacp.h"

static pthread_mutex_t conn_list_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t conn_list_enum_cv = PTHREAD_COND_INITIALIZER;
static TAILQ_HEAD(, nabu_connection) conn_list =
    TAILQ_HEAD_INITIALIZER(conn_list);
unsigned int conn_count;

static void
conn_insert(struct nabu_connection *conn)
{
	assert(! conn->on_list);

	pthread_mutex_lock(&conn_list_mutex);
	TAILQ_INSERT_TAIL(&conn_list, conn, link);
	conn->on_list = true;
	conn_count++;
	pthread_mutex_unlock(&conn_list_mutex);
}

static void
conn_remove(struct nabu_connection *conn)
{
	if (conn->on_list) {
		pthread_mutex_lock(&conn_list_mutex);
		while (conn->enum_count != 0) {
			pthread_cond_wait(&conn_list_enum_cv,
			    &conn_list_mutex);
		}
		TAILQ_REMOVE(&conn_list, conn, link);
		conn->on_list = false;
		conn_count--;
		pthread_mutex_unlock(&conn_list_mutex);
	}
}

/*
 * conn_enumerate --
 *	Enumerate all of the connections.
 */
bool
conn_enumerate(bool (*func)(struct nabu_connection *, void *), void *ctx)
{
	struct nabu_connection *conn;
	bool rv = true;

	pthread_mutex_lock(&conn_list_mutex);
	TAILQ_FOREACH(conn, &conn_list, link) {
		conn->enum_count++;
		assert(conn->enum_count != 0);
		pthread_mutex_unlock(&conn_list_mutex);
		if (! (*func)(conn, ctx)) {
			rv = false;
		}
		pthread_mutex_lock(&conn_list_mutex);
		assert(conn->enum_count != 0);
		conn->enum_count--;
		pthread_cond_broadcast(&conn_list_enum_cv);
		if (rv == false) {
			break;
		}
	}
	pthread_mutex_unlock(&conn_list_mutex);

	return rv;
}

/*
 * conn_thread --
 *	Worker thread that handles NABU connections.
 */
static void *
conn_thread(void *arg)
{
	struct nabu_connection *conn = arg;

	/* Just run the Adaptor event loop until it returns. */
	adaptor_event_loop(conn);

	/*
	 * If we got there, the connection was cancelled or aborted,
	 * so so ahead and destroy it now.
	 */
	conn_destroy(conn);

	return NULL;
}

/*
 * conn_create_common --
 *	Common connection-creation duties.
 */
static void
conn_create_common(char *name, int fd, const struct conn_add_args *args,
    conn_type type, void *(*func)(void *))
{
	struct nabu_connection *conn;

	conn = calloc(1, sizeof(*conn));
	if (conn == NULL) {
		log_error("[%s] Unable to allocate connection structure.",
		    name);
		close(fd);
		return;
	}

	conn->type = type;
	LIST_INIT(&conn->nhacp_sessions);

	/* Not exactly "common", but hey, we allocate the conn here. */
	if (conn->type == CONN_TYPE_SERIAL) {
		conn->baud = args->baud;
		conn->stop_bits = args->stop_bits;
		conn->flow_control = args->flow_control;
	}

	conn->file_root = args->file_root;
	pthread_mutex_init(&conn->mutex, NULL);

	if (! conn_io_init(&conn->io, name, fd)) {
		/* Error already logged. */
		goto bad;
	}

	if (conn->file_root != NULL) {
		log_info("[%s] Using '%s' for local storage.",
		    conn_name(conn), conn->file_root);
	}

	/*
	 * If a channel was specified, set it now.
	 */
	if (args->channel != 0) {
		image_channel_select(conn, (int16_t)args->channel);
	}
	conn->l_selected_file = args->selected_file;

	if (! conn_io_start(&conn->io, func, conn)) {
		/* Error already logged. */
		goto bad;
	}

	conn_insert(conn);
	return;

 bad:
	conn_destroy(conn);
	return;
}

/*
 * The native buad rate of the NABU is:
 *
 *	3.57954MHz	 / 2			/ 16
 *	NTSC Colorbust	   on-board divider	  on-chip divider on TR1863
 *
 * ==> 111860.625
 */
#define	NABU_NATIVE_BPS		((3579540 / 2) / 16)
#define	NABU_FALLBACK_BPS	115200

/*
 * conn_serial_setparam --
 *	Set the specified parameters on the serial port.
 */
static bool
conn_serial_setparam(int fd, const struct conn_add_args *args)
{
	struct termios t;

	assert(args->stop_bits == 1 || args->stop_bits == 2);

	if (tcgetattr(fd, &t) < 0) {
		log_error("[%s] tcgetattr() failed: %s", args->port,
		    strerror(errno));
		goto failed;
	}

	cfmakeraw(&t);
	t.c_cflag &= ~(CSIZE | PARENB | PARODD);
	t.c_cflag |= CLOCAL | CS8;

	if (args->stop_bits == 2) {
		t.c_cflag |= CSTOPB;
	} else {
		t.c_cflag &= ~CSTOPB;
	}

	if (args->flow_control) {
		t.c_cflag |= CRTSCTS;
	} else {
		t.c_cflag &= ~CRTSCTS;
	}

#ifdef HAVE_LINUX_TERMIOS2
	/*
	 * For Linux, we need to use a different API to set the speed,
	 * but only after we use the standard termios API to set all
	 * of the other parameters.  But we can't do it in-line here
	 * because of course we can't (see conn_linux.c for details).
	 *
	 * Complicating matters, apparently Linux doesn't have termios2
	 * on all architecture.  &shrug;
	 */
	if (tcsetattr(fd, TCSANOW, &t) < 0) {
		log_error("[%s] Failed to set 8N%u%s: %s", args->port,
		    args->stop_bits,
		    args->flow_control ? "+RTS/CTS" : "", strerror(errno));
		goto failed;
	}

	if (! conn_serial_setspeed_linux(fd, args)) {
		/* Specific error message already logged. */
		log_error("[%s] Failed to set %u baud.", args->port,
		    args->baud);
		goto failed;
	}
#else /* ! HAVE_LINUX_TERMIOS2 */
	if (cfsetspeed(&t, (speed_t)args->baud) < 0) {
		log_error("[%s] cfsetspeed(%u) failed: %s", args->port,
		    args->baud, strerror(errno));
		goto failed;
	}

	if (tcsetattr(fd, TCSANOW, &t) < 0) {
		log_error("[%s] Failed to set 8N%u-%u%s: %s", args->port,
		    args->stop_bits, args->baud,
		    args->flow_control ? "+RTS/CTS" : "", strerror(errno));
		goto failed;
	}
#endif /* HAVE_LINUX_TERMIOS2 */

	return true;
 failed:
	return false;
}

/*
 * conn_add_serial --
 *	Add a serial connection.
 */
void
conn_add_serial(struct conn_add_args *args)
{
	int fd;

	log_info("Creating Serial connection on %s.", args->port);

	fd = open(args->port, O_RDWR | O_NONBLOCK | O_NOCTTY);
	if (fd < 0) {
		log_error("Unable to open %s: %s", args->port, strerror(errno));
		return;
	}

	/*
	 * The native protocol is 8N1 @ 111860 baud, but it's much
	 * more reliable if we use 2 stop bits.  Otherwise, the NABU
	 * can get out of sync when receiving a stream of bytes in
	 * a packet.
	 *
	 * Configuration can override the default.
	 */
	if (args->stop_bits == 0) {
		args->stop_bits = 2;
	}

	if (args->baud != 0) {
		if (! conn_serial_setparam(fd, args)) {
			log_error("[%s] Unable to set configured baud rate.",
			    args->port);
			goto bad;
		}
	} else {
		/*
		 * We first try to set the NABU's native baud rate,
		 * and if that fails, fall back to a more "standard"
		 * 115.2K.
		 */
		args->baud = NABU_NATIVE_BPS;
		if (! conn_serial_setparam(fd, args)) {
			log_error("[%s] Failed to set NABU-native baud rate; "
			    "falling back...", args->port);
			args->baud = NABU_FALLBACK_BPS;
			if (! conn_serial_setparam(fd, args)) {
				log_error("[%s] Failed to set fallback "
				    "baud rate.", args->port);
				goto bad;
			}
		}
	}
	log_info("[%s] Using 8N%u-%u%s.", args->port, args->stop_bits,
	    args->baud, args->flow_control ? "+RTS/CTS" : "");

	conn_create_common(args->port, fd, args, CONN_TYPE_SERIAL,
	    conn_thread);
 	return;
 bad:
	close(fd);
}

/*
 * conn_tcp_thread --
 *	Worker thread that handles accepting TCP connections from
 *	NABU emulators (like MAME).
 */
static void *
conn_tcp_thread(void *arg)
{
	struct nabu_connection *conn = arg;
	struct image_channel *chan;
	char host[NI_MAXHOST];
	struct sockaddr_storage peerss;
	socklen_t peersslen;
	struct conn_add_args args;
	int sock, v;

	for (;;) {
		peersslen = sizeof(peerss);
		if (! conn_io_accept(&conn->io, (struct sockaddr *)&peerss,
				     &peersslen, &sock)) {
			/* Error already logged. */
			break;
		}

		/* Disable Nagle. */
		v = 1;
		setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &v, sizeof(v));

		/* Get the numeric peer name string. */
		v = getnameinfo((struct sockaddr *)&peerss,
		    peersslen, host, sizeof(host), NULL, 0,
		    NI_NUMERICHOST);
		if (v) {
			log_error("[%s] getnameinfo() failed: %s",
			    conn_name(conn), gai_strerror(v));
			close(sock);
			continue;
		}

		log_info("[%s] Creating TCP connection for %s.",
		    conn_name(conn), host);

		pthread_mutex_lock(&conn->mutex);
		chan = conn->l_channel;
		pthread_mutex_unlock(&conn->mutex);

		memset(&args, 0, sizeof(args));

		args.channel = chan != NULL ? chan->number : 0;
		args.file_root = conn->file_root != NULL ?
		    strdup(conn->file_root) : NULL;
		args.selected_file = conn_get_selected_file(conn);

		conn_create_common(strdup(host), sock, &args,
		    CONN_TYPE_TCP, conn_thread);
	}

	/* Error on the listen socket -- He's dead, Jim. */
	conn_destroy(conn);

	return NULL;
}

/*
 * conn_add_tcp --
 *	Add a TCP listener.  This creates a "connection" that simply
 *	listens for incoming connections from the network and in-turn
 *	creates new connections to service them.
 */
void
conn_add_tcp(const struct conn_add_args *args)
{
	int sock;
	long port;
	char name[sizeof("IPv4-65536")];

	log_info("Creating TCP listener on port %s.", args->port);

	port = strtol(args->port, NULL, 10);
	if (port < 1 || port > UINT16_MAX) {
		log_error("Invalid TCP port number: %s", args->port);
		return;
	}

	static const struct addrinfo hints = {
		.ai_flags = AI_PASSIVE | AI_NUMERICSERV,
		.ai_socktype = SOCK_STREAM,
		.ai_protocol = IPPROTO_TCP,
	};
	struct addrinfo *ai0, *ai;
	int error;

	error = getaddrinfo(NULL, args->port, &hints, &ai0);
	if (error) {
		log_error("getaddrinfo() failed: %s", gai_strerror(error));
		return;
	}

	for (ai = ai0; ai != NULL; ai = ai->ai_next) {
		snprintf(name, sizeof(name), "IPv%s-%ld",
		    ai->ai_family == AF_INET ? "4" :
		    ai->ai_family == AF_INET6 ? "6" : "?",
		    port);

		sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (sock >= 0) {
			if (bind(sock, ai->ai_addr, ai->ai_addrlen) == 0) {
				if (listen(sock, 8) == 0) {
					conn_create_common(strdup(name), sock,
					    args, CONN_TYPE_LISTENER,
					    conn_tcp_thread);
					sock = -1;
				} else {
					log_error("Unable to listen on %s: %s",
					    name, strerror(errno));
				}
			} else {
				log_error("Unable to bind %s: %s",
				    name, strerror(errno));
			}
		} else {
			log_error("Unable to create %s socket: %s",
			    name, strerror(errno));
		}
		if (sock >= 0) {
			close(sock);
		}
	}
	freeaddrinfo(ai0);
}

/*
 * conn_destroy --
 *	Destroy a connection structure.
 */
void
conn_destroy(struct nabu_connection *conn)
{
	conn_remove(conn);

	image_release(conn_set_last_image(conn, NULL));
	conn_reboot(conn);

	pthread_mutex_destroy(&conn->mutex);

	conn_io_fini(&conn->io);

	free(conn->file_root);
	free(conn);
}

/*
 * conn_reboot --
 *	Handle a the reboot of a client at the other end of the
 *	connection.
 */
void
conn_reboot(struct nabu_connection *conn)
{
	if (! LIST_EMPTY(&conn->nhacp_sessions)) {
		log_info("[%s] Clearing previous NHACP state.",
		    conn_name(conn));
		nhacp_conn_fini(conn);
	}
	if (conn->retronet != NULL) {
		log_info("[%s] Clearing previous RetroNet state.",
		    conn_name(conn));
		retronet_conn_fini(conn);
	}
}

/*
 * conn_get_last_image --
 *	Return the last image used by the connection.
 */
struct nabu_image *
conn_get_last_image(struct nabu_connection *conn)
{
	struct nabu_image *img;

	pthread_mutex_lock(&conn->mutex);
	img = conn->l_last_image;
	pthread_mutex_unlock(&conn->mutex);

	return img;
}

/*
 * conn_set_last_image --
 *	Set the specified image as the most-recent.  Returns
 *	the old value.
 */
struct nabu_image *
conn_set_last_image(struct nabu_connection *conn, struct nabu_image *img)
{
	struct nabu_image *oimg;

	pthread_mutex_lock(&conn->mutex);
	oimg = conn->l_last_image;
	conn->l_last_image = img;
	pthread_mutex_unlock(&conn->mutex);

	return oimg;
}

/*
 * conn_set_last_image_if --
 *	Like conn_set_last_image(), but only if the last image
 *	matches the specified match value.
 */
struct nabu_image *
conn_set_last_image_if(struct nabu_connection *conn, struct nabu_image *match,
    struct nabu_image *img)
{
	struct nabu_image *oimg;

	pthread_mutex_lock(&conn->mutex);
	if (conn->l_last_image == match) {
		oimg = conn->l_last_image;
		conn->l_last_image = img;
	} else {
		oimg = NULL;
	}
	pthread_mutex_unlock(&conn->mutex);

	return oimg;
}

/*
 * conn_get_channel --
 *	Return the connection's currently-selected channel.
 */
struct image_channel *
conn_get_channel(struct nabu_connection *conn)
{
	struct image_channel *chan;

	pthread_mutex_lock(&conn->mutex);
	chan = conn->l_channel;
	pthread_mutex_unlock(&conn->mutex);

	return chan;
}

/*
 * conn_set_channel --
 *	Set the specified channel as the connection's selected channel.
 */
void
conn_set_channel(struct nabu_connection *conn, struct image_channel *chan)
{
	char *selected_file;

	/*
	 * Changing the channel clears the selected file.
	 */

	pthread_mutex_lock(&conn->mutex);
	conn->l_channel = chan;
	conn->retronet_enabled = chan->retronet_enabled;
	selected_file = conn->l_selected_file;
	conn->l_selected_file = NULL;
	pthread_mutex_unlock(&conn->mutex);

	if (selected_file != NULL) {
		free(selected_file);
	}
}

/*
 * conn_get_selected_file --
 *	Return the selected file on this connection, or NULL if
 *	no file is selected.  Caller must free the returned string.
 */
static const char *
conn_get_selected_file_logic(struct nabu_connection *conn)
{
	if (conn->l_selected_file != NULL) {
		return conn->l_selected_file;
	}
	if (conn->l_channel != NULL) {
		return conn->l_channel->default_file;
	}
	return NULL;
}

char *
conn_get_selected_file(struct nabu_connection *conn)
{
	size_t len;
	const char *sel;
	char *cp;

	for (cp = NULL, len = 0;;) {
		/*
		 * Avoid allocating memory while holding the
		 * channel lock.  First, figure out what selection
		 * we're going to use and drop the lock.  Then,
		 * allocate space for the name, re-acquire the
		 * lock, and determine the selection again.  If
		 * the selection will still fit, copy the string
		 * and return.  Otherwise, try again.
		 */

		pthread_mutex_lock(&conn->mutex);
		sel = conn_get_selected_file_logic(conn);
		if (sel != NULL) {
			len = strlen(sel);
		}
		pthread_mutex_unlock(&conn->mutex);
		if (sel == NULL) {
			return NULL;
		}

		cp = malloc(len + 1);
		if (cp == NULL) {
			return NULL;
		}

		pthread_mutex_lock(&conn->mutex);
		sel = conn_get_selected_file_logic(conn);
		if (sel != NULL && strlen(sel) <= len) {
			strcpy(cp, sel);
		} else {
			sel = NULL;
		}
		pthread_mutex_unlock(&conn->mutex);
		if (sel != NULL) {
			return cp;
		}
		free(cp);
	}
}

/*
 * conn_set_selected_file --
 *	Set the selected file for the connection.
 */
void
conn_set_selected_file(struct nabu_connection *conn, char *name)
{
	char *oname;

	pthread_mutex_lock(&conn->mutex);
	oname = conn->l_selected_file;
	conn->l_selected_file = name;
	pthread_mutex_unlock(&conn->mutex);

	if (oname != NULL) {
		free(oname);
	}
}
