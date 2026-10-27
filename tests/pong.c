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
 * pong - a tiny sanctum peer built on libkyrka that answers icmp echo
 * requests coming out of its tunnel, without needing a tun interface.
 *
 * Used to test a tunnel end-to-end, both by tests/tunnel_test and by
 * pointing the apps at it:
 *
 *	$ ./tests/pong 4500 <64 hex characters secret> [shroud] [mtu <n>]
 *	    [probe <n>]
 *
 * The peer address is learned from authenticated packets.
 *
 * Graces are handled the way sanctum handles them: heartbeats are
 * logged and MTU probes are acked, but only up to the given mtu which
 * simulates the path MTU between us and our peer. With probe we send
 * an MTU probe of that size on every heartbeat until our peer acks it.
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/socket.h>

#include <arpa/inet.h>
#include <netinet/in.h>

#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libkyrka/libkyrka.h>

#define errno_s		strerror(errno)

static void	usage(void) __attribute__((noreturn));

static void	fatal(const char *, ...) __attribute__((noreturn))
		    __attribute__((format (printf, 1, 2)));

static void		pong_probe(void);
static void		pong_icmp(struct kyrka_packet *);
static void		pong_grace(struct kyrka_packet *);
static u_int16_t	pong_csum(const u_int8_t *, size_t);
static void		pong_secret(const char *, u_int8_t *, size_t);
static void		pong_kyrka_event(KYRKA *, union kyrka_event *, void *);
static void		pong_heaven_input(struct kyrka_packet *, u_int64_t,
			    void *);
static void		pong_purgatory_input(struct kyrka_packet *, u_int64_t,
			    void *);

/* Our socket and libkyrka context. */
static int			fd = -1;
static KYRKA			*ctx = NULL;

/* The peer address, learned from authenticated packets. */
static struct sockaddr_in	peer;
static struct sockaddr_in	sender;

/* Number of echo requests answered. */
static u_int64_t		answered = 0;

/* The largest MTU probe we ack. */
static u_int16_t		path_mtu = 1500;

/* The MTU probe we send, and if our peer acked it. */
static u_int16_t		probe_size = 0;
static int			probe_acked = 0;

static void
usage(void)
{
	fprintf(stderr,
	    "usage: pong <port> <secret-hex> [shroud] [mtu <n>] "
	    "[probe <n>]\n");
	exit(1);
}

int
main(int argc, char **argv)
{
	struct pollfd		pfd;
	struct sockaddr_in	sin;
	struct kyrka_packet	pkt;
	ssize_t			ret;
	size_t			len;
	socklen_t		socklen;
	int			idx, shroud;
	u_int8_t		*ptr, secret[32];

	if (argc < 3)
		usage();

	shroud = 0;

	for (idx = 3; idx < argc; idx++) {
		if (!strcmp(argv[idx], "shroud")) {
			shroud = 1;
		} else if (!strcmp(argv[idx], "mtu") && idx + 1 < argc) {
			path_mtu = atoi(argv[++idx]);
		} else if (!strcmp(argv[idx], "probe") && idx + 1 < argc) {
			probe_size = atoi(argv[++idx]);
		} else {
			usage();
		}
	}

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd == -1)
		fatal("socket: %s", errno_s);

	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_port = htons(atoi(argv[1]));

	if (bind(fd, (const struct sockaddr *)&sin, sizeof(sin)) == -1)
		fatal("bind: %s", errno_s);

	pong_secret(argv[2], secret, sizeof(secret));

	ctx = kyrka_ctx_alloc(pong_kyrka_event, NULL);
	if (ctx == NULL)
		fatal("kyrka_ctx_alloc");

	if (kyrka_secret_load(ctx, secret, sizeof(secret)) == -1)
		fatal("kyrka_secret_load: %d", kyrka_last_error(ctx));

	memset(secret, 0, sizeof(secret));

	if (shroud && kyrka_shroud_enable(ctx) == -1)
		fatal("kyrka_shroud_enable: %d", kyrka_last_error(ctx));

	if (kyrka_heaven_ifc(ctx, pong_heaven_input, NULL) == -1)
		fatal("kyrka_heaven_ifc: %d", kyrka_last_error(ctx));

	if (kyrka_purgatory_ifc(ctx, pong_purgatory_input, NULL) == -1)
		fatal("kyrka_purgatory_ifc: %d", kyrka_last_error(ctx));

	if (kyrka_mtu_size(ctx, 1408) == -1)
		fatal("kyrka_mtu_size: %d", kyrka_last_error(ctx));

	memset(&peer, 0, sizeof(peer));

	printf("pong listening on port %s\n", argv[1]);
	fflush(stdout);

	for (;;) {
		pfd.fd = fd;
		pfd.events = POLLIN;

		if (poll(&pfd, 1, 1000) == -1) {
			if (errno == EINTR)
				continue;
			fatal("poll: %s", errno_s);
		}

		if (kyrka_key_manage(ctx) == -1 &&
		    kyrka_last_error(ctx) != KYRKA_ERROR_NO_CONFIG)
			printf("kyrka_key_manage: %d\n", kyrka_last_error(ctx));

		if (!(pfd.revents & POLLIN))
			continue;

		ptr = kyrka_packet_recvbuf(ctx, &pkt, &len);
		if (ptr == NULL)
			fatal("kyrka_packet_recvbuf");

		socklen = sizeof(sender);

		ret = recvfrom(fd, ptr, len, 0,
		    (struct sockaddr *)&sender, &socklen);
		if (ret == -1) {
			if (errno == EINTR)
				continue;
			fatal("recvfrom: %s", errno_s);
		}

		if (peer.sin_port == 0)
			memcpy(&peer, &sender, sizeof(sender));

		pkt.length = ret;
		pkt.shroud = KYRKA_PACKET_SHROUD_PEER;

		if (kyrka_purgatory_input(ctx, &pkt) == -1 &&
		    kyrka_last_error(ctx) != KYRKA_ERROR_NO_RX_KEY)
			printf("purgatory_input: %d\n", kyrka_last_error(ctx));
	}

	return (0);
}

/*
 * Decode the hex encoded secret.
 */
static void
pong_secret(const char *hex, u_int8_t *out, size_t len)
{
	size_t		idx;
	unsigned int	byte;

	if (strlen(hex) != len * 2)
		fatal("secret must be %zu hex characters", len * 2);

	for (idx = 0; idx < len; idx++) {
		if (!isxdigit((unsigned char)hex[idx * 2]) ||
		    !isxdigit((unsigned char)hex[idx * 2 + 1]))
			fatal("secret is not hex");

		if (sscanf(&hex[idx * 2], "%2x", &byte) != 1)
			fatal("secret is not hex");

		out[idx] = byte;
	}
}

/*
 * A libkyrka event, we just log it.
 */
static void
pong_kyrka_event(KYRKA *kctx, union kyrka_event *evt, void *udata)
{
	switch (evt->type) {
	case KYRKA_EVENT_KEYS_INFO:
		printf("keys tx=%08x rx=%08x\n",
		    evt->keys.tx_spi, evt->keys.rx_spi);
		break;
	case KYRKA_EVENT_EXCHANGE_INFO:
		printf("exchange: %s\n", evt->exchange.reason);
		break;
	case KYRKA_EVENT_LOGMSG:
		printf("log: %s\n", evt->logmsg.log);
		break;
	}

	fflush(stdout);
}

/*
 * A packet came out of the tunnel, its sender is authenticated so we
 * learn its address and answer it if it is an icmp echo request.
 */
static void
pong_heaven_input(struct kyrka_packet *pkt, u_int64_t seq, void *udata)
{
	memcpy(&peer, &sender, sizeof(sender));

	if (pkt->type == KYRKA_PACKET_TYPE_GRACE)
		pong_grace(pkt);
	else
		pong_icmp(pkt);
}

/*
 * A grace arrived, log heartbeats and ack MTU probes that fit our
 * simulated path MTU like sanctum does.
 */
static void
pong_grace(struct kyrka_packet *in)
{
	struct kyrka_packet	pkt;
	size_t			len;
	u_int16_t		hdr[2], *out;

	if (in->length < sizeof(hdr[0]))
		return;

	memcpy(hdr, kyrka_packet_data(in), MIN(in->length, sizeof(hdr)));

	switch (hdr[0]) {
	case 1:
		printf("grace heartbeat\n");

		if (probe_size != 0 && probe_acked == 0)
			pong_probe();
		break;
	case 2:
		if (in->length < sizeof(hdr) || hdr[1] != in->length)
			return;

		printf("grace mtu probe %u\n", hdr[1]);

		if (hdr[1] > path_mtu)
			break;

		out = kyrka_packet_databuf(ctx, &pkt, &len);
		if (out == NULL)
			fatal("kyrka_packet_databuf");

		out[0] = 3;
		out[1] = hdr[1];

		pkt.length = sizeof(hdr);
		pkt.type = KYRKA_PACKET_TYPE_GRACE;
		pkt.shroud = KYRKA_PACKET_SHROUD_PEER;

		if (kyrka_heaven_input(ctx, &pkt) == -1) {
			printf("kyrka_heaven_input: %d\n",
			    kyrka_last_error(ctx));
		}
		break;
	case 3:
		if (in->length < sizeof(hdr))
			return;

		printf("grace mtu ack %u\n", hdr[1]);
		probe_acked = 1;
		break;
	default:
		printf("grace unknown type %u\n", hdr[0]);
		break;
	}

	fflush(stdout);
}

/*
 * Send our peer an MTU probe of probe_size, padded to that size.
 */
static void
pong_probe(void)
{
	struct kyrka_packet	pkt;
	size_t			len;
	u_int16_t		*out;

	out = kyrka_packet_databuf(ctx, &pkt, &len);
	if (out == NULL)
		fatal("kyrka_packet_databuf");

	if (probe_size > len)
		fatal("probe of %u does not fit", probe_size);

	memset(out, 0, probe_size);
	out[0] = 2;
	out[1] = probe_size;

	pkt.length = probe_size;
	pkt.type = KYRKA_PACKET_TYPE_GRACE;
	pkt.shroud = KYRKA_PACKET_SHROUD_PEER;

	if (kyrka_heaven_input(ctx, &pkt) == -1)
		printf("kyrka_heaven_input: %d\n", kyrka_last_error(ctx));
}

/*
 * Encrypted data is ready to be sent to our peer.
 */
static void
pong_purgatory_input(struct kyrka_packet *pkt, u_int64_t seq, void *udata)
{
	size_t		len;
	u_int8_t	*data;

	if (peer.sin_port == 0)
		return;

	data = kyrka_packet_sendbuf(ctx, pkt, &len);
	if (data == NULL)
		fatal("kyrka_packet_sendbuf");

	if (sendto(fd, data, len, 0,
	    (const struct sockaddr *)&peer, sizeof(peer)) == -1)
		printf("sendto: %s\n", errno_s);
}

/*
 * Turn an icmp echo request into a reply and send it back.
 */
static void
pong_icmp(struct kyrka_packet *in)
{
	struct kyrka_packet	pkt;
	u_int16_t		csum;
	size_t			hlen, len;
	u_int8_t		*ip, *out, tmp[4];

	ip = kyrka_packet_data(in);

	if (in->length < 28 || (ip[0] >> 4) != 4 || ip[9] != 1)
		return;

	hlen = (ip[0] & 0x0f) * 4;
	if (hlen < 20 || hlen + 8 > in->length || ip[hlen] != 8)
		return;

	out = kyrka_packet_databuf(ctx, &pkt, &len);
	if (out == NULL)
		fatal("kyrka_packet_databuf");

	if (in->length > len)
		return;

	memcpy(out, ip, in->length);

	memcpy(tmp, &out[12], 4);
	memcpy(&out[12], &out[16], 4);
	memcpy(&out[16], tmp, 4);

	out[8] = 64;
	out[10] = 0;
	out[11] = 0;

	csum = pong_csum(out, hlen);
	out[10] = csum >> 8;
	out[11] = csum & 0xff;

	out[hlen] = 0;
	out[hlen + 2] = 0;
	out[hlen + 3] = 0;

	csum = pong_csum(&out[hlen], in->length - hlen);
	out[hlen + 2] = csum >> 8;
	out[hlen + 3] = csum & 0xff;

	pkt.length = in->length;
	pkt.shroud = KYRKA_PACKET_SHROUD_PEER;

	if (kyrka_heaven_input(ctx, &pkt) == -1) {
		printf("kyrka_heaven_input: %d\n", kyrka_last_error(ctx));
		return;
	}

	answered++;
	printf("pong %llu\n", (unsigned long long)answered);
	fflush(stdout);
}

/*
 * The internet checksum.
 */
static u_int16_t
pong_csum(const u_int8_t *data, size_t len)
{
	size_t		idx;
	u_int32_t	sum;

	sum = 0;

	for (idx = 0; idx + 1 < len; idx += 2)
		sum += ((u_int32_t)data[idx] << 8) | data[idx + 1];

	if (len & 1)
		sum += (u_int32_t)data[len - 1] << 8;

	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);

	return (~sum & 0xffff);
}

static void
fatal(const char *fmt, ...)
{
	va_list		args;

	kyrka_emergency_erase();

	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);

	fprintf(stderr, "\n");
	exit(1);
}
