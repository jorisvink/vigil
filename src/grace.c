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
#include <sys/param.h>

#include <stdio.h>

#include "vigil.h"

/* The step size when walking down the MTU. */
#define GRACE_MTU_STEP			32

/* The number of probes made using the well known sizes below. */
#define GRACE_MTU_FIRST_PROBES		4

static void	grace_mtu(struct vigil_peer *);
static void	grace_heartbeat(struct vigil_peer *);
static void	grace_p2p_probe(struct vigil_peer *);
static void	grace_heartbeat_answer(struct vigil_peer *);
static void	grace_send(struct vigil_peer *, const void *, size_t, size_t);

static void	grace_mtu_reset(struct vigil_peer *);
static void	grace_mtu_ack(struct vigil_peer *, u_int16_t);
static void	grace_mtu_probe(struct vigil_peer *, u_int16_t);

/* The well known path sizes, the first entry is our configured MTU. */
static const u_int16_t	mtu_sizes[] = {
	0,
	1500,
	1422,
	1280,
};

/*
 * Initialise the grace state for a new peer.
 */
void
vigil_grace_init(struct vigil_peer *peer)
{
	PRECOND(peer != NULL);

	memset(&peer->heartbeats, 0, sizeof(peer->heartbeats));
	memset(&peer->mtu_probes, 0, sizeof(peer->mtu_probes));

	peer->heartbeats.interval = VIGIL_GRACE_HOLEPUNCH_INTERVAL;
	peer->heartbeats.next = vigil->now + peer->heartbeats.interval;

	peer->mtu = 0;
	peer->mtu_step = 0;
	peer->mtu_size = 0;
	peer->mtu_probes.interval = VIGIL_GRACE_MTU_PROBE_INTERVAL;
	peer->mtu_probes.next = vigil->now + peer->mtu_probes.interval;
}

/*
 * Called every peer update tick, sends all the graces that are due.
 */
void
vigil_grace_update(struct vigil_peer *peer)
{
	PRECOND(peer != NULL);

	if (peer->heartbeats.reset != 0 &&
	    vigil->now >= peer->heartbeats.reset) {
		peer->heartbeats.reset = 0;
		peer->heartbeats.interval = VIGIL_GRACE_HEARTBEAT_INTERVAL;
	}

	grace_mtu(peer);
	grace_p2p_probe(peer);
	grace_heartbeat(peer);
}

/*
 * We got new keys or our peer moved, send heartbeats every second
 * for a while to open up NAT states between us.
 */
void
vigil_grace_holepunch(struct vigil_peer *peer)
{
	PRECOND(peer != NULL);

	peer->heartbeats.next = vigil->now;
	peer->heartbeats.interval = VIGIL_GRACE_HOLEPUNCH_INTERVAL;
	peer->heartbeats.reset = vigil->now + VIGIL_GRACE_HEARTBEAT_INTERVAL;
}

/*
 * Send a heartbeat to the peer on its next update, if we send any.
 */
void
vigil_grace_poke(struct vigil_peer *peer)
{
	PRECOND(peer != NULL);

	if (peer->heartbeats.next != 0)
		peer->heartbeats.next = vigil->now;
}

/*
 * Start a new MTU discovery run for the peer.
 */
void
vigil_grace_mtu_start(struct vigil_peer *peer)
{
	PRECOND(peer != NULL);

	grace_mtu_reset(peer);
	peer->mtu_probes.next = vigil->now;
}

/*
 * A grace arrived from our peer, handle it, gracefully.
 */
void
vigil_grace_input(struct vigil_peer *peer, struct sockaddr_in *from,
    const void *data, size_t len)
{
	const struct vigil_grace_mtu	*mtu;
	const struct vigil_grace	*grace;

	PRECOND(peer != NULL);
	PRECOND(from != NULL);
	PRECOND(data != NULL);

	if (len < sizeof(*grace)) {
		vigil_log(LOG_NOTICE,
		    "[peer=%02x] sent incorrect grace packet of %zu bytes",
		    peer->id, len);
		return;
	}

	grace = data;

	switch (grace->type) {
	case VIGIL_GRACE_TYPE_HEARTBEAT:
		grace_heartbeat_answer(peer);
		break;
	case VIGIL_GRACE_TYPE_MTU_PROBE:
	case VIGIL_GRACE_TYPE_MTU_ACK:
		if (len < sizeof(*mtu)) {
			vigil_log(LOG_NOTICE,
			    "[peer=%02x] sent invalid grace mtu of %zu bytes",
			    peer->id, len);
			return;
		}

		mtu = data;

		if (mtu->size < VIGIL_GRACE_MTU_MIN ||
		    mtu->size > vigil->config->mtu) {
			vigil_log(LOG_NOTICE,
			    "[peer=%02x] sent an MTU grace with an invalid "
			    "size (%u)", peer->id, mtu->size);
			return;
		}

		if (grace->type == VIGIL_GRACE_TYPE_MTU_PROBE) {
			grace_mtu_ack(peer, mtu->size);
			break;
		}

		if (vigil_inet_match(from, &peer->addr))
			peer->p2p_verify = 0;

		if (!(vigil->config->flags & VIGIL_CONFIG_FLAG_MTU_DISCOVERY))
			break;

		if (peer->mtu != mtu->size) {
			vigil_log(LOG_INFO, "[peer=%02x] MTU ack received "
			    "(mtu=%u)", peer->id, mtu->size);
		}

		peer->mtu = mtu->size;
		grace_mtu_reset(peer);
		break;
	default:
		vigil_log(LOG_NOTICE,
		    "[peer=%02x] sent invalid grace type %02x",
		    peer->id, grace->type);
		break;
	}
}

/*
 * Send a heartbeat if it is time to do so.
 */
static void
grace_heartbeat(struct vigil_peer *peer)
{
	struct vigil_grace	grace;

	PRECOND(peer != NULL);

	if (peer->tx_spi == 0)
		return;

	if (peer->heartbeats.next == 0 || vigil->now < peer->heartbeats.next)
		return;

	memset(&grace, 0, sizeof(grace));

	grace.type = VIGIL_GRACE_TYPE_HEARTBEAT;
	grace_send(peer, &grace, sizeof(grace), sizeof(grace));

	peer->heartbeats.next = vigil->now + peer->heartbeats.interval;
}

/*
 * We received a heartbeat from our peer, so we just answer it immediately.
 * We are on a mobile client that loves sleeping so we do not wait for our
 * own timers to somehow fire.
 *
 * We do not answer if we sent one less than half an interval ago,
 * otherwise two vigils would keep answering each other forever.
 */
static void
grace_heartbeat_answer(struct vigil_peer *peer)
{
	PRECOND(peer != NULL);

	if (peer->heartbeats.next == 0 ||
	    vigil->now + (peer->heartbeats.interval / 2) <
	    peer->heartbeats.next)
		return;

	peer->heartbeats.next = vigil->now;
	grace_heartbeat(peer);
}

/*
 * Reset the MTU discovery state, the next run starts in 600 seconds.
 */
static void
grace_mtu_reset(struct vigil_peer *peer)
{
	PRECOND(peer != NULL);

	peer->mtu_step = 0;
	peer->mtu_size = 0;
	peer->mtu_probes.next = vigil->now + VIGIL_GRACE_MTU_DISCOVERY;
}

/*
 * Send the next MTU probe if discovery is running and one is due.
 */
static void
grace_mtu(struct vigil_peer *peer)
{
	u_int16_t	size, overhead;

	PRECOND(peer != NULL);

	if (!(vigil->config->flags & VIGIL_CONFIG_FLAG_MTU_DISCOVERY))
		return;

	if (peer->tx_spi == 0)
		return;

	if (peer->mtu_probes.next == 0 || vigil->now < peer->mtu_probes.next)
		return;

	if (peer->mtu_size == VIGIL_GRACE_MTU_MIN) {
		vigil_log(LOG_INFO, "[peer=%02x] reached MTU limit "
		    "without acks, your link sucks", peer->id);
		grace_mtu_reset(peer);
		return;
	}

	overhead = VIGIL_TUNNEL_OVERHEAD;
	if (vigil->config->flags & VIGIL_CONFIG_FLAG_SHROUD)
		overhead += VIGIL_SHROUD_OVERHEAD;

	while (peer->mtu_step < GRACE_MTU_FIRST_PROBES) {
		if (peer->mtu_step == 0)
			size = vigil->config->mtu;
		else
			size = mtu_sizes[peer->mtu_step] - overhead;

		peer->mtu_step++;
		if (size > vigil->config->mtu || size < VIGIL_GRACE_MTU_MIN)
			continue;

		grace_mtu_probe(peer, size);
		return;
	}

	if (peer->mtu_size == 0) {
		peer->mtu_size = mtu_sizes[GRACE_MTU_FIRST_PROBES - 1] -
		    overhead - GRACE_MTU_STEP;
	} else {
		VERIFY(peer->mtu_size > VIGIL_GRACE_MTU_MIN);
		peer->mtu_size -= MIN(peer->mtu_size - VIGIL_GRACE_MTU_MIN,
		    GRACE_MTU_STEP);
	}

	if (peer->mtu_size > vigil->config->mtu)
		peer->mtu_size = vigil->config->mtu;

	grace_mtu_probe(peer, peer->mtu_size);
}

/*
 * Send an MTU probe of the given size, the probe is padded to that size.
 */
static void
grace_mtu_probe(struct vigil_peer *peer, u_int16_t size)
{
	struct vigil_grace_mtu		probe;

	PRECOND(peer != NULL);
	PRECOND(size >= VIGIL_GRACE_MTU_MIN && size <= vigil->config->mtu);

	probe.size = size;
	probe.grace.type = VIGIL_GRACE_TYPE_MTU_PROBE;

	grace_send(peer, &probe, sizeof(probe), size);

	peer->mtu_probes.next = vigil->now + peer->mtu_probes.interval;
}

/*
 * Without MTU discovery we still need to know if a new p2p path works,
 * so we send the smallest MTU probe every second until it is acked.
 * Sanctum acks these even if it does not do MTU discovery itself.
 */
static void
grace_p2p_probe(struct vigil_peer *peer)
{
	struct vigil_grace_mtu		probe;

	PRECOND(peer != NULL);

	if (vigil->config->flags & VIGIL_CONFIG_FLAG_MTU_DISCOVERY)
		return;

	if (peer->tx_spi == 0 || peer->p2p_verify == 0)
		return;

	if (vigil->now < peer->p2p_probe)
		return;

	probe.size = VIGIL_GRACE_MTU_MIN;
	probe.grace.type = VIGIL_GRACE_TYPE_MTU_PROBE;

	grace_send(peer, &probe, sizeof(probe), probe.size);
	peer->p2p_probe = vigil->now + VIGIL_GRACE_HOLEPUNCH_INTERVAL;
}

/*
 * Let our peer know its MTU probe of the given size arrived.
 */
static void
grace_mtu_ack(struct vigil_peer *peer, u_int16_t size)
{
	struct vigil_grace_mtu		ack;

	PRECOND(peer != NULL);
	PRECOND(size >= VIGIL_GRACE_MTU_MIN && size <= vigil->config->mtu);

	ack.size = size;
	ack.grace.type = VIGIL_GRACE_TYPE_MTU_ACK;

	grace_send(peer, &ack, sizeof(ack), sizeof(ack));
}

/*
 * Encrypt and send a grace to our peer, the grace is zero padded
 * up to the given total length.
 */
static void
grace_send(struct vigil_peer *peer, const void *grace, size_t len,
    size_t total)
{
	struct kyrka_packet	pkt;
	u_int8_t		*ptr;
	size_t			maxlen;

	PRECOND(peer != NULL);
	PRECOND(grace != NULL);
	PRECOND(len <= total);

	ptr = kyrka_packet_databuf(peer->ctx, &pkt, &maxlen);
	if (ptr == NULL)
		fatal("failed to get peer data buffer for grace");

	VERIFY(total <= maxlen);

	memset(ptr, 0, total);
	memcpy(ptr, grace, len);

	pkt.length = total;
	pkt.type = KYRKA_PACKET_TYPE_GRACE;

	if (vigil->config->mode != VIGIL_MODE_TUNNEL &&
	    vigil_inet_match(&peer->addr, &peer->cathedral.addr))
		pkt.shroud = KYRKA_PACKET_SHROUD_CATHEDRAL;
	else
		pkt.shroud = KYRKA_PACKET_SHROUD_PEER;

	if (kyrka_heaven_input(peer->ctx, &pkt) == -1 &&
	    kyrka_last_error(peer->ctx) != KYRKA_ERROR_NO_TX_KEY) {
		vigil_log(LOG_NOTICE,
		    "[peer=%02x] grace kyrka_heaven_input: %d",
		    peer->id, kyrka_last_error(peer->ctx));
	}
}
