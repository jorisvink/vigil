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
 * vigild - run the vigil core on a regular host, without needing a
 * tun interface or root.
 *
 * The tunnel interface is one end of a socketpair, the other end is
 * handled here: icmp echo requests coming out of the tunnel are
 * answered and, with -p, we send echo requests to the given address
 * once per second and report the replies.
 *
 *	$ ./tests/vigild [-p ip] [-n count] [-w secs] <config> [datadir]
 *
 * With -n vigild exits after count replies (0) or after 60 seconds
 * without them (1). With -w it stays up for that many seconds after
 * getting its replies, without sending anything itself.
 */

#include <sys/types.h>
#include <sys/socket.h>

#include <arpa/inet.h>

#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

#include "vigil.h"

#define VIGILD_TIMEOUT		(60 * 1000)

static void	usage(void) __attribute__((noreturn));

static void		*vigild_run(void *);
static void		vigild_input(u_int8_t *, size_t);
static u_int16_t	vigild_csum(const u_int8_t *, size_t);
static void		vigild_ping(struct in_addr *, u_int16_t);

/* The tunnel side and our side of the socketpair. */
static int		tun[2];

/* The id of our run. */
static u_int64_t	run_id = 0;

/* The configuration and data directory. */
static char		config[VIGIL_CONFIG_MAX];
static size_t		config_len;
static const char	*datadir = NULL;

/* The address of our interface, learned from the config summary. */
static struct in_addr	local;

/* Number of replies we got for our echo requests. */
static int		replies = 0;

static void
usage(void)
{
	fprintf(stderr,
	    "usage: vigild [-p ip] [-n count] [-w secs] <config> [datadir]\n");
	exit(1);
}

int
main(int argc, char **argv)
{
	FILE			*fp;
	struct pollfd		pfd;
	ssize_t			ret;
	pthread_t		thread;
	struct in_addr		target;
	u_int64_t		start, next;
	char			summary[4096], *p;
	u_int8_t		pkt[KYRKA_PACKET_DATA_LEN];
	int			ch, ping, count, linger, lingering;

	ping = 0;
	count = 0;
	linger = 0;
	lingering = 0;

	while ((ch = getopt(argc, argv, "n:p:w:")) != -1) {
		switch (ch) {
		case 'w':
			linger = atoi(optarg);
			break;
		case 'n':
			count = atoi(optarg);
			break;
		case 'p':
			if (inet_pton(AF_INET, optarg, &target) != 1)
				usage();
			ping = 1;
			break;
		default:
			usage();
		}
	}

	argc -= optind;
	argv += optind;

	if (argc != 1 && argc != 2)
		usage();

	if (argc == 2)
		datadir = argv[1];

	fp = fopen(argv[0], "r");
	if (fp == NULL)
		fatal("fopen(%s): %s", argv[0], errno_s);

	config_len = fread(config, 1, sizeof(config) - 1, fp);
	fclose(fp);

	if (vigil_config_check(config, config_len,
	    summary, sizeof(summary)) == -1)
		fatal("%s", summary);

	p = strstr(summary, "\"address\":\"");
	if (p == NULL)
		fatal("no address in summary");

	p += strlen("\"address\":\"");
	p[strcspn(p, "/")] = '\0';

	if (inet_pton(AF_INET, p, &local) != 1)
		fatal("bad address in summary");

	if (socketpair(AF_UNIX, SOCK_DGRAM, 0, tun) == -1)
		fatal("socketpair: %s", errno_s);

	signal(SIGPIPE, SIG_IGN);

	run_id = vigil_run_id();

	if (pthread_create(&thread, NULL, vigild_run, NULL) != 0)
		fatal("pthread_create");

	start = vigil_time_ms();
	next = start;

	for (;;) {
		pfd.fd = tun[1];
		pfd.events = POLLIN;

		if (poll(&pfd, 1, 100) == -1) {
			if (errno == EINTR)
				continue;
			fatal("poll: %s", errno_s);
		}

		if (pfd.revents & POLLIN) {
			ret = read(tun[1], pkt, sizeof(pkt));
			if (ret == -1)
				fatal("read: %s", errno_s);

			vigild_input(pkt, ret);
		}

		if (ping && vigil_time_ms() >= next) {
			next = vigil_time_ms() + 1000;
			vigild_ping(&target, (u_int16_t)(next / 1000));
		}

		if (count > 0 && replies >= count) {
			printf("vigild: got %d replies\n", replies);
			fflush(stdout);

			if (linger == 0)
				break;

			printf("vigild: lingering for %d seconds\n", linger);
			fflush(stdout);

			ping = 0;
			count = 0;
			lingering = 1;
			start = vigil_time_ms() + (u_int64_t)linger * 1000;
		}

		if (lingering && vigil_time_ms() >= start)
			break;

		if (count > 0 && vigil_time_ms() - start > VIGILD_TIMEOUT) {
			printf("vigild: timed out with %d replies\n", replies);
			fflush(stdout);
			vigil_stop(run_id);
			pthread_join(thread, NULL);
			return (1);
		}
	}

	vigil_stop(run_id);
	pthread_join(thread, NULL);

	return (0);
}

/*
 * Run the vigil core on the tunnel side of our socketpair.
 */
static void *
vigild_run(void *arg)
{
	if (vigil_run(run_id, tun[0], config, config_len,
	    NULL, 0, datadir) == -1)
		exit(1);

	return (NULL);
}

/*
 * A packet came out of the tunnel, answer echo requests and count
 * replies to our own.
 */
static void
vigild_input(u_int8_t *pkt, size_t len)
{
	size_t		hlen;
	u_int16_t	csum;
	u_int8_t	tmp[4];

	if (len < 28 || (pkt[0] >> 4) != 4 || pkt[9] != 1)
		return;

	hlen = (pkt[0] & 0x0f) * 4;
	if (hlen < 20 || hlen + 8 > len)
		return;

	if (pkt[hlen] == 0) {
		replies++;
		printf("vigild: reply from %u.%u.%u.%u seq %u\n",
		    pkt[12], pkt[13], pkt[14], pkt[15],
		    (pkt[hlen + 6] << 8) | pkt[hlen + 7]);
		fflush(stdout);
		return;
	}

	if (pkt[hlen] != 8)
		return;

	memcpy(tmp, &pkt[12], 4);
	memcpy(&pkt[12], &pkt[16], 4);
	memcpy(&pkt[16], tmp, 4);

	pkt[8] = 64;
	pkt[10] = 0;
	pkt[11] = 0;

	csum = vigild_csum(pkt, hlen);
	pkt[10] = csum >> 8;
	pkt[11] = csum & 0xff;

	pkt[hlen] = 0;
	pkt[hlen + 2] = 0;
	pkt[hlen + 3] = 0;

	csum = vigild_csum(&pkt[hlen], len - hlen);
	pkt[hlen + 2] = csum >> 8;
	pkt[hlen + 3] = csum & 0xff;

	if (write(tun[1], pkt, len) == -1)
		fatal("write: %s", errno_s);
}

/*
 * Send an icmp echo request to the given address into the tunnel.
 */
static void
vigild_ping(struct in_addr *dst, u_int16_t seq)
{
	u_int16_t	csum;
	u_int8_t	pkt[64];

	memset(pkt, 0, sizeof(pkt));

	pkt[0] = 0x45;
	pkt[3] = sizeof(pkt);
	pkt[8] = 64;
	pkt[9] = 1;
	memcpy(&pkt[12], &local.s_addr, 4);
	memcpy(&pkt[16], &dst->s_addr, 4);

	csum = vigild_csum(pkt, 20);
	pkt[10] = csum >> 8;
	pkt[11] = csum & 0xff;

	pkt[20] = 8;
	pkt[24] = 0x70;
	pkt[25] = 0x72;
	pkt[26] = seq >> 8;
	pkt[27] = seq & 0xff;

	csum = vigild_csum(&pkt[20], sizeof(pkt) - 20);
	pkt[22] = csum >> 8;
	pkt[23] = csum & 0xff;

	if (write(tun[1], pkt, sizeof(pkt)) == -1)
		fatal("write: %s", errno_s);
}

/*
 * The internet checksum.
 */
static u_int16_t
vigild_csum(const u_int8_t *data, size_t len)
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
