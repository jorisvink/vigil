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

#include <inttypes.h>
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
	u_int8_t		slot;
	u_int16_t		group;
	int			signaling;

	u_int64_t		rx;
	u_int64_t		tx;
	struct vigil_cathedral	cathedral;

	KYRKA			*ctx;
};

/*
 * The calls of one of our Gospel configurations.
 */
struct call_gospel {
	int				running;

	struct call_liturgy		signaling;
	struct call_liturgy		discovery;

	int				resolved;
	u_int64_t			next_resolve;
	struct sockaddr_in		configured;

	struct vigil_remembrance	remembrance;

	u_int64_t			last_notify;
	u_int64_t			next_notify;

	u_int64_t			pushed[KYRKA_PEERS_PER_FLOCK];
	u_int8_t			sent[KYRKA_PEERS_PER_FLOCK];
};

/*
 * The peers of one of our Gospel configurations that we want to call and
 * with what call, from any thread. The peers the cathedral told us want to
 * call us and with what call and those that are around to be called.
 *
 * Done holds the call we had with a peer that the cathedral still shows us,
 * we don't report it as calling until it calls with another one or it is
 * gone.
 */
struct call_marks {
	volatile u_int8_t	wanted[KYRKA_PEERS_PER_FLOCK];
	volatile u_int8_t	present[KYRKA_PEERS_PER_FLOCK];
	volatile u_int8_t	online[KYRKA_PEERS_PER_FLOCK];
	volatile u_int8_t	done[KYRKA_PEERS_PER_FLOCK];
};

static void	call_update(u_int8_t);
static void	call_io_event(void *);
static int	call_cathedral_resolve(u_int8_t);

static void	call_liturgy_read(struct call_liturgy *);
static void	call_liturgy_check(struct call_liturgy *);
static void	call_liturgy_rebind(struct call_liturgy *);
static void	call_liturgy_cleanup(struct call_liturgy *);
static int	call_liturgy_init(struct call_liturgy *, u_int8_t, int);

static void	call_kyrka_event(KYRKA *, union kyrka_event *, void *);
static void	call_kyrka_send(struct kyrka_packet *, u_int64_t, void *);

/* The calls of each of our Gospel configurations. */
static struct call_gospel		gospels[VIGIL_GOSPEL_MAX];

/*
 * Each call we make gets its own number that we signal to the peer, so it
 * can tell a new call from the one it just had with us.
 */
static volatile u_int8_t		serial;

/* What we know about the peers of each of our Gospel configurations. */
static struct call_marks		marks[VIGIL_GOSPEL_MAX];

/*
 * Set up the calls for the Gospel configuration in the given slot by
 * setting up its liturgies.
 */
void
vigil_call_attach(u_int8_t slot)
{
	u_int8_t		id;
	int			idx;
	struct vigil_config	*cfg;
	struct call_gospel	*gospel;

	PRECOND(slot < VIGIL_GOSPEL_MAX);
	PRECOND(vigil->gospels[slot] != NULL);
	PRECOND(vigil->datadir != NULL);

	cfg = vigil->gospels[slot];
	gospel = &gospels[slot];

	VERIFY(gospel->running == 0);
	VERIFY(cfg->mode == VIGIL_MODE_GOSPEL);

	memset(gospel, 0, sizeof(*gospel));

	vigil_remembrance_init(&gospel->remembrance, vigil->datadir,
	    cfg->cathedral_flock | VIGIL_CALL_FLOCK_DOMAIN);

	for (idx = 0; idx < KYRKA_PEERS_PER_FLOCK; idx++) {
		vigil_atomic_write(&marks[slot].wanted[idx], 0);
		vigil_atomic_write(&marks[slot].done[idx], 0);
		vigil_atomic_write(&marks[slot].online[idx], 0);
		vigil_atomic_write(&marks[slot].present[idx], 0);
	}

	if (call_liturgy_init(&gospel->signaling, slot, 1) == -1) {
		vigil_log(LOG_NOTICE, "calls are not available");
		call_liturgy_cleanup(&gospel->signaling);
		vigil_remembrance_cleanup(&gospel->remembrance);
		return;
	}

	if (call_liturgy_init(&gospel->discovery, slot, 0) == -1) {
		vigil_log(LOG_NOTICE, "calls are not available");
		call_liturgy_cleanup(&gospel->signaling);
		call_liturgy_cleanup(&gospel->discovery);
		vigil_remembrance_cleanup(&gospel->remembrance);
		return;
	}

	nyfe_random_bytes(&id, sizeof(id));
	vigil_atomic_write(&serial, id);

	gospel->running = 1;

	if (call_cathedral_resolve(slot) != -1)
		gospel->resolved = 1;

	vigil_log(LOG_INFO, "calls running for %" PRIx64 " group %04x",
	    cfg->cathedral_flock, cfg->liturgy_group);
}

/*
 * Cleanup any resources allocated for the calls in the given slot.
 */
void
vigil_call_detach(u_int8_t slot)
{
	int			idx;
	struct call_gospel	*gospel;

	PRECOND(slot < VIGIL_GOSPEL_MAX);

	gospel = &gospels[slot];

	if (!gospel->running)
		return;

	call_liturgy_cleanup(&gospel->signaling);
	call_liturgy_cleanup(&gospel->discovery);
	vigil_remembrance_cleanup(&gospel->remembrance);

	for (idx = 0; idx < KYRKA_PEERS_PER_FLOCK; idx++) {
		vigil_atomic_write(&marks[slot].wanted[idx], 0);
		vigil_atomic_write(&marks[slot].online[idx], 0);
		vigil_atomic_write(&marks[slot].present[idx], 0);
	}

	gospel->running = 0;
	gospel->resolved = 0;
}

/*
 * Update the cathedrals of all our Gospel configurations with what we
 * want right now.
 */
void
vigil_call_update(void)
{
	u_int8_t	slot;

	for (slot = 0; slot < VIGIL_GOSPEL_MAX; slot++) {
		if (gospels[slot].running)
			call_update(slot);
	}
}

/*
 * Give all liturgies a new socket and update the cathedrals right away,
 * the network we were on changed.
 */
void
vigil_call_rebind(void)
{
	u_int8_t	slot;

	for (slot = 0; slot < VIGIL_GOSPEL_MAX; slot++) {
		if (!gospels[slot].running)
			continue;

		call_liturgy_rebind(&gospels[slot].signaling);
		call_liturgy_rebind(&gospels[slot].discovery);

		gospels[slot].next_notify = 0;
	}
}

/*
 * Update all liturgies immediately on the next update.
 */
void
vigil_call_poke(void)
{
	u_int8_t	slot;

	for (slot = 0; slot < VIGIL_GOSPEL_MAX; slot++)
		gospels[slot].next_notify = 0;
}

/*
 * Returns the current signaling cathedral for the given slot.
 */
struct sockaddr_in *
vigil_call_cathedral(u_int8_t slot)
{
	struct call_gospel	*gospel;

	PRECOND(slot < VIGIL_GOSPEL_MAX);

	gospel = &gospels[slot];

	if (!gospel->running)
		return (NULL);

	if (gospel->signaling.cathedral.addr.sin_port == 0)
		return (NULL);

	return (&gospel->signaling.cathedral.addr);
}

/*
 * Returns 1 if the signaling cathedral for the given slot is alive.
 */
int
vigil_call_alive(u_int8_t slot)
{
	PRECOND(slot < VIGIL_GOSPEL_MAX);

	if (!gospels[slot].running)
		return (0);

	return (gospels[slot].signaling.cathedral.alive);
}

/*
 * Select another cathedral from the remembrances of the given slot,
 * if possible.
 */
int
vigil_call_cathedral_pick(u_int8_t slot, struct vigil_cathedral *cat)
{
	u_int32_t		idx;
	struct sockaddr_in	sin;
	struct call_gospel	*gospel;
	int			count, attempts;

	PRECOND(slot < VIGIL_GOSPEL_MAX);
	PRECOND(cat != NULL);

	gospel = &gospels[slot];

	if (!gospel->running)
		return (-1);

	count = 0;
	while (vigil_remembrance_address(&gospel->remembrance,
	    count, &sin) != -1)
		count++;

	if (gospel->resolved)
		count++;

	if (count == 0)
		return (-1);

	for (attempts = 0; attempts < KYRKA_CATHEDRALS_MAX; attempts++) {
		nyfe_random_bytes(&idx, sizeof(idx));
		idx = idx % count;

		if (vigil_remembrance_address(&gospel->remembrance,
		    idx, &sin) == -1)
			memcpy(&sin, &gospel->configured, sizeof(sin));

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
 * Append the selected cathedrals of the liturgies in the given slot and
 * their rx/tx statistics to the given JSON buffer.
 */
void
vigil_call_liturgies(u_int8_t slot, struct vigil_buf *buf)
{
	struct call_gospel	*gospel;

	PRECOND(slot < VIGIL_GOSPEL_MAX);
	PRECOND(buf != NULL);

	gospel = &gospels[slot];
	vigil_buf_appendf(buf, ",\"liturgies\":[");

	if (gospel->running) {
		vigil_status_liturgy(buf, "signaling",
		    gospel->signaling.group, &gospel->signaling.cathedral,
		    gospel->signaling.rx, gospel->signaling.tx);
		vigil_buf_appendf(buf, ",");
		vigil_status_liturgy(buf, "discovery",
		    gospel->discovery.group, &gospel->discovery.cathedral,
		    gospel->discovery.rx, gospel->discovery.tx);
	}

	vigil_buf_appendf(buf, "]");
}

/*
 * Start signaling the peer we want to reach in the given flock, we stop
 * signaling everyone else. May be called from any thread.
 */
void
vigil_call_dial(u_int64_t flock, u_int8_t peer)
{
	u_int8_t	slot;
	u_int8_t	id;

	PRECOND(peer > 0 && peer < KYRKA_PEERS_PER_FLOCK);

	vigil_call_hangup();

	if (vigil_gospel_slot(flock, &slot) == -1)
		return;

	do {
		id = vigil_atomic_add(&serial, 1) + 1;
	} while (id == 0);

	vigil_atomic_write(&marks[slot].wanted[peer], id);
	vigil_gospel_release();
}

/*
 * Stop signaling all peers in all flocks.
 */
void
vigil_call_hangup(void)
{
	int		idx;
	u_int8_t	slot;

	for (slot = 0; slot < VIGIL_GOSPEL_MAX; slot++) {
		for (idx = 0; idx < KYRKA_PEERS_PER_FLOCK; idx++)
			vigil_atomic_write(&marks[slot].wanted[idx], 0);
	}
}

/*
 * A call with the given peer in the given flock is over, stop signaling it
 * and ignore the call it signals us with until it is gone.
 */
void
vigil_call_done(u_int64_t flock, u_int8_t peer)
{
	u_int8_t	slot;

	PRECOND(peer > 0 && peer < KYRKA_PEERS_PER_FLOCK);

	if (vigil_gospel_slot(flock, &slot) == -1)
		return;

	vigil_atomic_write(&marks[slot].wanted[peer], 0);
	vigil_atomic_write(&marks[slot].done[peer],
	    vigil_atomic_read(&marks[slot].present[peer]));

	vigil_gospel_release();
}

/*
 * Returns the current call state for the given peer in the given slot.
 */
int
vigil_call_peer(u_int8_t slot, u_int8_t peer)
{
	int		state;
	u_int8_t	call;

	PRECOND(slot < VIGIL_GOSPEL_MAX);
	PRECOND(peer > 0 && peer < KYRKA_PEERS_PER_FLOCK);

	state = 0;

	if (vigil_atomic_read(&marks[slot].wanted[peer]))
		state |= VIGIL_CALL_PEER_WANTED;

	call = vigil_atomic_read(&marks[slot].present[peer]);
	if (call != 0) {
		state |= VIGIL_CALL_PEER_PRESENT;
		if (call == vigil_atomic_read(&marks[slot].done[peer]))
			state |= VIGIL_CALL_PEER_DONE;
	}

	if (vigil_atomic_read(&marks[slot].online[peer]))
		state |= VIGIL_CALL_PEER_ONLINE;

	return (state);
}

/*
 * Update the cathedral of the given slot with what we want right now.
 */
static void
call_update(u_int8_t slot)
{
	int			idx;
	struct call_gospel	*gospel;
	u_int8_t		peers[KYRKA_PEERS_PER_FLOCK];

	PRECOND(slot < VIGIL_GOSPEL_MAX);

	gospel = &gospels[slot];

	if (gospel->resolved == 0 && vigil->now >= gospel->next_resolve) {
		if (call_cathedral_resolve(slot) != -1)
			gospel->resolved = 1;
	}

	for (idx = 1; idx < KYRKA_PEERS_PER_FLOCK; idx++) {
		if (!vigil_atomic_read(&marks[slot].present[idx]))
			continue;

		if ((vigil->now - gospel->pushed[idx]) <
		    VIGIL_CALL_PRESENT_EXPIRE)
			continue;

		vigil_atomic_write(&marks[slot].done[idx], 0);
		vigil_atomic_write(&marks[slot].present[idx], 0);
	}

	for (idx = 0; idx < KYRKA_PEERS_PER_FLOCK; idx++)
		peers[idx] = vigil_atomic_read(&marks[slot].wanted[idx]);

	if (gospel->next_notify > 0 && vigil->now < gospel->next_notify) {
		if (!memcmp(peers, gospel->sent, sizeof(peers)))
			return;
		if ((vigil->now - gospel->last_notify) < VIGIL_CALL_NOTIFY_MIN)
			return;
	}

	memcpy(gospel->sent, peers, sizeof(gospel->sent));
	gospel->last_notify = vigil->now;
	gospel->next_notify = vigil->now + VIGIL_CALL_NOTIFY;

	call_liturgy_check(&gospel->signaling);
	call_liturgy_check(&gospel->discovery);

	if (kyrka_cathedral_liturgy(gospel->signaling.ctx,
	    peers, sizeof(peers)) == -1) {
		vigil_log(LOG_NOTICE, "signaling kyrka_cathedral_liturgy: %d",
		    kyrka_last_error(gospel->signaling.ctx));
	}

	if (kyrka_cathedral_liturgy(gospel->discovery.ctx, NULL, 0) == -1) {
		vigil_log(LOG_NOTICE, "discovery kyrka_cathedral_liturgy: %d",
		    kyrka_last_error(gospel->discovery.ctx));
	}
}

/*
 * Helper function to attempt to resolve the cathedral of the given slot.
 */
static int
call_cathedral_resolve(u_int8_t slot)
{
	struct call_gospel	*gospel;
	struct vigil_config	*cfg;

	PRECOND(slot < VIGIL_GOSPEL_MAX);
	PRECOND(vigil->gospels[slot] != NULL);

	cfg = vigil->gospels[slot];
	gospel = &gospels[slot];

	gospel->next_resolve = vigil->now + CALL_RESOLVE_RETRY;

	if (vigil_resolve(cfg->cathedral_host, cfg->cathedral_port,
	    &gospel->configured) == -1)
		return (-1);

	vigil_log(LOG_INFO, "gospel cathedral for %" PRIx64 " is %s",
	    cfg->cathedral_flock, vigil_address(&gospel->configured));

	return (0);
}

/*
 * Helper function to setup a liturgy for the given slot.
 */
static int
call_liturgy_init(struct call_liturgy *lit, u_int8_t slot, int sig)
{
	struct kyrka_cathedral_cfg	cfg;
	struct vigil_config		*gospel;

	PRECOND(lit != NULL);
	PRECOND(slot < VIGIL_GOSPEL_MAX);
	PRECOND(sig == 0 || sig == 1);
	PRECOND(vigil->gospels[slot] != NULL);

	gospel = vigil->gospels[slot];

	memset(lit, 0, sizeof(*lit));
	lit->slot = slot;
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

	vigil_kyrka_cathedral(lit->ctx, &cfg, gospel, 0, gospel->mtu);

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
		if (vigil_call_cathedral_pick(lit->slot, &lit->cathedral) == -1)
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
			vigil_atomic_write(&marks[lit->slot].online[idx], 0);
	}

	if (vigil_call_cathedral_pick(lit->slot, &lit->cathedral) == -1) {
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

	PRECOND(udata != NULL);

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
	u_int8_t		slot;
	struct call_marks	*mark;
	struct call_liturgy	*lit;
	struct call_gospel	*gospel;
	u_int8_t		local, state;

	PRECOND(ctx != NULL);
	PRECOND(evt != NULL);
	PRECOND(udata != NULL);

	lit = udata;
	slot = lit->slot;
	mark = &marks[slot];
	gospel = &gospels[slot];
	local = vigil->gospels[slot]->spi & 0xff;

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
				vigil_atomic_write(&mark->online[idx],
				    state ? 1 : 0);
				continue;
			}

			/*
			 * The cathedral pushes us each peer that signals us
			 * on its own with the call it signals us for, peers
			 * expire in vigil_call_update().
			 */
			if (state == 0)
				continue;

			if (!vigil_chat_accepted(slot, idx) &&
			    !vigil_atomic_read(&mark->wanted[idx]))
				continue;

			gospel->pushed[idx] = vigil->now;

			if (vigil_atomic_read(&mark->present[idx]) == state)
				continue;

			if (!vigil_atomic_read(&mark->wanted[idx])) {
				vigil_log(LOG_INFO,
				    "[calls]: %02x is calling us (%02x)",
				    idx, state);
			}

			vigil_atomic_write(&mark->present[idx], state);
		}
		break;
	case KYRKA_EVENT_REMEMBRANCE_RECEIVED:
		if (lit->signaling) {
			vigil_remembrance_save(&gospel->remembrance,
			    &evt->remembrance);
		}
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
