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
#include <net/if.h>
#include <netinet/in.h>

#include <ifaddrs.h>
#include <stdio.h>
#include <stdlib.h>

#include "vigil.h"

/* ARP hardware type and ops. */
#define ETHER_ARP_HW_ETHER	1
#define ETHER_ARP_OP_REQUEST	1
#define ETHER_ARP_OP_REPLY	2

/* The maximum age in milliseconds a MAC is valid. */
#define ETHER_MAC_AGE_MAX	((60 * 20) * 1000)

/* The interval in milliseconds between arp requests for an address. */
#define ETHER_ARP_INTERVAL	(1 * 1000)

/* The bit in the first octet of a MAC address marking it as a group. */
#define ETHER_MAC_GROUP		0x01

/* The first octet of our own MAC addresses, locally administered. */
#define ETHER_MAC_LOCAL		0x06

/* The bits of the multicast group that make it into its MAC address. */
#define ETHER_MULTICAST_GROUP	0x007fffff

/* Time between heartbeats and holepunching bursts. */
#define ETHER_HB_BURST		1000
#define ETHER_HB_INTERVAL	(5 * 1000)
#define ETHER_HB_BURST_TIME	(15 * 1000)

/*
 * An ARP packet.
 */
struct ether_arp {
	u_int16_t	hw;
	u_int16_t	proto;
	u_int8_t	hwlen;
	u_int8_t	protolen;
	u_int16_t	op;
	u_int8_t	sha[VIGIL_ETHER_MAC_LEN];
	u_int32_t	spa;
	u_int8_t	tha[VIGIL_ETHER_MAC_LEN];
	u_int32_t	tpa;
} __attribute__((packed));

static void	ether_mac_prune(struct vigil_peer *);
static void	ether_header(struct vigil_ether *, u_int16_t);
static int	ether_multicast(struct in_addr *, u_int8_t *);
static void	ether_send(struct vigil_peer *, const struct vigil_ether *,
		    const void *, size_t);

static void	ether_heartbeat_send(struct vigil_peer *);
static void	ether_heartbeat_recv(struct vigil_peer *, const u_int8_t *,
		    size_t);

static void	ether_discovery_probe(struct vigil_peer *, u_int32_t,
		    u_int16_t);
static void	ether_discovery_send(struct vigil_peer *, u_int16_t,
		    struct sockaddr_in *);

static void	ether_arp_request(struct in_addr *);
static void	ether_arp_reply(struct vigil_peer *, const struct ether_arp *);
static void	ether_arp_input(struct vigil_peer *, struct vigil_mac *,
		    const u_int8_t *, size_t);

static struct vigil_mac	*ether_mac_lookup(struct in_addr *,
			    struct vigil_peer **);
static struct vigil_mac	*ether_mac_register(struct vigil_peer *,
			    const u_int8_t *);

/* The ethernet broadcast address. */
static const u_int8_t	broadcast[VIGIL_ETHER_MAC_LEN] = {
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff
};

/* The prefix for ipv4 multicast MAC addresses. */
static const u_int8_t	multicast[3] = { 0x01, 0x00, 0x5e };

/* The last address we sent an arp request for. */
static struct in_addr	arp_last;

/* The next time we may send an arp request for arp_last. */
static u_int64_t	arp_next;

/*
 * Initialise an ethernet peer.
 */
void
vigil_ether_init(struct vigil_peer *peer)
{
	PRECOND(peer != NULL);

	memset(&peer->heartbeats, 0, sizeof(peer->heartbeats));

	peer->heartbeats.next = vigil->now;
	peer->heartbeats.interval = ETHER_HB_INTERVAL;
}

/*
 * Call for every peer update tick, removes expired MAC addresses for this
 * peer and sends heartbeats when required.
 */
void
vigil_ether_update(struct vigil_peer *peer)
{
	PRECOND(peer != NULL);

	if (peer->heartbeats.reset != 0 &&
	    vigil->now >= peer->heartbeats.reset) {
		peer->heartbeats.reset = 0;
		peer->heartbeats.interval = ETHER_HB_INTERVAL;
	}

	ether_mac_prune(peer);
	ether_heartbeat_send(peer);
}

/*
 * We got new keys or our peer moved, send heartbeats every second
 * for a while to open up NAT states between us.
 */
void
vigil_ether_holepunch(struct vigil_peer *peer)
{
	PRECOND(peer != NULL);

	peer->heartbeats.next = vigil->now;
	peer->heartbeats.interval = ETHER_HB_BURST;
	peer->heartbeats.reset = vigil->now + ETHER_HB_BURST_TIME;
}

/*
 * Send a heartbeat to the peer on its next update.
 */
void
vigil_ether_poke(struct vigil_peer *peer)
{
	PRECOND(peer != NULL);

	peer->heartbeats.next = vigil->now;
}

/*
 * Prune all expired MAC addresses from the peer.
 */
static void
ether_mac_prune(struct vigil_peer *peer)
{
	struct vigil_mac	*mac, *next;

	PRECOND(peer != NULL);

	for (mac = LIST_FIRST(&peer->macs); mac != NULL; mac = next) {
		next = LIST_NEXT(mac, list);

		if ((vigil->now - mac->age) >= ETHER_MAC_AGE_MAX) {
			vigil_log(LOG_INFO,
			    "[peer=%02x] %02x:%02x:%02x:%02x:%02x:%02x gone",
			    peer->id, mac->addr[0], mac->addr[1], mac->addr[2],
			    mac->addr[3], mac->addr[4], mac->addr[5]);
			LIST_REMOVE(mac, list);
			free(mac);
		}
	}
}

/*
 * Remove all MAC addresses from the peer.
 */
void
vigil_ether_cleanup(struct vigil_peer *peer)
{
	struct vigil_mac	*mac;

	PRECOND(peer != NULL);

	while ((mac = LIST_FIRST(&peer->macs)) != NULL) {
		LIST_REMOVE(mac, list);
		free(mac);
	}
}

/*
 * Send an ip packet from our tunnel interface as an ethernet frame to
 * the peer its destination lives on, if we don't know it yet we arp
 * for it and drop the packet.
 */
void
vigil_ether_output(const u_int8_t *data, size_t len)
{
	struct vigil_ether	eth;
	struct in_addr		dst;
	struct vigil_mac	*mac;
	struct vigil_peer	*peer;

	PRECOND(data != NULL);

	if (vigil_inet_ipv4_dst(data, len, &dst) == -1)
		return;

	ether_header(&eth, VIGIL_ETHER_TYPE_IPV4);

	if (ether_multicast(&dst, eth.dst) != -1) {
		LIST_FOREACH(peer, vigil_peer_list(), list)
			ether_send(peer, &eth, data, len);
		return;
	}

	if ((mac = ether_mac_lookup(&dst, &peer)) == NULL) {
		ether_arp_request(&dst);
		return;
	}

	memcpy(eth.dst, mac->addr, sizeof(eth.dst));
	ether_send(peer, &eth, data, len);
}

/*
 * An ethernet frame arrived from the given peer, register its source
 * MAC address and handle it if it was meant for us.
 */
void
vigil_ether_input(struct vigil_peer *peer, struct sockaddr_in *from,
    const u_int8_t *data, size_t len)
{
	struct vigil_mac	*mac;
	size_t			total;
	u_int16_t		proto;
	struct vigil_ether	eth, self;

	PRECOND(peer != NULL);
	PRECOND(from != NULL);
	PRECOND(data != NULL);

	if (len < sizeof(eth))
		return;

	memcpy(&eth, data, sizeof(eth));
	proto = ntohs(eth.proto);

	switch (proto) {
	case VIGIL_ETHER_TYPE_ARP:
	case VIGIL_ETHER_TYPE_IPV4:
		break;
	case VIGIL_ETHER_TYPE_HEARTBEAT:
		ether_heartbeat_recv(peer, data, len);
		return;
	case VIGIL_ETHER_TYPE_DISC_PROBE:
		ether_discovery_send(peer, VIGIL_ETHER_TYPE_DISC_ACK, from);
		vigil_peer_local(peer, from);
		return;
	case VIGIL_ETHER_TYPE_DISC_ACK:
		vigil_peer_local(peer, from);
		return;
	default:
		return;
	}

	ether_header(&self, proto);

	if (eth.src[0] & ETHER_MAC_GROUP)
		return;

	if (!memcmp(eth.src, self.src, sizeof(self.src)))
		return;

	if (!(eth.dst[0] & ETHER_MAC_GROUP) &&
	    memcmp(eth.dst, self.src, sizeof(self.src)))
		return;

	mac = ether_mac_register(peer, eth.src);

	data += sizeof(eth);
	len -= sizeof(eth);

	if (proto == VIGIL_ETHER_TYPE_ARP) {
		ether_arp_input(peer, mac, data, len);
		return;
	}

	if (vigil_inet_ipv4_length(data, len, &total) == -1)
		return;

	if (vigil_platform_tun_write(vigil->fd, data, total) == -1) {
		if (errno != EAGAIN && errno != EWOULDBLOCK)
			vigil_log(LOG_NOTICE, "tunnel write: %s", errno_s);
	}
}

/*
 * Setup an ethernet header with our MAC address as the source.
 */
static void
ether_header(struct vigil_ether *eth, u_int16_t proto)
{
	u_int32_t	id;

	PRECOND(eth != NULL);

	id = vigil->config->cathedral_id;

	memset(eth, 0, sizeof(*eth));

	eth->src[0] = ETHER_MAC_LOCAL;
	eth->src[1] = vigil->config->spi & 0xff;
	eth->src[2] = (id >> 24) & 0xff;
	eth->src[3] = (id >> 16) & 0xff;
	eth->src[4] = (id >> 8) & 0xff;
	eth->src[5] = id & 0xff;

	eth->proto = htons(proto);
}

/*
 * Check if the destination is a broadcast or multicast address and
 * if so write its MAC address into mac. Returns 0 if it was.
 */
static int
ether_multicast(struct in_addr *dst, u_int8_t *mac)
{
	u_int32_t	ip, mask, net, group;

	PRECOND(dst != NULL);
	PRECOND(mac != NULL);

	ip = ntohl(dst->s_addr);
	mask = 0xffffffff << (32 - vigil->config->tun_prefix);
	net = ntohl(vigil->config->ip.s_addr) | ~mask;

	if (ip == INADDR_BROADCAST || ip == net) {
		memcpy(mac, broadcast, sizeof(broadcast));
		return (0);
	}

	if (!IN_MULTICAST(ip))
		return (-1);

	group = ip & ETHER_MULTICAST_GROUP;

	memcpy(mac, multicast, sizeof(multicast));

	mac[3] = (group >> 16) & 0xff;
	mac[4] = (group >> 8) & 0xff;
	mac[5] = group & 0xff;

	return (0);
}

/*
 * Encrypt and send an ethernet frame to our peer.
 */
static void
ether_send(struct vigil_peer *peer, const struct vigil_ether *eth,
    const void *data, size_t len)
{
	struct kyrka_packet	pkt;
	u_int8_t		*ptr;
	size_t			maxlen;

	PRECOND(peer != NULL);
	PRECOND(eth != NULL);
	PRECOND(data != NULL || len == 0);

	ptr = kyrka_packet_databuf(peer->ctx, &pkt, &maxlen);
	if (ptr == NULL)
		fatal("failed to get peer data buffer");

	if (sizeof(*eth) + len > maxlen) {
		vigil_log(LOG_NOTICE,
		    "[peer=%02x] frame of %zu too large (max:%zu)",
		    peer->id, sizeof(*eth) + len, maxlen);
		return;
	}

	memcpy(ptr, eth, sizeof(*eth));

	if (len > 0)
		memcpy(ptr + sizeof(*eth), data, len);

	pkt.length = sizeof(*eth) + len;

	if (vigil_inet_match(&peer->addr, &peer->cathedral.addr))
		pkt.shroud = KYRKA_PACKET_SHROUD_CATHEDRAL;
	else
		pkt.shroud = KYRKA_PACKET_SHROUD_PEER;

	if (kyrka_heaven_input(peer->ctx, &pkt) == -1) {
		if (kyrka_last_error(peer->ctx) != KYRKA_ERROR_NO_TX_KEY) {
			vigil_log(LOG_NOTICE,
			    "[peer=%02x] kyrka_heaven_input: %d (%zu)",
			    peer->id, kyrka_last_error(peer->ctx), pkt.length);
		}

		return;
	}

	peer->tx_pkts++;
}

/*
 * Ask all peers who has the given address.
 */
static void
ether_arp_request(struct in_addr *addr)
{
	struct ether_arp	arp;
	struct vigil_ether	eth;
	struct vigil_peer	*peer;

	PRECOND(addr != NULL);

	if (arp_last.s_addr == addr->s_addr && vigil->now < arp_next)
		return;

	arp_last = *addr;
	arp_next = vigil->now + ETHER_ARP_INTERVAL;

	ether_header(&eth, VIGIL_ETHER_TYPE_ARP);
	memcpy(eth.dst, broadcast, sizeof(broadcast));

	memset(&arp, 0, sizeof(arp));

	arp.hw = htons(ETHER_ARP_HW_ETHER);
	arp.proto = htons(VIGIL_ETHER_TYPE_IPV4);
	arp.hwlen = VIGIL_ETHER_MAC_LEN;
	arp.protolen = sizeof(arp.spa);
	arp.op = htons(ETHER_ARP_OP_REQUEST);

	memcpy(arp.sha, eth.src, sizeof(arp.sha));
	arp.spa = vigil->ip.s_addr;
	arp.tpa = addr->s_addr;

	LIST_FOREACH(peer, vigil_peer_list(), list)
		ether_send(peer, &eth, &arp, sizeof(arp));
}

/*
 * An ARP packet arrived from the given peer, update the address of
 * the sender and answer it if it is asking for us.
 */
static void
ether_arp_input(struct vigil_peer *peer, struct vigil_mac *mac,
    const u_int8_t *data, size_t len)
{
	struct ether_arp	arp;
	struct vigil_peer	*p0;
	struct vigil_mac	*m0;
	u_int16_t		op, hw, proto;

	PRECOND(peer != NULL);
	PRECOND(mac != NULL);
	PRECOND(data != NULL);

	if (len < sizeof(arp))
		return;

	memcpy(&arp, data, sizeof(arp));
	op = ntohs(arp.op);
	hw = ntohs(arp.hw);
	proto = ntohs(arp.proto);

	if (hw != ETHER_ARP_HW_ETHER || proto != VIGIL_ETHER_TYPE_IPV4)
		return;

	if (arp.hwlen != VIGIL_ETHER_MAC_LEN || arp.protolen != sizeof(arp.spa))
		return;

	if (op != ETHER_ARP_OP_REQUEST && op != ETHER_ARP_OP_REPLY)
		return;

	if (memcmp(arp.sha, mac->addr, sizeof(arp.sha)))
		return;

	if (arp.spa != 0 && arp.spa != vigil->ip.s_addr &&
	    arp.spa != mac->ip.s_addr) {
		LIST_FOREACH(p0, vigil_peer_list(), list) {
			LIST_FOREACH(m0, &p0->macs, list) {
				if (m0->ip.s_addr == arp.spa)
					m0->ip.s_addr = 0;
			}
		}

		mac->ip.s_addr = arp.spa;
	}

	if (op == ETHER_ARP_OP_REQUEST && arp.tpa == vigil->ip.s_addr)
		ether_arp_reply(peer, &arp);
}

/*
 * Answer the given arp request with our MAC address.
 */
static void
ether_arp_reply(struct vigil_peer *peer, const struct ether_arp *req)
{
	struct ether_arp	arp;
	struct vigil_ether	eth;

	PRECOND(peer != NULL);
	PRECOND(req != NULL);

	ether_header(&eth, VIGIL_ETHER_TYPE_ARP);
	memcpy(eth.dst, req->sha, sizeof(eth.dst));

	memcpy(&arp, req, sizeof(arp));
	arp.op = htons(ETHER_ARP_OP_REPLY);

	memcpy(arp.tha, req->sha, sizeof(arp.tha));
	arp.tpa = req->spa;

	memcpy(arp.sha, eth.src, sizeof(arp.sha));
	arp.spa = vigil->ip.s_addr;

	ether_send(peer, &eth, &arp, sizeof(arp));
}

/*
 * Find the MAC address and the peer it lives on for the given address.
 */
static struct vigil_mac *
ether_mac_lookup(struct in_addr *addr, struct vigil_peer **out)
{
	struct vigil_mac	*mac;
	struct vigil_peer	*peer;

	PRECOND(addr != NULL);
	PRECOND(out != NULL);

	LIST_FOREACH(peer, vigil_peer_list(), list) {
		LIST_FOREACH(mac, &peer->macs, list) {
			if (mac->ip.s_addr == addr->s_addr) {
				*out = peer;
				return (mac);
			}
		}
	}

	return (NULL);
}

/*
 * Register the given MAC address as seen on the peer.
 */
static struct vigil_mac *
ether_mac_register(struct vigil_peer *peer, const u_int8_t *addr)
{
	struct vigil_peer	*p0;
	struct vigil_mac	*mac;

	PRECOND(peer != NULL);
	PRECOND(addr != NULL);

	mac = NULL;

	LIST_FOREACH(p0, vigil_peer_list(), list) {
		LIST_FOREACH(mac, &p0->macs, list) {
			if (!memcmp(mac->addr, addr, VIGIL_ETHER_MAC_LEN))
				break;
		}

		if (mac == NULL)
			continue;

		if (p0 != peer) {
			vigil_log(LOG_INFO,
			    "[mac] %02x:%02x:%02x:%02x:%02x:%02x moved %u->%u",
			    mac->addr[0], mac->addr[1], mac->addr[2],
			    mac->addr[3], mac->addr[4], mac->addr[5],
			    p0->id, peer->id);

			LIST_REMOVE(mac, list);
			LIST_INSERT_HEAD(&peer->macs, mac, list);
		}

		break;
	}

	if (mac != NULL) {
		mac->age = vigil->now;
		return (mac);
	}

	mac = calloc(1, sizeof(*mac));
	if (mac == NULL)
		fatal("failed to allocate new ethernet mac address");

	mac->age = vigil->now;
	memcpy(mac->addr, addr, VIGIL_ETHER_MAC_LEN);

	LIST_INSERT_HEAD(&peer->macs, mac, list);

	vigil_log(LOG_INFO,
	    "[peer=%02x] %02x:%02x:%02x:%02x:%02x:%02x discovered",
	    peer->id, mac->addr[0], mac->addr[1], mac->addr[2],
	    mac->addr[3], mac->addr[4], mac->addr[5]);

	return (mac);
}

/*
 * Send a heartbeat to our peer if it is time to do so. When our peer
 * shares our external address the heartbeat carries our local addresses
 * and port so it can find us on the local network, just like tier6.
 */
static void
ether_heartbeat_send(struct vigil_peer *peer)
{
	struct vigil_ether_hb	hb;
	int			idx;
	struct vigil_ether	eth;
	struct sockaddr_in	*sin, *mask;
	struct ifaddrs		*ifa, *ifap;

	PRECOND(peer != NULL);

	if (peer->tx_spi == 0)
		return;

	if (peer->heartbeats.next == 0 || vigil->now < peer->heartbeats.next)
		return;

	peer->heartbeats.next = vigil->now + peer->heartbeats.interval;

	memset(&hb, 0, sizeof(hb));
	memset(&eth, 0, sizeof(eth));

	eth.proto = htons(VIGIL_ETHER_TYPE_HEARTBEAT);

	if (peer->local_discovery) {
		if (getifaddrs(&ifap) == -1)
			fatal("getifaddrs: %s", errno_s);

		idx = 0;
		hb.port = peer->port;

		for (ifa = ifap; ifa != NULL; ifa = ifa->ifa_next) {
			if (idx >= VIGIL_ETHER_HB_IPV4_MAX)
				break;

			if (ifa->ifa_addr == NULL || ifa->ifa_netmask == NULL)
				continue;

			if (ifa->ifa_addr->sa_family != AF_INET)
				continue;

			if (!(ifa->ifa_flags & IFF_UP))
				continue;

			if (ifa->ifa_flags & (IFF_POINTOPOINT | IFF_LOOPBACK))
				continue;

			sin = (struct sockaddr_in *)ifa->ifa_addr;
			mask = (struct sockaddr_in *)ifa->ifa_netmask;

			hb.ips[idx] = sin->sin_addr.s_addr;
			hb.masks[idx] = mask->sin_addr.s_addr;

			idx++;
		}

		freeifaddrs(ifap);
	}

	ether_send(peer, &eth, &hb, sizeof(hb));
}

/*
 * We received a heartbeat from our peer. If it shares our external
 * address we probe each of its local addresses that is on one of our
 * networks, it moves to that path once it answers.
 */
static void
ether_heartbeat_recv(struct vigil_peer *peer, const u_int8_t *data,
    size_t len)
{
	struct vigil_ether_hb	hb;
	int			idx;
	struct sockaddr_in	*sin;
	struct ifaddrs		*ifa, *ifap;
	u_int32_t		ip, mask, net;

	PRECOND(peer != NULL);
	PRECOND(data != NULL);

	if (len != sizeof(struct vigil_ether) + sizeof(hb)) {
		vigil_log(LOG_NOTICE, "[peer=%02x] heartbeat of size %zu",
		    peer->id, len);
		return;
	}

	if (peer->local_discovery == 0)
		return;

	memcpy(&hb, data + sizeof(struct vigil_ether), sizeof(hb));

	if (hb.port == 0 || hb.ips[0] == 0)
		return;

	if (getifaddrs(&ifap) == -1)
		fatal("getifaddrs: %s", errno_s);

	for (idx = 0; idx < VIGIL_ETHER_HB_IPV4_MAX; idx++) {
		if (hb.ips[idx] == 0)
			break;

		net = ntohl(hb.ips[idx]) & ntohl(hb.masks[idx]);

		for (ifa = ifap; ifa != NULL; ifa = ifa->ifa_next) {
			if (ifa->ifa_addr == NULL || ifa->ifa_netmask == NULL)
				continue;

			if (ifa->ifa_addr->sa_family != AF_INET)
				continue;

			if (ifa->ifa_flags & (IFF_POINTOPOINT | IFF_LOOPBACK))
				continue;

			sin = (struct sockaddr_in *)ifa->ifa_addr;
			ip = ntohl(sin->sin_addr.s_addr);

			sin = (struct sockaddr_in *)ifa->ifa_netmask;
			mask = ntohl(sin->sin_addr.s_addr);

			if ((ip & mask) == net)
				break;
		}

		if (ifa != NULL)
			ether_discovery_probe(peer, hb.ips[idx], hb.port);
	}

	freeifaddrs(ifap);
}

/*
 * Send a discovery probe to the given local address of our peer, if it
 * is there it answers with a discovery ack.
 */
static void
ether_discovery_probe(struct vigil_peer *peer, u_int32_t ip, u_int16_t port)
{
	struct sockaddr_in	sin;

	PRECOND(peer != NULL);
	PRECOND(ip != 0);
	PRECOND(port != 0);

	if (peer->addr.sin_addr.s_addr == ip && peer->addr.sin_port == port)
		return;

	memset(&sin, 0, sizeof(sin));

	sin.sin_family = AF_INET;
	sin.sin_port = port;
	sin.sin_addr.s_addr = ip;

	ether_discovery_send(peer, VIGIL_ETHER_TYPE_DISC_PROBE, &sin);
}

/*
 * Send a discovery frame of the given type to the given address instead
 * of to the address we currently use for our peer.
 */
static void
ether_discovery_send(struct vigil_peer *peer, u_int16_t proto,
    struct sockaddr_in *to)
{
	struct vigil_ether	eth;
	struct sockaddr_in	addr;

	PRECOND(peer != NULL);
	PRECOND(proto == VIGIL_ETHER_TYPE_DISC_PROBE ||
	    proto == VIGIL_ETHER_TYPE_DISC_ACK);
	PRECOND(to != NULL);

	memset(&eth, 0, sizeof(eth));
	eth.proto = htons(proto);

	memcpy(&addr, &peer->addr, sizeof(addr));
	memcpy(&peer->addr, to, sizeof(*to));

	ether_send(peer, &eth, NULL, 0);

	memcpy(&peer->addr, &addr, sizeof(addr));
}
