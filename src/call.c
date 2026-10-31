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

#include <libkyrka/libnyfe.h>

/* The milliseconds between attempts to resolve a gospel cathedral. */
#define CALL_RESOLVE_RETRY		(10 * 1000)

/*
 * A liturgy we run for calls, with its own socket and context.
 */
struct call_liturgy {
	struct vigil_io		io;
	u_int16_t		group;
	int			signaling;

	u_int64_t		rx;
	u_int64_t		tx;
	struct vigil_cathedral	cathedral;

	KYRKA			*ctx;
};

static void	call_io_event(void *);
static int	call_cathedral_resolve(void);

static void	call_liturgy_read(struct call_liturgy *);
static void	call_liturgy_check(struct call_liturgy *);
static void	call_liturgy_rebind(struct call_liturgy *);
static void	call_liturgy_cleanup(struct call_liturgy *);
static int	call_liturgy_init(struct call_liturgy *, int);

static void	call_kyrka_event(KYRKA *, union kyrka_event *, void *);
static void	call_kyrka_send(struct kyrka_packet *, u_int64_t, void *);

/* The signaling and discovery liturgies. */
static struct call_liturgy		signaling;
static struct call_liturgy		discovery;

/* If we are running at all. */
static int				running = 0;

/* The configured cathedral and when try to resolve it again. */
static struct sockaddr_in		configured;
static int				resolved = 0;
static u_int64_t			next_resolve = 0;

/* The remembrance we get back from our main cathedral. */
static struct vigil_remembrance		remembrance;

/* The last and next time we should update the cathedral. */
static u_int64_t			last_notify;
static u_int64_t			next_notify;

/*
 * Each call we make gets its own number that we signal to the peer, so it
 * can tell a new call from the one it just had with us.
 */
static volatile u_int8_t		serial;

/* The peers we want to call and with what call, from any thread. */
static volatile u_int8_t		wanted[KYRKA_PEERS_PER_FLOCK];

/* The peers the cathedral told us want to call us and with what call. */
static volatile u_int8_t		present[KYRKA_PEERS_PER_FLOCK];

/* When the cathedral last pushed each peer to us. */
static u_int64_t			pushed[KYRKA_PEERS_PER_FLOCK];

/* The peers the cathedral told us are around to be called. */
static volatile u_int8_t		online[KYRKA_PEERS_PER_FLOCK];

/*
 * The call we had with a peer that the cathedral still shows us, we don't
 * report it as calling until it calls with another one or it is gone.
 */
static volatile u_int8_t		done[KYRKA_PEERS_PER_FLOCK];

/* What we last sent to the cathedral. */
static u_int8_t				sent[KYRKA_PEERS_PER_FLOCK];

/*
 * Initialise a call by setting up the relevant liturgies.
 */
void
vigil_call_init(const char *datadir)
{
	int		idx;
	u_int8_t	id;

	PRECOND(datadir != NULL);

	VERIFY(running == 0);
	VERIFY(vigil->gospel != NULL);
	VERIFY(vigil->gospel->mode == VIGIL_MODE_GOSPEL);

	last_notify = 0;
	next_notify = 0;
	next_resolve = 0;

	memset(sent, 0, sizeof(sent));
	memset(pushed, 0, sizeof(pushed));
	memset(&configured, 0, sizeof(configured));
	memset(&remembrance, 0, sizeof(remembrance));

	vigil_remembrance_init(&remembrance, datadir,
	    vigil->gospel->cathedral_flock | VIGIL_CALL_FLOCK_DOMAIN);

	vigil_call_hangup();

	for (idx = 0; idx < KYRKA_PEERS_PER_FLOCK; idx++) {
		vigil_atomic_write(&done[idx], 0);
		vigil_atomic_write(&online[idx], 0);
		vigil_atomic_write(&present[idx], 0);
	}

	if (call_liturgy_init(&signaling, 1) == -1) {
		vigil_log(LOG_NOTICE, "calls are not available");
		call_liturgy_cleanup(&signaling);
		return;
	}

	if (call_liturgy_init(&discovery, 0) == -1) {
		vigil_log(LOG_NOTICE, "calls are not available");
		call_liturgy_cleanup(&signaling);
		call_liturgy_cleanup(&discovery);
		return;
	}

	nyfe_random_bytes(&id, sizeof(id));
	vigil_atomic_write(&serial, id);

	running = 1;

	if (call_cathedral_resolve() != -1)
		resolved = 1;

	vigil_log(LOG_INFO, "calls running group %04x",
	    vigil->gospel->liturgy_group);
}

/*
 * Cleanup any resources allocated for a call.
 */
void
vigil_call_cleanup(void)
{
	int		idx;

	if (!running)
		return;

	call_liturgy_cleanup(&signaling);
	call_liturgy_cleanup(&discovery);
	vigil_remembrance_cleanup(&remembrance);

	vigil_call_hangup();

	for (idx = 0; idx < KYRKA_PEERS_PER_FLOCK; idx++) {
		vigil_atomic_write(&online[idx], 0);
		vigil_atomic_write(&present[idx], 0);
	}

	running = 0;
	resolved = 0;
}

/*
 * Update the cathedral with what we want right now.
 */
void
vigil_call_update(void)
{
	int		idx;
	u_int8_t	peers[KYRKA_PEERS_PER_FLOCK];

	if (!running)
		return;

	if (resolved == 0 && vigil->now >= next_resolve) {
		if (call_cathedral_resolve() != -1)
			resolved = 1;
	}

	for (idx = 1; idx < KYRKA_PEERS_PER_FLOCK; idx++) {
		if (!vigil_atomic_read(&present[idx]))
			continue;

		if ((vigil->now - pushed[idx]) < VIGIL_CALL_PRESENT_EXPIRE)
			continue;

		vigil_atomic_write(&done[idx], 0);
		vigil_atomic_write(&present[idx], 0);
	}

	for (idx = 0; idx < KYRKA_PEERS_PER_FLOCK; idx++)
		peers[idx] = vigil_atomic_read(&wanted[idx]);

	if (next_notify > 0 && vigil->now < next_notify) {
		if (!memcmp(peers, sent, sizeof(peers)))
			return;
		if ((vigil->now - last_notify) < VIGIL_CALL_NOTIFY_MIN)
			return;
	}

	memcpy(sent, peers, sizeof(sent));
	last_notify = vigil->now;
	next_notify = vigil->now + VIGIL_CALL_NOTIFY;

	call_liturgy_check(&signaling);
	call_liturgy_check(&discovery);

	if (kyrka_cathedral_liturgy(signaling.ctx,
	    peers, sizeof(peers)) == -1) {
		vigil_log(LOG_NOTICE, "signaling kyrka_cathedral_liturgy: %d",
		    kyrka_last_error(signaling.ctx));
	}

	if (kyrka_cathedral_liturgy(discovery.ctx, NULL, 0) == -1) {
		vigil_log(LOG_NOTICE, "discovery kyrka_cathedral_liturgy: %d",
		    kyrka_last_error(discovery.ctx));
	}
}

/*
 * Give both liturgies a new socket and update the cathedral right away,
 * the network we were on changed.
 */
void
vigil_call_rebind(void)
{
	if (!running)
		return;

	call_liturgy_rebind(&signaling);
	call_liturgy_rebind(&discovery);

	next_notify = 0;
}

/*
 * Update both liturgies on immediately on the next update.
 */
void
vigil_call_poke(void)
{
	next_notify = 0;
}

/*
 * Returns our current signaling cathedral.
 */
struct sockaddr_in *
vigil_call_cathedral(void)
{
	if (!running)
		return (NULL);

	if (signaling.cathedral.addr.sin_port == 0)
		return (NULL);

	return (&signaling.cathedral.addr);
}

/*
 * Returns 1 if the signaling cathedral is alive.
 */
int
vigil_call_alive(void)
{
	if (!running)
		return (0);

	return (signaling.cathedral.alive);
}

/*
 * Select another cathedral from our remembrances, if possible.
 */
int
vigil_call_cathedral_pick(struct vigil_cathedral *cat)
{
	u_int32_t		idx;
	struct sockaddr_in	sin;
	int			count, attempts;

	PRECOND(cat != NULL);

	if (!running)
		return (-1);

	count = 0;
	while (vigil_remembrance_address(&remembrance, count, &sin) != -1)
		count++;

	if (resolved)
		count++;

	if (count == 0)
		return (-1);

	for (attempts = 0; attempts < KYRKA_CATHEDRALS_MAX; attempts++) {
		nyfe_random_bytes(&idx, sizeof(idx));
		idx = idx % count;

		if (vigil_remembrance_address(&remembrance, idx, &sin) == -1)
			memcpy(&sin, &configured, sizeof(sin));

		if (count == 1)
			break;

		if (!vigil_inet_match(&sin, &cat->addr))
			break;
	}

	cat->alive = 0;
	cat->last = vigil->now;
	cat->timeout = VIGIL_CATHEDRAL_TIMEOUT_INIT;

	memcpy(&cat->addr, &sin, sizeof(sin));

	return (0);
}

/*
 * Append our current selected cathedrals and their rx/tx statistics
 * to the given JSON buffer.
 */
void
vigil_call_liturgies(struct vigil_buf *buf)
{
	PRECOND(buf != NULL);

	vigil_buf_appendf(buf, ",\"liturgies\":[");

	if (running) {
		vigil_status_liturgy(buf, "signaling", signaling.group,
		    &signaling.cathedral, signaling.rx, signaling.tx);
		vigil_buf_appendf(buf, ",");
		vigil_status_liturgy(buf, "discovery", discovery.group,
		    &discovery.cathedral, discovery.rx, discovery.tx);
	}

	vigil_buf_appendf(buf, "]");
}

/*
 * Start signaling the peer we want to reach.
 */
void
vigil_call_dial(u_int8_t peer)
{
	u_int8_t	id;

	PRECOND(peer > 0 && peer < KYRKA_PEERS_PER_FLOCK);

	vigil_call_hangup();

	do {
		id = vigil_atomic_add(&serial, 1) + 1;
	} while (id == 0);

	vigil_atomic_write(&wanted[peer], id);
}

/*
 * Stop signaling all peers.
 */
void
vigil_call_hangup(void)
{
	int		idx;

	for (idx = 0; idx < KYRKA_PEERS_PER_FLOCK; idx++)
		vigil_atomic_write(&wanted[idx], 0);
}

/*
 * A call with the given peer is over, stop signaling it and ignore the
 * call it signals us with until it is gone.
 */
void
vigil_call_done(u_int8_t peer)
{
	PRECOND(peer > 0 && peer < KYRKA_PEERS_PER_FLOCK);

	vigil_atomic_write(&wanted[peer], 0);
	vigil_atomic_write(&done[peer], vigil_atomic_read(&present[peer]));
}

/*
 * Returns the current call state for the given peer.
 */
int
vigil_call_peer(u_int8_t peer)
{
	int		state;
	u_int8_t	call;

	PRECOND(peer > 0 && peer < KYRKA_PEERS_PER_FLOCK);

	state = 0;

	if (vigil_atomic_read(&wanted[peer]))
		state |= VIGIL_CALL_PEER_WANTED;

	call = vigil_atomic_read(&present[peer]);
	if (call != 0) {
		state |= VIGIL_CALL_PEER_PRESENT;
		if (call == vigil_atomic_read(&done[peer]))
			state |= VIGIL_CALL_PEER_DONE;
	}

	if (vigil_atomic_read(&online[peer]))
		state |= VIGIL_CALL_PEER_ONLINE;

	return (state);
}

/*
 * Helper function to attempt to resolve a cathedral.
 */
static int
call_cathedral_resolve(void)
{
	PRECOND(vigil->gospel != NULL);

	next_resolve = vigil->now + CALL_RESOLVE_RETRY;

	if (vigil_resolve(vigil->gospel->cathedral_host,
	    vigil->gospel->cathedral_port, &configured) == -1)
		return (-1);

	vigil_log(LOG_INFO, "gospel cathedral is %s",
	    vigil_address(&configured));

	return (0);
}

/*
 * Helper function to setup a liturgy.
 */
static int
call_liturgy_init(struct call_liturgy *lit, int sig)
{
	struct kyrka_cathedral_cfg	cfg;
	struct vigil_config		*gospel;

	PRECOND(lit != NULL);
	PRECOND(sig == 0 || sig == 1);
	PRECOND(vigil->gospel != NULL);

	gospel = vigil->gospel;

	memset(lit, 0, sizeof(*lit));
	lit->signaling = sig;

	if ((lit->io.fd = vigil_socket_udp()) == -1)
		return (-1);

	lit->io.handle = call_io_event;
	vigil_io_schedule(&lit->io);

	if ((lit->ctx = kyrka_ctx_alloc(call_kyrka_event, lit)) == NULL)
		fatal("failed to create calls context");

	memset(&cfg, 0, sizeof(cfg));

	cfg.udata = lit;
	cfg.send = call_kyrka_send;
	cfg.tunnel = gospel->spi & 0xff;
	cfg.identity = gospel->cathedral_id;

	cfg.flock_src = gospel->cathedral_flock | VIGIL_CALL_FLOCK_DOMAIN;
	cfg.flock_dst = cfg.flock_src;

	if (sig) {
		cfg.remembrance = 1;
		cfg.group = gospel->liturgy_group;
	} else {
		cfg.group = gospel->liturgy_group + 1;
	}

	lit->group = cfg.group;

	if (kyrka_cathedral_config(lit->ctx, &cfg) == -1) {
		fatal("failed to configure cathedral: %d",
		    kyrka_last_error(lit->ctx));
	}

	if (kyrka_cathedral_secret_load(lit->ctx, gospel->cathedral_secret,
	    sizeof(gospel->cathedral_secret)) == -1) {
		fatal("kyrka_cathedral_secret_load: %d",
		    kyrka_last_error(lit->ctx));
	}

	if (kyrka_cathedral_cosk_load(lit->ctx, gospel->cathedral_cosk,
	    sizeof(gospel->cathedral_cosk)) == -1) {
		fatal("kyrka_cathedral_cosk_load: %d",
		    kyrka_last_error(lit->ctx));
	}

	if (gospel->flags & VIGIL_CONFIG_FLAG_SHROUD) {
		if (kyrka_shroud_enable(lit->ctx) == -1) {
			fatal("failed to enable shroud: %d",
			    kyrka_last_error(lit->ctx));
		}
	}

	if (kyrka_mtu_size(lit->ctx, gospel->mtu) == -1) {
		fatal("failed to set libkyrka mtu size: %d",
		    kyrka_last_error(lit->ctx));
	}

	return (0);
}

/*
 * Assign a cathedral to the given liturgy, or select another one if
 * it timed out.
 */
static void
call_liturgy_check(struct call_liturgy *lit)
{
	int		idx;

	PRECOND(lit != NULL);

	if (lit->cathedral.addr.sin_port == 0) {
		if (vigil_call_cathedral_pick(&lit->cathedral) == -1)
			return;

		if (lit->signaling)
			lit->cathedral.timeout = VIGIL_CALL_TIMEOUT_INIT;

		vigil_log(LOG_INFO, "[calls]: liturgy %04x via %s",
		    lit->group, vigil_address(&lit->cathedral.addr));
		return;
	}

	if ((vigil->now - lit->cathedral.last) <= lit->cathedral.timeout)
		return;

	if (lit->cathedral.alive) {
		vigil_log(LOG_NOTICE, "[calls]: liturgy %04x timed out",
		    lit->group);
	}

	lit->cathedral.alive = 0;

	if (!lit->signaling) {
		for (idx = 0; idx < KYRKA_PEERS_PER_FLOCK; idx++)
			vigil_atomic_write(&online[idx], 0);
	}

	if (vigil_call_cathedral_pick(&lit->cathedral) == -1) {
		lit->cathedral.last = vigil->now;
		return;
	}

	if (lit->signaling)
		lit->cathedral.timeout = VIGIL_CALL_TIMEOUT_INIT;

	vigil_log(LOG_NOTICE, "[calls]: liturgy %04x switching to %s",
	    lit->group, vigil_address(&lit->cathedral.addr));
}

/*
 * Cleanup any resources allocated for a liturgy.
 */
static void
call_liturgy_cleanup(struct call_liturgy *lit)
{
	PRECOND(lit != NULL);

	if (lit->ctx == NULL)
		return;

	vigil_io_unschedule(&lit->io);
	vigil_socket_close(lit->io.fd);

	kyrka_ctx_free(lit->ctx);
	lit->ctx = NULL;
}

/*
 * Reopen underlying sockets for the given liturgy.
 */
static void
call_liturgy_rebind(struct call_liturgy *lit)
{
	int		fd;

	PRECOND(lit != NULL);
	PRECOND(lit->ctx != NULL);

	if ((fd = vigil_socket_udp()) == -1)
		return;

	vigil_io_unschedule(&lit->io);
	vigil_socket_close(lit->io.fd);

	lit->io.fd = fd;
	vigil_io_schedule(&lit->io);
}

/*
 * Callback for when an event occurred on one of our sockets.
 */
static void
call_io_event(void *udata)
{
	struct call_liturgy	*lit;

	PRECOND(udata == &signaling.io || udata == &discovery.io);

	lit = udata;

	if (lit->io.flags & VIGIL_IO_READABLE)
		call_liturgy_read(lit);
}

/*
 * Read incoming packets and hand them over to our kyrka context.
 */
static void
call_liturgy_read(struct call_liturgy *lit)
{
	int			idx;
	size_t			len;
	ssize_t			ret;
	struct kyrka_packet	pkt;
	u_int8_t		*ptr;

	PRECOND(lit != NULL);
	PRECOND(lit->ctx != NULL);

	for (idx = 0; idx < VIGIL_IO_BATCH; idx++) {
		ptr = kyrka_packet_recvbuf(lit->ctx, &pkt, &len);
		if (ptr == NULL)
			fatal("failed to get calls receive buffer");

		if ((ret = read(lit->io.fd, ptr, len)) == -1) {
			if (errno == EINTR)
				continue;
			if (errno == EWOULDBLOCK || errno == EAGAIN) {
				lit->io.flags &= ~VIGIL_IO_READABLE;
				break;
			}
			vigil_log(LOG_NOTICE, "[calls]: read: %s", errno_s);
			lit->io.flags &= ~VIGIL_IO_READABLE;
			break;
		}

		if (ret == 0)
			continue;

		lit->rx += ret;
		vigil->gospel_rx += ret;

		pkt.length = ret;
		pkt.shroud = KYRKA_PACKET_SHROUD_CATHEDRAL;

		if (kyrka_purgatory_input(lit->ctx, &pkt) == -1) {
			vigil_log(LOG_NOTICE, "[call] purgatory input: %d",
			    kyrka_last_error(lit->ctx));
		}
	}
}

/*
 * Callback from libkyrka when an event occurred on one of our liturgies.
 */
static void
call_kyrka_event(KYRKA *ctx, union kyrka_event *evt, void *udata)
{
	int			idx;
	struct call_liturgy	*lit;
	u_int8_t		local, state;

	PRECOND(ctx != NULL);
	PRECOND(evt != NULL);
	PRECOND(udata != NULL);

	lit = udata;
	local = vigil->gospel->spi & 0xff;

	if (evt->type == KYRKA_EVENT_LITURGY_RECEIVED ||
	    evt->type == KYRKA_EVENT_REMEMBRANCE_RECEIVED) {
		lit->cathedral.last = vigil->now;
		lit->cathedral.timeout = VIGIL_CATHEDRAL_TIMEOUT;

		if (lit->cathedral.alive == 0) {
			lit->cathedral.alive = 1;
			vigil_log(LOG_INFO,
			    "[calls]: liturgy %04x cathedral %s is alive",
			    lit->group, vigil_address(&lit->cathedral.addr));
		}
	}

	switch (evt->type) {
	case KYRKA_EVENT_LITURGY_RECEIVED:
		for (idx = 1; idx < KYRKA_PEERS_PER_FLOCK; idx++) {
			if (idx == local)
				continue;

			state = evt->liturgy.peers[idx];

			if (!lit->signaling) {
				vigil_atomic_write(&online[idx], state ? 1 : 0);
				continue;
			}

			/*
			 * The cathedral pushes us each peer that signals us
			 * on its own with the call it signals us for, peers
			 * expire in vigil_call_update().
			 */
			if (state == 0)
				continue;

			pushed[idx] = vigil->now;

			if (vigil_atomic_read(&present[idx]) == state)
				continue;

			if (!vigil_atomic_read(&wanted[idx])) {
				vigil_log(LOG_INFO,
				    "[calls]: %02x is calling us (%02x)",
				    idx, state);
			}

			vigil_atomic_write(&present[idx], state);
		}
		break;
	case KYRKA_EVENT_REMEMBRANCE_RECEIVED:
		if (lit->signaling)
			vigil_remembrance_save(&remembrance, &evt->remembrance);
		break;
	case KYRKA_EVENT_LOGMSG:
		vigil_log(LOG_INFO, "[calls]: libkyrka: %s", evt->logmsg.log);
		break;
	default:
		vigil_log(LOG_NOTICE,
		    "[calls]: received unexpected event %u", evt->type);
		break;
	}
}

/*
 * Callback from libkyrka when one of our liturgies has data for
 * the cathedral.
 */
static void
call_kyrka_send(struct kyrka_packet *pkt, u_int64_t magic, void *udata)
{
	size_t			len;
	u_int8_t		*ptr;
	struct call_liturgy	*lit;

	PRECOND(pkt != NULL);
	PRECOND(udata != NULL);

	lit = udata;

	if (lit->cathedral.addr.sin_port == 0)
		return;

	ptr = kyrka_packet_sendbuf(lit->ctx, pkt, &len);
	if (ptr == NULL)
		fatal("failed to get calls sendbuf");

	if (sendto(lit->io.fd, ptr, len, 0,
	    (const struct sockaddr *)&lit->cathedral.addr,
	    sizeof(lit->cathedral.addr)) == -1) {
		if (errno != EAGAIN && errno != EWOULDBLOCK &&
		    !vigil_socket_unreachable(errno, 1))
			vigil_log(LOG_NOTICE, "calls sendto: %s", errno_s);
		return;
	}

	lit->tx += len;
	vigil->gospel_tx += len;
}
