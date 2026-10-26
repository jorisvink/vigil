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

static void			peer_create(u_int8_t);
static int			peer_socket(u_int16_t *);
static void			peer_free(struct vigil_peer *);
static void			peer_relay(struct vigil_peer *);
static void			peer_delete(struct vigil_peer *);
static u_int32_t		peer_shroud(struct vigil_peer *);
static void			peer_holepunch(struct vigil_peer *);
static void			peer_alive_check(struct vigil_peer *);
static struct vigil_peer	*peer_lookup(const u_int8_t *, size_t);
static void			peer_heaven_input(struct kyrka_packet *,
				    u_int64_t, void *);
static void			peer_purgatory_input(struct kyrka_packet *,
				    u_int64_t, void *);
static int			peer_mtu_exceeded(struct vigil_peer *,
				    const u_int8_t *, size_t);

static void	peer_io_event(void *);
static void	peer_io_read(struct vigil_peer *);

static void	peer_cathedral_alive(struct vigil_peer *);
static void	peer_cathedral_check(struct vigil_peer *);
static void	peer_cathedral_update(struct vigil_peer *);
static void	peer_cathedral_config(struct vigil_peer *);

static void	peer_p2p_check(struct vigil_peer *);
static void	peer_p2p_failed(struct vigil_peer *);

static void	peer_kyrka_event(KYRKA *, union kyrka_event *, void *);
static void	peer_kyrka_send(struct kyrka_packet *, u_int64_t, void *);

/* Our list of active peers. */
static struct vigil_peer_list		peers;

/* The sender of our current received packet. */
static struct sockaddr_in		sender;

/* The next time we should update all peers. */
static u_int64_t			next_update;

/*
 * Initialise the peer subsystem.
 */
void
vigil_peer_init(void)
{
	next_update = 0;
	LIST_INIT(&peers);
}

/*
 * Returns the list of peers, used by the status code.
 */
struct vigil_peer_list *
vigil_peer_list(void)
{
	return (&peers);
}

/*
 * Bring a peer up or down depending on the state given.
 */
void
vigil_peer_state(u_int8_t id, u_int8_t state)
{
	struct vigil_peer	*peer;

	PRECOND(id >= 1);

	if (state != 0 && state != 1) {
		vigil_log(LOG_NOTICE,
		    "[cathedral] sent wrong state (%u) for %02x", state, id);
		return;
	}

	LIST_FOREACH(peer, &peers, list) {
		if (peer->id == id)
			break;
	}

	if (peer == NULL && state == 1)
		peer_create(id);
	else if (peer != NULL && state == 0)
		peer_delete(peer);
	else if (peer != NULL && state == 1)
		peer->flags &= ~VIGIL_PEER_FLAG_REMOVE;
}

/*
 * Run the peer housekeeping every second.
 */
void
vigil_peer_update(void)
{
	struct vigil_peer	*peer;

	if (next_update > 0 && vigil->now < next_update)
		return;

	next_update = vigil->now + VIGIL_PEER_UPDATE_INTERVAL;

	LIST_FOREACH(peer, &peers, list) {
		if (vigil->config->mode != VIGIL_MODE_TUNNEL &&
		    (vigil->config->flags & VIGIL_CONFIG_FLAG_REMEMBRANCE))
			peer_cathedral_check(peer);

		if (vigil->config->mode != VIGIL_MODE_TUNNEL)
			peer_p2p_check(peer);

		peer_alive_check(peer);

		if (kyrka_key_manage(peer->ctx) == -1 &&
		    kyrka_last_error(peer->ctx) != KYRKA_ERROR_NO_SECRET) {
			vigil_log(LOG_NOTICE,
			    "[peer=%02x] kyrka_key_manage: %d",
			    peer->id, kyrka_last_error(peer->ctx));
		}

		if (vigil->config->mode != VIGIL_MODE_TUNNEL)
			peer_cathedral_update(peer);

		if (vigil->config->flags & VIGIL_CONFIG_FLAG_TIER6)
			vigil_ether_update(peer);
		else
			vigil_grace_update(peer);
	}
}

/*
 * Send a packet that was read from our tunnel interface to the peer
 * that should receive it.
 *
 * In tunnel and cathedral mode there is only a single peer, in liturgy
 * mode the destination address tells us what peer it is meant for.
 */
void
vigil_peer_output(const u_int8_t *data, size_t len)
{
	struct kyrka_packet	pkt;
	struct in_addr		src;
	u_int8_t		*ptr;
	struct vigil_peer	*peer;
	size_t			maxlen;

	PRECOND(data != NULL);

	switch (vigil_inet_version(data, len)) {
	case 4:
		break;
	case 6:
		if (vigil->config->flags & VIGIL_CONFIG_FLAG_BLOCK_IPV6)
			return;
		if (vigil->config->mode == VIGIL_MODE_LITURGY)
			return;
		break;
	default:
		return;
	}

	if (vigil->config->flags & VIGIL_CONFIG_FLAG_TIER6) {
		vigil_ether_output(data, len);
		return;
	}

	if ((peer = peer_lookup(data, len)) == NULL)
		return;

	if (peer->mtu != 0 && len > peer->mtu) {
		if (peer_mtu_exceeded(peer, data, len) == 0)
			return;
	}

	if ((ptr = kyrka_packet_databuf(peer->ctx, &pkt, &maxlen)) == NULL)
		fatal("failed to get peer data buffer");

	if (len > maxlen) {
		vigil_log(LOG_NOTICE,
		    "[peer=%02x] packet of %zu too large (max:%zu)",
		    peer->id, len, maxlen);
		return;
	}

	memcpy(ptr, data, len);
	pkt.length = len;
	pkt.shroud = peer_shroud(peer);

	if (vigil->config->mode == VIGIL_MODE_LITURGY) {
		vigil_inet_liturgy(&src, &vigil->config->liturgy_prefix,
		    vigil->config->spi & 0xff, peer->id);

		if (vigil_inet_rewrite_src(ptr, len, &src) == -1)
			return;
	}

	if (kyrka_heaven_input(peer->ctx, &pkt) == -1) {
		if (kyrka_last_error(peer->ctx) != KYRKA_ERROR_NO_TX_KEY) {
			vigil_log(LOG_NOTICE,
			    "[peer=%02x] kyrka_heaven_input: %d (%zu)",
			    peer->id, kyrka_last_error(peer->ctx), len);
		}

		return;
	}

	peer->tx_pkts++;
}

/*
 * Prune all peers marked for removal.
 */
void
vigil_peer_prune(void)
{
	struct vigil_peer	*peer, *next;

	for (peer = LIST_FIRST(&peers); peer != NULL; peer = next) {
		next = LIST_NEXT(peer, list);

		if (!(peer->flags & VIGIL_PEER_FLAG_REMOVE))
			continue;

		vigil_log(LOG_INFO, "[peer=%02x] tunnel removed", peer->id);
		peer_free(peer);
	}
}

/*
 * Remove all peers, called when we are shutting down.
 */
void
vigil_peer_cleanup(void)
{
	struct vigil_peer	*peer;

	while ((peer = LIST_FIRST(&peers)) != NULL)
		peer_free(peer);
}

/*
 * Create a new tunnel for the given peer and schedule it onto
 * our internal event loop.
 */
static void
peer_create(u_int8_t id)
{
	struct vigil_peer		*peer;

	PRECOND(id >= 1);

	peer = calloc(1, sizeof(*peer));
	if (peer == NULL)
		fatal("calloc: peer failed");

	LIST_INIT(&peer->macs);

	peer->id = id;
	peer->alive = vigil->now;
	peer->io.handle = peer_io_event;

	if (vigil->config->flags & VIGIL_CONFIG_FLAG_TIER6)
		vigil_ether_init(peer);
	else
		vigil_grace_init(peer);

	peer->io.fd = peer_socket(&peer->port);
	if (peer->io.fd == -1) {
		free(peer);
		if (vigil->config->mode != VIGIL_MODE_LITURGY)
			vigil->flags &= ~VIGIL_FLAG_RUNNING;
		return;
	}

	vigil_io_schedule(&peer->io);

	if ((peer->ctx = kyrka_ctx_alloc(peer_kyrka_event, peer)) == NULL)
		fatal("failed to create peer context");

	if (kyrka_heaven_ifc(peer->ctx, peer_heaven_input, peer) == -1)
		fatal("kyrka_heaven_ifc: %d", kyrka_last_error(peer->ctx));

	if (kyrka_purgatory_ifc(peer->ctx, peer_purgatory_input, peer) == -1)
		fatal("kyrka_purgatory_ifc: %d", kyrka_last_error(peer->ctx));

	if (vigil->config->mode == VIGIL_MODE_TUNNEL) {
		memcpy(&peer->addr, &vigil->peer, sizeof(vigil->peer));

		if (kyrka_secret_load(peer->ctx, vigil->config->secret,
		    sizeof(vigil->config->secret)) == -1) {
			fatal("kyrka_secret_load: %d",
			    kyrka_last_error(peer->ctx));
		}
	} else {
		peer_cathedral_config(peer);
	}

	if (vigil->config->flags & VIGIL_CONFIG_FLAG_SHROUD) {
		if (kyrka_shroud_enable(peer->ctx) == -1) {
			fatal("failed to enable shroud: %d",
			    kyrka_last_error(peer->ctx));
		}
	}

	if (kyrka_mtu_size(peer->ctx, vigil->config->mtu) == -1) {
		fatal("failed to set libkyrka mtu size: %d",
		    kyrka_last_error(peer->ctx));
	}

	LIST_INSERT_HEAD(&peers, peer, list);

	vigil_log(LOG_INFO, "[peer=%02x] tunnel created (%s) (port=%u)", id,
	    vigil_address(&peer->addr), ntohs(peer->port));
}

/*
 * Configure the cathedral side of the given peer its context, the keys
 * are loaded from memory as we never have them on disk.
 */
static void
peer_cathedral_config(struct vigil_peer *peer)
{
	struct kyrka_cathedral_cfg	cfg;

	PRECOND(peer != NULL);
	PRECOND(vigil->config->mode != VIGIL_MODE_TUNNEL);

	if (vigil->config->mode == VIGIL_MODE_LITURGY &&
	    (vigil->config->flags & VIGIL_CONFIG_FLAG_REMEMBRANCE)) {
		memset(&peer->cathedral, 0, sizeof(peer->cathedral));

		if (vigil_remembrance_cathedral(&vigil->remembrance,
		    &peer->cathedral) == -1) {
			memcpy(&peer->cathedral, &vigil->cathedral,
			    sizeof(vigil->cathedral));
		}
	} else {
		memcpy(&peer->cathedral, &vigil->cathedral,
		    sizeof(vigil->cathedral));
	}

	memcpy(&peer->addr, &peer->cathedral.addr,
	    sizeof(peer->cathedral.addr));

	peer->cathedral.last = vigil->now;
	peer->cathedral.timeout = VIGIL_CATHEDRAL_TIMEOUT_INIT;

	memset(&cfg, 0, sizeof(cfg));

	cfg.identity = vigil->config->cathedral_id;
	cfg.flock_src = vigil->config->cathedral_flock;

	if (vigil->config->cathedral_flock_dst != 0)
		cfg.flock_dst = vigil->config->cathedral_flock_dst;
	else
		cfg.flock_dst = vigil->config->cathedral_flock;

	if (vigil->config->mode == VIGIL_MODE_LITURGY)
		cfg.tunnel = (vigil->config->spi & 0xff) << 8 | peer->id;
	else
		cfg.tunnel = vigil->config->spi;

	if (vigil->config->mode == VIGIL_MODE_CATHEDRAL &&
	    (vigil->config->flags & VIGIL_CONFIG_FLAG_REMEMBRANCE))
		cfg.remembrance = 1;

	cfg.udata = peer;
	cfg.send = peer_kyrka_send;

	if (kyrka_cathedral_config(peer->ctx, &cfg) == -1) {
		fatal("kyrka_cathedral_config: %d",
		    kyrka_last_error(peer->ctx));
	}

	if (kyrka_device_kek_load(peer->ctx, vigil->config->kek,
	    sizeof(vigil->config->kek)) == -1)
		fatal("kyrka_device_kek_load: %d", kyrka_last_error(peer->ctx));

	if (kyrka_cathedral_secret_load(peer->ctx,
	    vigil->config->cathedral_secret,
	    sizeof(vigil->config->cathedral_secret)) == -1) {
		fatal("kyrka_cathedral_secret_load: %d",
		    kyrka_last_error(peer->ctx));
	}

	if (kyrka_cathedral_cosk_load(peer->ctx,
	    vigil->config->cathedral_cosk,
	    sizeof(vigil->config->cathedral_cosk)) == -1) {
		fatal("kyrka_cathedral_cosk_load: %d",
		    kyrka_last_error(peer->ctx));
	}
}

/*
 * Mark the given peer as to be removed, but only if the timeout
 * triggered, in case the peer has trouble talking to cathedrals
 * but not to us.
 */
static void
peer_delete(struct vigil_peer *peer)
{
	PRECOND(peer != NULL);

	if (peer->flags & VIGIL_PEER_FLAG_REMOVE)
		return;

	if ((vigil->now - peer->alive) < VIGIL_PEER_ALIVE_TIMEOUT)
		return;

	peer->flags |= VIGIL_PEER_FLAG_REMOVE;
	vigil_log(LOG_INFO, "[peer=%02x] tunnel marked for removal", peer->id);
}

/*
 * Release all resources tied to a peer.
 */
static void
peer_free(struct vigil_peer *peer)
{
	PRECOND(peer != NULL);

	LIST_REMOVE(peer, list);
	vigil_io_unschedule(&peer->io);

	vigil_ether_cleanup(peer);
	kyrka_ctx_free(peer->ctx);
	vigil_socket_close(peer->io.fd);

	free(peer);
}

/*
 * Find the peer that should receive the given packet.
 */
static struct vigil_peer *
peer_lookup(const u_int8_t *data, size_t len)
{
	struct vigil_peer	*peer;
	u_int8_t		local, id;
	struct in_addr		dst, expected;

	PRECOND(data != NULL);

	if (vigil->config->mode != VIGIL_MODE_LITURGY)
		return (LIST_FIRST(&peers));

	if (vigil_inet_ipv4_dst(data, len, &dst) == -1)
		return (NULL);

	local = vigil->config->spi & 0xff;
	id = ntohl(dst.s_addr) >> 8 & 0xff;

	vigil_inet_liturgy(&expected,
	    &vigil->config->liturgy_prefix, id, local);

	if (dst.s_addr != expected.s_addr)
		return (NULL);

	LIST_FOREACH(peer, &peers, list) {
		if (peer->id == id)
			return (peer);
	}

	return (NULL);
}

/*
 * A packet is larger than the MTU towards our peer, if it has DF set
 * we answer it with an icmp fragmentation needed like a router would.
 *
 * Returns 0 if the packet was answered and must be dropped, -1 if it
 * should be sent anyway.
 */
static int
peer_mtu_exceeded(struct vigil_peer *peer, const u_int8_t *data, size_t len)
{
	size_t		ret;
	u_int8_t	icmp[128];

	PRECOND(peer != NULL);
	PRECOND(data != NULL);
	PRECOND(peer->mtu != 0);

	ret = vigil_inet_icmp_frag(data, len, peer->mtu, icmp, sizeof(icmp));
	if (ret == 0)
		return (-1);

	if (vigil_platform_tun_write(vigil->fd, icmp, ret) == -1) {
		if (errno != EAGAIN && errno != EWOULDBLOCK)
			vigil_log(LOG_NOTICE, "tunnel write: %s", errno_s);
	}

	return (0);
}

/*
 * Returns what shroud key libkyrka should use for packets to the peer.
 */
static u_int32_t
peer_shroud(struct vigil_peer *peer)
{
	PRECOND(peer != NULL);

	if (vigil->config->mode == VIGIL_MODE_TUNNEL)
		return (KYRKA_PACKET_SHROUD_PEER);

	if (vigil_inet_match(&peer->addr, &peer->cathedral.addr))
		return (KYRKA_PACKET_SHROUD_CATHEDRAL);

	return (KYRKA_PACKET_SHROUD_PEER);
}

/*
 * Callback from our event loop when data is to be handled on the peer socket.
 */
static void
peer_io_event(void *udata)
{
	struct vigil_peer	*peer;

	PRECOND(udata != NULL);

	peer = udata;

	if (peer->io.flags & VIGIL_IO_READABLE)
		peer_io_read(peer);
}

/*
 * Attempt to read packets from the peer socket and insert them into
 * the libkyrka context for handling.
 */
static void
peer_io_read(struct vigil_peer *peer)
{
	int			idx;
	size_t			len;
	ssize_t			ret;
	struct kyrka_packet	pkt;
	u_int8_t		*ptr;
	socklen_t		socklen;

	PRECOND(peer != NULL);

	for (idx = 0; idx < VIGIL_IO_BATCH; idx++) {
		if ((ptr = kyrka_packet_recvbuf(peer->ctx, &pkt, &len)) == NULL)
			fatal("failed to get peer receive buffer");

		socklen = sizeof(sender);

		if ((ret = recvfrom(peer->io.fd, ptr, len, MSG_DONTWAIT,
		    (struct sockaddr *)&sender, &socklen)) == -1) {
			if (errno == EINTR)
				continue;
			if (errno == EWOULDBLOCK || errno == EAGAIN) {
				peer->io.flags &= ~VIGIL_IO_READABLE;
				break;
			}

			vigil_log(LOG_NOTICE,
			    "[peer=%02x] recvfrom: %s", peer->id, errno_s);
			peer->io.flags &= ~VIGIL_IO_READABLE;
			break;
		}

		if (ret == 0)
			continue;

		pkt.length = ret;
		peer->rx_bytes += ret;

		if (vigil->config->mode != VIGIL_MODE_TUNNEL &&
		    vigil_inet_match(&sender, &peer->cathedral.addr))
			pkt.shroud = KYRKA_PACKET_SHROUD_CATHEDRAL;
		else
			pkt.shroud = KYRKA_PACKET_SHROUD_PEER;

		(void)kyrka_purgatory_input(peer->ctx, &pkt);
	}
}

/*
 * Callback from libkyrka when an event occurred on the peer tunnel.
 */
static void
peer_kyrka_event(KYRKA *ctx, union kyrka_event *evt, void *udata)
{
	u_int32_t		ip;
	struct sockaddr_in	sin;
	struct vigil_peer	*peer;

	PRECOND(ctx != NULL);
	PRECOND(evt != NULL);
	PRECOND(udata != NULL);

	peer = udata;

	switch (evt->type) {
	case KYRKA_EVENT_KEYS_INFO:
		vigil_log(LOG_INFO, "[peer=%02x] tx=%08x rx=%08x",
		    peer->id, evt->keys.tx_spi, evt->keys.rx_spi);

		if (evt->keys.tx_spi != 0 && evt->keys.tx_spi != peer->tx_spi) {
			peer->tx_spi = evt->keys.tx_spi;
			peer_holepunch(peer);
			vigil_grace_mtu_start(peer);
		}

		peer->tx_spi = evt->keys.tx_spi;
		peer->rx_spi = evt->keys.rx_spi;

		if (peer->tx_spi != 0 && peer->rx_spi != 0) {
			if (!(peer->flags & VIGIL_PEER_FLAG_ONLINE)) {
				vigil_log(LOG_INFO,
				    "[peer=%02x] tunnel online", peer->id);
			}

			peer->flags |= VIGIL_PEER_FLAG_ONLINE;
		}
		break;
	case KYRKA_EVENT_KEYS_ERASED:
		peer->tx_spi = 0;
		peer->rx_spi = 0;
		peer->flags &= ~VIGIL_PEER_FLAG_ONLINE;
		vigil_log(LOG_INFO, "[peer=%02x] keys erased", peer->id);
		break;
	case KYRKA_EVENT_EXCHANGE_INFO:
		vigil_log(LOG_INFO, "[peer=%02x] exchange: %s",
		    peer->id, evt->exchange.reason);
		break;
	case KYRKA_EVENT_AMBRY_RECEIVED:
		peer->ambry = evt->ambry.generation;
		peer_cathedral_alive(peer);
		vigil_log(LOG_INFO, "[peer=%02x] ambry generation %08x",
		    peer->id, evt->ambry.generation);
		break;
	case KYRKA_EVENT_REMEMBRANCE_RECEIVED:
		peer_cathedral_alive(peer);
		vigil_remembrance_save(&vigil->remembrance, &evt->remembrance);
		break;
	case KYRKA_EVENT_LOGMSG:
		vigil_log(LOG_INFO, "[peer=%02x] log: %s",
		    peer->id, evt->logmsg.log);
		break;
	case KYRKA_EVENT_PEER_DISCOVERY:
		peer_cathedral_alive(peer);

		if (!(vigil->config->flags & VIGIL_CONFIG_FLAG_P2P))
			break;

		sin.sin_family = AF_INET;
		sin.sin_port = evt->peer.port;
		sin.sin_addr.s_addr = evt->peer.ip;

		/*
		 * If we share the same external address with our peer we
		 * relay via the cathedral, we cannot depend on hairpinning.
		 */
		if (evt->peer.flags & KYRKA_INFO_FLAG_SAME_EXTERNAL_IPV4) {
			if (peer->local_discovery == 0) {
				vigil_log(LOG_INFO, "[peer=%02x] peer shares "
				    "our external address, relaying",
				    peer->id);
			}

			peer->local_discovery = 1;

			/* A path tier6 local discovery found stays. */
			if (peer->flags & VIGIL_PEER_FLAG_LOCAL)
				break;

			memcpy(&sin, &peer->cathedral.addr, sizeof(sin));
		} else {
			peer->local_discovery = 0;
			peer->flags &= ~VIGIL_PEER_FLAG_LOCAL;
		}

		/* We will never reach a peer on loopback, relay instead. */
		ip = ntohl(sin.sin_addr.s_addr);
		if ((ip >> 24) == 127)
			memcpy(&sin, &peer->cathedral.addr, sizeof(sin));

		if (vigil->now < peer->p2p_retry &&
		    vigil_inet_match(&sin, &peer->p2p_failed))
			memcpy(&sin, &peer->cathedral.addr, sizeof(sin));

		if (vigil_inet_match(&sin, &peer->addr))
			break;

		vigil_grace_mtu_start(peer);

		if (vigil_inet_match(&sin, &peer->cathedral.addr)) {
			peer_relay(peer);
			break;
		}

		memcpy(&peer->addr, &sin, sizeof(sin));

		if (kyrka_p2p_active(peer->ctx, 1) == -1)
			fatal("failed to set p2p status");

		peer->flags |= VIGIL_PEER_FLAG_P2P;
		peer_holepunch(peer);

		peer->p2p_probe = 0;
		peer->p2p_verify = vigil->now + VIGIL_PEER_P2P_VERIFY;

		vigil_log(LOG_INFO, "[peer=%02x] p2p discovery %s",
		    peer->id, vigil_address(&peer->addr));
		break;
	default:
		vigil_log(LOG_NOTICE, "[peer=%02x] unknown event %u",
		    peer->id, evt->type);
		break;
	}
}

/*
 * Callback from libkyrka when plaintext data is available for a tunnel.
 */
static void
peer_heaven_input(struct kyrka_packet *pkt, u_int64_t seq, void *udata)
{
	u_int8_t		local;
	struct vigil_peer	*peer;
	void			*data;
	struct in_addr		src, expected;

	PRECOND(pkt != NULL);
	PRECOND(udata != NULL);

	peer = udata;
	data = kyrka_packet_data(pkt);

	peer->alive = vigil->now;

	if (vigil->config->mode != VIGIL_MODE_TUNNEL &&
	    vigil_inet_match(&sender, &peer->cathedral.addr))
		peer->cathedral.last = vigil->now;

	if (pkt->type == KYRKA_PACKET_TYPE_GRACE) {
		vigil_grace_input(peer, &sender, data, pkt->length);
		return;
	}

	if (pkt->type != KYRKA_PACKET_TYPE_IP)
		return;

	peer->rx_pkts++;

	if (vigil->config->flags & VIGIL_CONFIG_FLAG_TIER6) {
		if (vigil_inet_match(&sender, &peer->addr))
			peer->p2p_verify = 0;

		vigil_ether_input(peer, &sender, data, pkt->length);
		return;
	}

	switch (vigil_inet_version(data, pkt->length)) {
	case 4:
		break;
	case 6:
		if (vigil->config->flags & VIGIL_CONFIG_FLAG_BLOCK_IPV6)
			return;
		if (vigil->config->mode == VIGIL_MODE_LITURGY)
			return;
		break;
	default:
		return;
	}

	if (vigil->config->mode == VIGIL_MODE_LITURGY) {
		if (vigil_inet_ipv4_src(data, pkt->length, &src) == -1)
			return;

		local = vigil->config->spi & 0xff;

		vigil_inet_liturgy(&expected,
		    &vigil->config->liturgy_prefix, peer->id, local);

		if (src.s_addr != expected.s_addr)
			return;

		if (vigil_inet_rewrite_dst(data, pkt->length, &vigil->ip) == -1)
			return;
	}

	if (vigil_platform_tun_write(vigil->fd, data, pkt->length) == -1) {
		if (errno != EAGAIN && errno != EWOULDBLOCK)
			vigil_log(LOG_NOTICE, "tunnel write: %s", errno_s);
	}
}

/*
 * Callback from libkyrka when ciphertext is available. This ciphertext
 * is sent to the current known address for our peer.
 */
static void
peer_purgatory_input(struct kyrka_packet *pkt, u_int64_t seq, void *udata)
{
	size_t			len;
	u_int8_t		*data;
	struct vigil_peer	*peer;

	PRECOND(pkt != NULL);
	PRECOND(udata != NULL);

	peer = udata;

	data = kyrka_packet_sendbuf(peer->ctx, pkt, &len);
	if (data == NULL)
		fatal("failed to get peer send buffer");

	if (sendto(peer->io.fd, data, len, 0,
	    (const struct sockaddr *)&peer->addr, sizeof(peer->addr)) == -1) {
		if (errno != EAGAIN && errno != EWOULDBLOCK &&
		    !vigil_socket_unreachable(errno,
		    vigil->config->mode == VIGIL_MODE_TUNNEL ||
		    vigil_inet_match(&peer->addr, &peer->cathedral.addr))) {
			vigil_log(LOG_INFO,
			    "[peer=%02x] sendto: %s", peer->id, errno_s);
		}
	} else {
		peer->tx_bytes += len;
	}
}

/*
 * Callback from libkyrka when ciphertext data is to be sent to our cathedral.
 */
static void
peer_kyrka_send(struct kyrka_packet *pkt, u_int64_t magic, void *udata)
{
	size_t			len;
	struct sockaddr_in	sin;
	struct vigil_peer	*peer;
	u_int8_t		*data;

	PRECOND(pkt != NULL);
	PRECOND(udata != NULL);

	peer = udata;

	memcpy(&sin, &peer->cathedral.addr, sizeof(sin));

	if (magic == KYRKA_CATHEDRAL_NAT_MAGIC)
		sin.sin_port = htons(vigil->config->cathedral_nat_port);

	data = kyrka_packet_sendbuf(peer->ctx, pkt, &len);
	if (data == NULL)
		fatal("failed to get peer cathedral send buffer");

	if (sendto(peer->io.fd, data, len, 0,
	    (struct sockaddr *)&sin, sizeof(sin)) == -1) {
		if (errno != EAGAIN && errno != EWOULDBLOCK &&
		    !vigil_socket_unreachable(errno, 1)) {
			vigil_log(LOG_INFO,
			    "[peer=%02x] sendto: %s (cathedral)",
			    peer->id, errno_s);
		}
	} else {
		peer->tx_bytes += len;
	}
}

/*
 * Give every peer a new socket, the network we were on changed. We go
 * back to the cathedral and tell it about us right away, a p2p path we
 * had will not work from the new network anyway.
 */
void
vigil_peer_rebind(void)
{
	int			fd;
	struct vigil_peer	*peer;

	LIST_FOREACH(peer, &peers, list) {
		fd = peer_socket(&peer->port);
		if (fd == -1)
			continue;

		vigil_io_unschedule(&peer->io);
		vigil_socket_close(peer->io.fd);

		peer->io.fd = fd;
		vigil_io_schedule(&peer->io);

		peer->cathedral_update = 0;

		if (vigil->config->mode != VIGIL_MODE_TUNNEL &&
		    (peer->flags & VIGIL_PEER_FLAG_P2P))
			peer_relay(peer);
	}
}

/*
 * Send heartbeats to every peer and tell the cathedral about them on
 * the next update, which we do right away.
 */
void
vigil_peer_poke(void)
{
	struct vigil_peer	*peer;

	next_update = 0;

	LIST_FOREACH(peer, &peers, list) {
		peer->cathedral_update = 0;

		if (vigil->config->flags & VIGIL_CONFIG_FLAG_TIER6)
			vigil_ether_poke(peer);
		else
			vigil_grace_poke(peer);
	}
}

/*
 * Our peer answered us on a local address found via tier6 local
 * discovery, it is authenticated so we move the tunnel there.
 */
void
vigil_peer_local(struct vigil_peer *peer, struct sockaddr_in *sin)
{
	PRECOND(peer != NULL);
	PRECOND(sin != NULL);
	PRECOND(vigil->config->flags & VIGIL_CONFIG_FLAG_TIER6);

	if (vigil->config->mode == VIGIL_MODE_TUNNEL)
		return;

	if (!(vigil->config->flags & VIGIL_CONFIG_FLAG_P2P))
		return;

	if (vigil_inet_match(sin, &peer->addr))
		return;

	memcpy(&peer->addr, sin, sizeof(*sin));

	if (kyrka_p2p_active(peer->ctx, 1) == -1)
		fatal("failed to set p2p status");

	peer->p2p_verify = 0;
	peer->flags |= VIGIL_PEER_FLAG_P2P | VIGIL_PEER_FLAG_LOCAL;

	vigil_log(LOG_INFO, "[peer=%02x] local discovery %s",
	    peer->id, vigil_address(&peer->addr));
}

/*
 * Create the socket for a peer, bound to a port of our own which we
 * return in port. Returns -1 if we could not.
 */
static int
peer_socket(u_int16_t *port)
{
	int			fd;
	struct sockaddr_in	sin;
	socklen_t		socklen;

	PRECOND(port != NULL);

	fd = vigil_socket_udp();
	if (fd == -1)
		return (-1);

	socklen = sizeof(sin);
	if (getsockname(fd, (struct sockaddr *)&sin, &socklen) == -1) {
		vigil_log(LOG_NOTICE, "getsockname: %s", errno_s);
		vigil_socket_close(fd);
		return (-1);
	}

	*port = sin.sin_port;

	return (fd);
}

/*
 * Update our cathedral about our presence if required.
 */
static void
peer_cathedral_update(struct vigil_peer *peer)
{
	PRECOND(peer != NULL);

	if (vigil->now < peer->cathedral_update)
		return;

	peer->cathedral_update = vigil->now + VIGIL_CATHEDRAL_NOTIFY;

	if (kyrka_cathedral_notify(peer->ctx) == -1) {
		vigil_log(LOG_NOTICE,
		    "[peer=%02x] kyrka_cathedral_notify: %d",
		    peer->id, kyrka_last_error(peer->ctx));
	}

	if (vigil->config->flags & VIGIL_CONFIG_FLAG_P2P) {
		if (kyrka_cathedral_nat_detection(peer->ctx) == -1) {
			vigil_log(LOG_NOTICE, "[peer=%02x] "
			    "kyrka_cathedral_nat_detection: %d",
			    peer->id, kyrka_last_error(peer->ctx));
		}
	}
}

/*
 * Check if our cathedral is responsive or if we need to swap to another one.
 */
static void
peer_cathedral_check(struct vigil_peer *peer)
{
	int		was_cathedral;

	PRECOND(peer != NULL);

	if ((vigil->now - peer->cathedral.last) <= peer->cathedral.timeout)
		return;

	if (peer->cathedral.alive) {
		vigil_log(LOG_NOTICE,
		    "[peer=%02x] cathedral timed out (%u)", peer->id,
		    peer->cathedral.timeout / 1000);
	}

	peer->cathedral.alive = 0;
	was_cathedral = vigil_inet_match(&peer->addr, &peer->cathedral.addr);

	if (vigil_remembrance_cathedral(&vigil->remembrance,
	    &peer->cathedral) != -1) {
		vigil_log(LOG_NOTICE, "[peer=%02x] switching to cathedral %s",
		    peer->id, vigil_address(&peer->cathedral.addr));

		if (was_cathedral) {
			memcpy(&peer->addr, &peer->cathedral.addr,
			    sizeof(peer->addr));
		}
	} else {
		peer->cathedral.last = vigil->now;
	}
}

/*
 * Mark our cathedral as alive and set the timeout to the non initial one.
 */
static void
peer_cathedral_alive(struct vigil_peer *peer)
{
	PRECOND(peer != NULL);

	peer->cathedral.last = vigil->now;
	peer->cathedral.timeout = VIGIL_CATHEDRAL_TIMEOUT;

	if (peer->cathedral.alive == 0) {
		peer->cathedral.alive = 1;
		vigil_log(LOG_INFO, "[peer=%02x] cathedral %s is alive",
		    peer->id, vigil_address(&peer->cathedral.addr));
	}
}

/*
 * Send everything for the peer via its cathedral again.
 */
static void
peer_relay(struct vigil_peer *peer)
{
	PRECOND(peer != NULL);
	PRECOND(vigil->config->mode != VIGIL_MODE_TUNNEL);

	memcpy(&peer->addr, &peer->cathedral.addr, sizeof(peer->addr));

	if (kyrka_p2p_active(peer->ctx, 0) == -1)
		fatal("failed to set p2p status");

	peer->p2p_verify = 0;
	peer->flags &= ~(VIGIL_PEER_FLAG_P2P | VIGIL_PEER_FLAG_LOCAL);

	vigil_log(LOG_INFO, "[peer=%02x] relayed via %s",
	    peer->id, vigil_address(&peer->addr));
}

/*
 * Check if the p2p path got one of our probes acked in time, if not
 * we go back to relaying via the cathedral.
 */
static void
peer_p2p_check(struct vigil_peer *peer)
{
	PRECOND(peer != NULL);

	if (peer->p2p_verify == 0 || vigil->now < peer->p2p_verify)
		return;

	/*
	 * No keys means we could not probe either, get keys via the
	 * cathedral first. It will tell us about the p2p path again.
	 */
	if (peer->tx_spi == 0) {
		vigil_log(LOG_NOTICE, "[peer=%02x] no keys via p2p path %s",
		    peer->id, vigil_address(&peer->addr));
		peer_relay(peer);
		return;
	}

	vigil_log(LOG_NOTICE, "[peer=%02x] p2p path %s is not working",
	    peer->id, vigil_address(&peer->addr));

	peer_p2p_failed(peer);
}

/*
 * Do some form of dead peer detection and wipe keys if we find
 * the peer to be deadsies, same as sanctum its chapel does.
 */
static void
peer_alive_check(struct vigil_peer *peer)
{
	PRECOND(peer != NULL);

	if (peer->rx_spi == 0)
		return;

	if ((vigil->now - peer->alive) < VIGIL_PEER_DEAD_TIMEOUT)
		return;

	vigil_log(LOG_NOTICE,
	    "[peer=%02x] peer is unresponsive, resetting", peer->id);

	if (peer->flags & VIGIL_PEER_FLAG_P2P)
		peer_p2p_failed(peer);

	if (kyrka_peer_timeout(peer->ctx) == -1)
		fatal("kyrka_peer_timeout: %d", kyrka_last_error(peer->ctx));

	peer->alive = vigil->now;
}

/*
 * We got new keys or a new path to our peer, send heartbeats more often
 * for a while to open up NAT states between us.
 */
static void
peer_holepunch(struct vigil_peer *peer)
{
	PRECOND(peer != NULL);

	if (vigil->config->flags & VIGIL_CONFIG_FLAG_TIER6)
		vigil_ether_holepunch(peer);
	else
		vigil_grace_holepunch(peer);
}

/*
 * The p2p path to our peer failed, remember it so we do not try it
 * again for a while and relay via the cathedral.
 */
static void
peer_p2p_failed(struct vigil_peer *peer)
{
	PRECOND(peer != NULL);
	PRECOND(peer->flags & VIGIL_PEER_FLAG_P2P);

	memcpy(&peer->p2p_failed, &peer->addr, sizeof(peer->addr));
	peer->p2p_retry = vigil->now + VIGIL_PEER_P2P_RETRY;

	vigil_grace_mtu_start(peer);
	peer_relay(peer);
}
