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

#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>

#include <arpa/inet.h>

#include <libkyrka/libnyfe.h>

#include "vigil.h"

/* The KMAC256 label for inet autoconf. */
#define INET_TIER6_AUTOCONF		"TIER6.INET.AUTOCONF"

/* The address in the ipv4 header that inet_ipv4_rewrite() changes. */
#define INET_IPV4_SRC			1
#define INET_IPV4_DST			2

/* Where the tcp and udp checksums live, we fixup those. */
#define INET_TCP_OFF_CSUM		16
#define INET_UDP_OFF_CSUM		6

static int	inet_ipv4_header(const u_int8_t *, size_t, size_t *);
static int	inet_ipv4_rewrite(u_int8_t *, size_t, int, struct in_addr *);

static u_int16_t	inet_csum(const u_int8_t *, size_t);
static void		inet_csum_adjust(u_int8_t *, const u_int8_t *,
			    const u_int8_t *, int);

/*
 * Returns the ip version of the given packet, 0 if it is unknown.
 */
u_int8_t
vigil_inet_version(const u_int8_t *pkt, size_t len)
{
	PRECOND(pkt != NULL);

	if (len == 0)
		return (0);

	return (pkt[0] >> 4);
}

/*
 * Obtain the source address of the given ipv4 packet.
 */
int
vigil_inet_ipv4_src(const u_int8_t *pkt, size_t len, struct in_addr *addr)
{
	const struct ip		*ip;
	size_t			hlen;

	PRECOND(pkt != NULL);
	PRECOND(addr != NULL);

	if (inet_ipv4_header(pkt, len, &hlen) == -1)
		return (-1);

	ip = (const struct ip *)pkt;
	addr->s_addr = ip->ip_src.s_addr;

	return (0);
}

/*
 * Obtain the destination address of the given ipv4 packet.
 */
int
vigil_inet_ipv4_dst(const u_int8_t *pkt, size_t len, struct in_addr *addr)
{
	const struct ip		*ip;
	size_t			hlen;

	PRECOND(pkt != NULL);
	PRECOND(addr != NULL);

	if (inet_ipv4_header(pkt, len, &hlen) == -1)
		return (-1);

	ip = (const struct ip *)pkt;
	addr->s_addr = ip->ip_dst.s_addr;

	return (0);
}

/*
 * Get the total length of the given ipv4 packet from its header.
 */
int
vigil_inet_ipv4_length(const u_int8_t *pkt, size_t len, size_t *total)
{
	const struct ip		*ip;
	size_t			hlen;

	PRECOND(pkt != NULL);
	PRECOND(total != NULL);

	if (inet_ipv4_header(pkt, len, &hlen) == -1)
		return (-1);

	ip = (const struct ip *)pkt;
	*total = be16toh(ip->ip_len);

	if (*total < hlen || *total > len)
		return (-1);

	return (0);
}

/*
 * Rewrite the source address of the given ipv4 packet.
 */
int
vigil_inet_rewrite_src(u_int8_t *pkt, size_t len, struct in_addr *addr)
{
	PRECOND(pkt != NULL);
	PRECOND(addr != NULL);

	return (inet_ipv4_rewrite(pkt, len, INET_IPV4_SRC, addr));
}

/*
 * Rewrite the destination address of the given ipv4 packet.
 */
int
vigil_inet_rewrite_dst(u_int8_t *pkt, size_t len, struct in_addr *addr)
{
	PRECOND(pkt != NULL);
	PRECOND(addr != NULL);

	return (inet_ipv4_rewrite(pkt, len, INET_IPV4_DST, addr));
}

/*
 * Create the liturgy address for the a-b pair under the given prefix,
 * this results in prefix.a.b just like sanctum does.
 */
void
vigil_inet_liturgy(struct in_addr *addr, const struct in_addr *prefix,
    u_int8_t a, u_int8_t b)
{
	PRECOND(addr != NULL);
	PRECOND(prefix != NULL);

	addr->s_addr = htonl(ntohl(prefix->s_addr) |
	    ((u_int32_t)a << 8) | (u_int32_t)b);
}

/*
 * Create the tier6 autoconf address based on the flock and kek-id.
 */
void
vigil_inet_tier6(struct in_addr *addr, u_int64_t flock, u_int8_t kek)
{
	struct nyfe_kmac256	km;
	u_int8_t		zeroes[32], okm[4];

	PRECOND(addr != NULL);

	nyfe_mem_zero(zeroes, sizeof(zeroes));

	nyfe_kmac256_init(&km, zeroes, sizeof(zeroes),
	    INET_TIER6_AUTOCONF, sizeof(INET_TIER6_AUTOCONF) - 1);
	nyfe_kmac256_update(&km, &flock, sizeof(flock));
	nyfe_kmac256_final(&km, okm, sizeof(okm));

	addr->s_addr = htonl((u_int32_t)10 << 24 |
	    (u_int32_t)okm[1] << 16 | (u_int32_t)okm[2] << 8 | kek);
}

/*
 * Build an icmp fragmentation needed message for the given ipv4 packet
 * that was too large for the given mtu, into out.
 *
 * Returns the length of the message, or 0 if no message should be sent
 * because the packet is not ipv4, may be fragmented or is an icmp error.
 */
size_t
vigil_inet_icmp_frag(const u_int8_t *pkt, size_t len, u_int16_t mtu,
    u_int8_t *out, size_t outlen)
{
	const struct ip		*ip;
	struct ip		*hdr;
	struct icmp		*icmp;
	size_t			hlen, quote, total;

	PRECOND(pkt != NULL);
	PRECOND(out != NULL);

	if (inet_ipv4_header(pkt, len, &hlen) == -1)
		return (0);

	ip = (const struct ip *)pkt;

	if (!(be16toh(ip->ip_off) & IP_DF))
		return (0);

	if (ip->ip_p == IPPROTO_ICMP && len > hlen &&
	    pkt[hlen] != ICMP_ECHOREPLY && pkt[hlen] != ICMP_ECHO)
		return (0);

	quote = MIN(len, hlen + 8);
	total = sizeof(*hdr) + ICMP_MINLEN + quote;

	if (total > outlen)
		return (0);

	memset(out, 0, total);

	hdr = (struct ip *)out;
	hdr->ip_v = IPVERSION;
	hdr->ip_hl = sizeof(*hdr) >> 2;
	hdr->ip_len = htobe16(total);
	hdr->ip_ttl = IPDEFTTL;
	hdr->ip_p = IPPROTO_ICMP;
	hdr->ip_src.s_addr = ip->ip_dst.s_addr;
	hdr->ip_dst.s_addr = ip->ip_src.s_addr;
	hdr->ip_sum = htobe16(inet_csum(out, sizeof(*hdr)));

	icmp = (struct icmp *)(out + sizeof(*hdr));
	icmp->icmp_type = ICMP_UNREACH;
	icmp->icmp_code = ICMP_UNREACH_NEEDFRAG;
	icmp->icmp_nextmtu = htobe16(mtu);
	memcpy(out + sizeof(*hdr) + ICMP_MINLEN, pkt, quote);

	icmp->icmp_cksum = htobe16(inet_csum((u_int8_t *)icmp,
	    ICMP_MINLEN + quote));

	return (total);
}

/*
 * Check that the given packet holds a sane ipv4 header and return
 * the length of said header.
 */
static int
inet_ipv4_header(const u_int8_t *pkt, size_t len, size_t *hlen)
{
	const struct ip		*ip;

	PRECOND(pkt != NULL);
	PRECOND(hlen != NULL);

	if (len < sizeof(*ip))
		return (-1);

	ip = (const struct ip *)pkt;

	if (ip->ip_v != IPVERSION)
		return (-1);

	*hlen = (size_t)ip->ip_hl << 2;

	if (*hlen < sizeof(*ip) || *hlen > len)
		return (-1);

	return (0);
}

/*
 * Rewrite the given address in the ipv4 header and fixup both the ipv4
 * header checksum and the checksum of tcp or udp as these cover the
 * addresses via their pseudo header.
 */
static int
inet_ipv4_rewrite(u_int8_t *pkt, size_t len, int which, struct in_addr *addr)
{
	struct ip		*ip;
	struct in_addr		old;
	size_t			hlen;
	u_int8_t		*csum;
	struct in_addr		*field;

	PRECOND(pkt != NULL);
	PRECOND(addr != NULL);
	PRECOND(which == INET_IPV4_SRC || which == INET_IPV4_DST);

	if (inet_ipv4_header(pkt, len, &hlen) == -1)
		return (-1);

	ip = (struct ip *)pkt;

	if (which == INET_IPV4_SRC)
		field = &ip->ip_src;
	else
		field = &ip->ip_dst;

	old.s_addr = field->s_addr;
	field->s_addr = addr->s_addr;

	inet_csum_adjust((u_int8_t *)&ip->ip_sum,
	    (const u_int8_t *)&old, (const u_int8_t *)field, 0);

	if (be16toh(ip->ip_off) & IP_OFFMASK)
		return (0);

	switch (ip->ip_p) {
	case IPPROTO_TCP:
		if (len < hlen + INET_TCP_OFF_CSUM + 2)
			return (0);
		csum = &pkt[hlen + INET_TCP_OFF_CSUM];
		inet_csum_adjust(csum, (const u_int8_t *)&old,
		    (const u_int8_t *)field, 0);
		break;
	case IPPROTO_UDP:
		if (len < hlen + INET_UDP_OFF_CSUM + 2)
			return (0);
		csum = &pkt[hlen + INET_UDP_OFF_CSUM];
		if (csum[0] == 0 && csum[1] == 0)
			break;
		inet_csum_adjust(csum, (const u_int8_t *)&old,
		    (const u_int8_t *)field, 1);
		break;
	}

	return (0);
}

/*
 * Incrementally update the internet checksum at csum for a 32-bit value
 * that changed from old to new (RFC 1624). If udp is 1 a resulting
 * checksum of 0 is sent as 0xffff as 0 means no checksum for udp.
 */
static void
inet_csum_adjust(u_int8_t *csum, const u_int8_t *old,
    const u_int8_t *new, int udp)
{
	u_int32_t	sum;
	int		idx;

	PRECOND(csum != NULL);
	PRECOND(old != NULL);
	PRECOND(new != NULL);
	PRECOND(udp == 0 || udp == 1);

	sum = ~(((u_int32_t)csum[0] << 8) | csum[1]) & 0xffff;

	for (idx = 0; idx < 4; idx += 2) {
		sum += ~(((u_int32_t)old[idx] << 8) | old[idx + 1]) & 0xffff;
		sum += ((u_int32_t)new[idx] << 8) | new[idx + 1];
	}

	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);

	sum = ~sum & 0xffff;

	if (udp && sum == 0)
		sum = 0xffff;

	csum[0] = (sum >> 8) & 0xff;
	csum[1] = sum & 0xff;
}

/*
 * Calculate the internet checksum over the given data.
 */
static u_int16_t
inet_csum(const u_int8_t *data, size_t len)
{
	size_t		idx;
	u_int32_t	sum;

	PRECOND(data != NULL);

	sum = 0;

	for (idx = 0; idx + 1 < len; idx += 2)
		sum += ((u_int32_t)data[idx] << 8) | data[idx + 1];

	if (len & 1)
		sum += (u_int32_t)data[len - 1] << 8;

	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);

	return (~sum & 0xffff);
}
