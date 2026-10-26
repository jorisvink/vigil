/*
 * Copyright (c) 2026 Joris Vink <joris@sanctorum.se>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/socket.h>

#include <arpa/inet.h>
#include <netinet/in.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "vigil.h"

static void	liturgy_io_read(void);
static void	liturgy_io_event(void *);

static void	liturgy_kyrka_event(KYRKA *, union kyrka_event *, void *);
static void	liturgy_kyrka_send(struct kyrka_packet *, u_int64_t, void *);

/* The local i/o event schedule for use in our event loop. */
static struct vigil_io			io;

/* The current cathedral used for the liturgy. */
static struct vigil_cathedral		cathedral;

/* Our libkyrka context. */
static KYRKA				*liturgy = NULL;

/* The next time we should notify our cathedral. */
static u_int64_t			next_notify;

/*
 * Initialise our autodiscovery by creating a socket, setting up
 * the libkyrka context and getting the socket setup in our event loop.
 */
void
vigil_liturgy_init(void)
{
	struct kyrka_cathedral_cfg	cfg;

	PRECOND(liturgy == NULL);
	PRECOND(vigil->config->mode == VIGIL_MODE_LITURGY);

	next_notify = 0;

	if ((io.fd = vigil_socket_udp()) == -1) {
		vigil->flags &= ~VIGIL_FLAG_RUNNING;
		return;
	}

	io.handle = liturgy_io_event;
	vigil_io_schedule(&io);

	if ((liturgy = kyrka_ctx_alloc(liturgy_kyrka_event, NULL)) == NULL)
		fatal("failed to create liturgy context");

	memset(&cfg, 0, sizeof(cfg));
	cfg.remembrance = 1;

	memcpy(&cathedral, &vigil->cathedral, sizeof(vigil->cathedral));

	cathedral.last = vigil->now;
	cathedral.timeout = VIGIL_CATHEDRAL_TIMEOUT_INIT;

	cfg.send = liturgy_kyrka_send;
	cfg.tunnel = vigil->config->spi & 0xff;
	cfg.group = vigil->config->liturgy_group;
	cfg.identity = vigil->config->cathedral_id;
	cfg.flock_src = vigil->config->cathedral_flock;
	cfg.flock_dst = vigil->config->cathedral_flock;

	if (vigil->config->flags & VIGIL_CONFIG_FLAG_HIDDEN)
		cfg.hidden = 1;

	if (kyrka_cathedral_config(liturgy, &cfg) == -1) {
		fatal("failed to configure cathedral: %d",
		    kyrka_last_error(liturgy));
	}

	if (kyrka_cathedral_secret_load(liturgy,
	    vigil->config->cathedral_secret,
	    sizeof(vigil->config->cathedral_secret)) == -1) {
		fatal("kyrka_cathedral_secret_load: %d",
		    kyrka_last_error(liturgy));
	}

	if (kyrka_cathedral_cosk_load(liturgy,
	    vigil->config->cathedral_cosk,
	    sizeof(vigil->config->cathedral_cosk)) == -1) {
		fatal("kyrka_cathedral_cosk_load: %d",
		    kyrka_last_error(liturgy));
	}

	if (vigil->config->flags & VIGIL_CONFIG_FLAG_SHROUD) {
		if (kyrka_shroud_enable(liturgy) == -1) {
			fatal("failed to enable shroud: %d",
			    kyrka_last_error(liturgy));
		}
	}

	if (kyrka_mtu_size(liturgy, vigil->config->mtu) == -1) {
		fatal("failed to set libkyrka mtu size: %d",
		    kyrka_last_error(liturgy));
	}

	vigil_log(LOG_INFO, "liturgy running (%s) group %04x",
	    vigil_address(&cathedral.addr), vigil->config->liturgy_group);
}

/*
 * Tear down the liturgy, called when we are shutting down.
 */
void
vigil_liturgy_cleanup(void)
{
	if (liturgy == NULL)
		return;

	vigil_io_unschedule(&io);
	vigil_socket_close(io.fd);

	kyrka_ctx_free(liturgy);
	liturgy = NULL;
}

/*
 * Update the current connected cathedral about our presence and check
 * when the last cathedral response was. If we consider the cathedral
 * timed out we select a new one and try that one (if we can).
 */
void
vigil_liturgy_update(void)
{
	PRECOND(liturgy != NULL);

	if (next_notify > 0 && vigil->now < next_notify)
		return;

	next_notify = vigil->now + VIGIL_CATHEDRAL_NOTIFY;

	if ((vigil->now - cathedral.last) > cathedral.timeout) {
		if (cathedral.alive) {
			vigil_log(LOG_NOTICE,
			    "liturgy cathedral timed out (%u)",
			    cathedral.timeout / 1000);
		}

		cathedral.alive = 0;

		if (vigil_remembrance_cathedral(&vigil->remembrance,
		    &cathedral) != -1) {
			vigil_log(LOG_NOTICE,
			    "liturgy switching to cathedral %s",
			    vigil_address(&cathedral.addr));
		} else {
			cathedral.last = vigil->now;
		}
	}

	if (kyrka_cathedral_liturgy(liturgy, NULL, 0) == -1) {
		vigil_log(LOG_NOTICE, "liturgy kyrka_cathedral_liturgy: %d",
		    kyrka_last_error(liturgy));
	}
}

/*
 * Give the liturgy a new socket and tell the cathedral about us right
 * away, the network we were on changed.
 */
void
vigil_liturgy_rebind(void)
{
	int		fd;

	if (liturgy == NULL)
		return;

	fd = vigil_socket_udp();
	if (fd == -1)
		return;

	vigil_io_unschedule(&io);
	vigil_socket_close(io.fd);

	io.fd = fd;
	vigil_io_schedule(&io);

	next_notify = 0;
}

/*
 * Tell the cathedral about us on the next update.
 */
void
vigil_liturgy_poke(void)
{
	next_notify = 0;
}

/*
 * Returns 1 if the liturgy its cathedral is alive, 0 otherwise.
 */
int
vigil_liturgy_alive(void)
{
	if (liturgy == NULL)
		return (0);

	return (cathedral.alive);
}

/*
 * Returns the address of the cathedral the liturgy currently uses,
 * or NULL if the liturgy is not running.
 */
struct sockaddr_in *
vigil_liturgy_cathedral(void)
{
	if (liturgy == NULL)
		return (NULL);

	return (&cathedral.addr);
}

/*
 * Callback for when an event occurred on our liturgy socket.
 */
static void
liturgy_io_event(void *udata)
{
	PRECOND(udata == &io);

	if (io.flags & VIGIL_IO_READABLE)
		liturgy_io_read();
}

/*
 * Attempt to read packets from our liturgy socket and push them
 * into the libkyrka context so they get handled.
 */
static void
liturgy_io_read(void)
{
	int			idx;
	size_t			len;
	ssize_t			ret;
	struct kyrka_packet	pkt;
	u_int8_t		*ptr;

	for (idx = 0; idx < VIGIL_IO_BATCH; idx++) {
		if ((ptr = kyrka_packet_recvbuf(liturgy, &pkt, &len)) == NULL)
			fatal("failed to get liturgy receive buffer");

		if ((ret = read(io.fd, ptr, len)) == -1) {
			if (errno == EINTR)
				continue;
			if (errno == EWOULDBLOCK || errno == EAGAIN) {
				io.flags &= ~VIGIL_IO_READABLE;
				break;
			}
			vigil_log(LOG_NOTICE, "liturgy read: %s", errno_s);
			io.flags &= ~VIGIL_IO_READABLE;
			break;
		}

		if (ret == 0)
			continue;

		pkt.length = ret;
		pkt.shroud = KYRKA_PACKET_SHROUD_CATHEDRAL;

		if (kyrka_purgatory_input(liturgy, &pkt) == -1) {
			vigil_log(LOG_NOTICE, "liturgy purgatory input: %d",
			    kyrka_last_error(liturgy));
		}
	}
}

/*
 * Callback from libkyrka when an event occurred on our liturgy context.
 */
static void
liturgy_kyrka_event(KYRKA *ctx, union kyrka_event *evt, void *udata)
{
	int		idx;
	u_int8_t	local;

	PRECOND(ctx == liturgy);
	PRECOND(evt != NULL);
	PRECOND(udata == NULL);

	if (evt->type == KYRKA_EVENT_LITURGY_RECEIVED ||
	    evt->type == KYRKA_EVENT_REMEMBRANCE_RECEIVED) {
		cathedral.last = vigil->now;
		cathedral.timeout = VIGIL_CATHEDRAL_TIMEOUT;

		if (cathedral.alive == 0) {
			cathedral.alive = 1;
			vigil_log(LOG_INFO, "liturgy cathedral %s is alive",
			    vigil_address(&cathedral.addr));
		}
	}

	local = vigil->config->spi & 0xff;

	switch (evt->type) {
	case KYRKA_EVENT_LITURGY_RECEIVED:
		for (idx = 1; idx < KYRKA_PEERS_PER_FLOCK; idx++) {
			if (idx == local)
				continue;
			vigil_peer_state(idx, evt->liturgy.peers[idx]);
		}
		break;
	case KYRKA_EVENT_REMEMBRANCE_RECEIVED:
		vigil_remembrance_save(&vigil->remembrance, &evt->remembrance);
		break;
	case KYRKA_EVENT_LOGMSG:
		vigil_log(LOG_INFO, "libkyrka: %s", evt->logmsg.log);
		break;
	default:
		vigil_log(LOG_NOTICE,
		    "liturgy received unexpected event %u", evt->type);
		break;
	}
}

/*
 * Callback from libkyrka when we have data to be sent to the cathedral.
 */
static void
liturgy_kyrka_send(struct kyrka_packet *pkt, u_int64_t magic, void *udata)
{
	size_t		len;
	u_int8_t	*ptr;

	PRECOND(pkt != NULL);
	PRECOND(udata == NULL);

	ptr = kyrka_packet_sendbuf(liturgy, pkt, &len);
	if (ptr == NULL)
		fatal("failed to get liturgy sendbuf");

	if (sendto(io.fd, ptr, len, 0,
	    (const struct sockaddr *)&cathedral.addr,
	    sizeof(cathedral.addr)) == -1) {
		if (errno != EAGAIN && errno != EWOULDBLOCK &&
		    !vigil_socket_unreachable(errno, 1))
			vigil_log(LOG_NOTICE, "liturgy sendto: %s", errno_s);
	}
}
