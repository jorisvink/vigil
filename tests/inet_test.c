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

/*
 * Tests for the ipv4 address rewriting, the incrementally updated
 * checksums are compared against fully recomputed ones.
 */

#include <sys/types.h>

#include <arpa/inet.h>

#include <stdio.h>

#include "vigil.h"

#include <libkyrka/libnyfe.h>

static u_int16_t	csum(const u_int8_t *, size_t, u_int32_t);

static u_int16_t	l4_csum(const u_int8_t *, size_t);

static void	build(u_int8_t *, size_t, u_int8_t, u_int16_t);

static void	check(const char *, u_int8_t *, size_t, u_int8_t);

static int		failures = 0;

int
main(void)
{
	size_t		ret;
	struct in_addr	addr, prefix;
	int		round, proto;
	char		str[INET_ADDRSTRLEN];
	u_int8_t	pkt[256], icmp[128], protos[] = { 1, 6, 17 };

	nyfe_random_init();

	(void)inet_pton(AF_INET, "172.31.0.0", &prefix);
	vigil_inet_liturgy(&addr, &prefix, 0x0c, 0x0d);
	(void)inet_ntop(AF_INET, &addr, str, sizeof(str));

	if (strcmp(str, "172.31.12.13")) {
		printf("FAIL liturgy address: %s\n", str);
		failures++;
	}

	for (round = 0; round < 2000; round++) {
		for (proto = 0; proto < 3; proto++) {
			build(pkt, sizeof(pkt), protos[proto], 0);

			nyfe_random_bytes(&addr, sizeof(addr));

			if (vigil_inet_rewrite_src(pkt,
			    sizeof(pkt), &addr) == -1) {
				printf("FAIL rewrite_src\n");
				failures++;
			}

			check("src", pkt, sizeof(pkt), protos[proto]);
			nyfe_random_bytes(&addr, sizeof(addr));

			if (vigil_inet_rewrite_dst(pkt,
			    sizeof(pkt), &addr) == -1) {
				printf("FAIL rewrite_dst\n");
				failures++;
			}

			check("dst", pkt, sizeof(pkt), protos[proto]);
		}
	}

	/* A fragment must only have its ip header checksum updated. */
	build(pkt, sizeof(pkt), 17, 0x0010);
	memcpy(&addr, "\x0a\x00\x00\x01", 4);
	if (vigil_inet_rewrite_dst(pkt, sizeof(pkt), &addr) == -1) {
		printf("FAIL fragment rewrite\n");
		failures++;
	}

	if (csum(pkt, 20, 0) != 0) {
		printf("FAIL fragment ip checksum\n");
		failures++;
	}

	/* Garbage must be refused. */
	memset(pkt, 0, sizeof(pkt));

	if (vigil_inet_rewrite_dst(pkt, sizeof(pkt), &addr) != -1 ||
	    vigil_inet_ipv4_dst(pkt, 10, &addr) != -1) {
		printf("FAIL garbage accepted\n");
		failures++;
	}

	/* An icmp fragmentation needed for a too large DF packet. */
	build(pkt, sizeof(pkt), 6, 0x4000);

	ret = vigil_inet_icmp_frag(pkt, sizeof(pkt),
	    1202, icmp, sizeof(icmp));
	if (ret != 20 + 8 + 20 + 8 || icmp[9] != 1 || icmp[20] != 3 ||
	    icmp[21] != 4 || ((icmp[26] << 8) | icmp[27]) != 1202 ||
	    csum(icmp, 20, 0) != 0 || csum(&icmp[20], ret - 20, 0) != 0 ||
	    memcmp(&icmp[12], &pkt[16], 4) || memcmp(&icmp[16], &pkt[12], 4) ||
	    memcmp(&icmp[28], pkt, 28)) {
		printf("FAIL icmp fragmentation needed\n");
		failures++;
	}

	/* No icmp for packets that may be fragmented. */
	build(pkt, sizeof(pkt), 6, 0);

	if (vigil_inet_icmp_frag(pkt, sizeof(pkt), 1202,
	    icmp, sizeof(icmp)) != 0) {
		printf("FAIL icmp for a packet without DF\n");
		failures++;
	}

	if (failures != 0) {
		printf("inet_test: %d failures\n", failures);
		return (1);
	}

	printf("inet_test: ok\n");

	return (0);
}

/*
 * Build a random packet of the given protocol with valid checksums.
 */
static void
build(u_int8_t *pkt, size_t len, u_int8_t proto, u_int16_t frag)
{
	u_int16_t	sum;

	nyfe_random_bytes(pkt, len);

	pkt[0] = 0x45;
	pkt[2] = len >> 8;
	pkt[3] = len & 0xff;
	pkt[6] = frag >> 8;
	pkt[7] = frag & 0xff;
	pkt[9] = proto;
	pkt[10] = 0;
	pkt[11] = 0;

	sum = csum(pkt, 20, 0);
	pkt[10] = sum >> 8;
	pkt[11] = sum & 0xff;

	switch (proto) {
	case 6:
		pkt[36] = 0;
		pkt[37] = 0;

		sum = l4_csum(pkt, len);
		pkt[36] = sum >> 8;
		pkt[37] = sum & 0xff;
		break;
	case 17:
		pkt[24] = (len - 20) >> 8;
		pkt[25] = (len - 20) & 0xff;
		pkt[26] = 0;
		pkt[27] = 0;

		sum = l4_csum(pkt, len);
		if (sum == 0)
			sum = 0xffff;
		pkt[26] = sum >> 8;
		pkt[27] = sum & 0xff;
		break;
	}
}

/*
 * Verify that the checksums in the packet are still correct.
 */
static void
check(const char *name, u_int8_t *pkt, size_t len, u_int8_t proto)
{
	if (csum(pkt, 20, 0) != 0) {
		printf("FAIL %s: ip checksum (proto %u)\n", name, proto);
		failures++;
	}

	if (proto == 1)
		return;

	if (l4_csum(pkt, len) != 0) {
		printf("FAIL %s: l4 checksum (proto %u)\n", name, proto);
		failures++;
	}
}

/*
 * The tcp/udp checksum including the pseudo header, 0 if valid.
 */
static u_int16_t
l4_csum(const u_int8_t *pkt, size_t len)
{
	int		idx;
	u_int32_t	pseudo;

	pseudo = pkt[9] + (len - 20);

	for (idx = 12; idx < 20; idx += 2)
		pseudo += ((u_int32_t)pkt[idx] << 8) | pkt[idx + 1];

	return (csum(&pkt[20], len - 20, pseudo));
}

/*
 * The internet checksum over data, seeded with sum.
 */
static u_int16_t
csum(const u_int8_t *data, size_t len, u_int32_t sum)
{
	size_t		idx;

	for (idx = 0; idx + 1 < len; idx += 2)
		sum += ((u_int32_t)data[idx] << 8) | data[idx + 1];

	if (len & 1)
		sum += (u_int32_t)data[len - 1] << 8;

	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);

	return (~sum & 0xffff);
}
