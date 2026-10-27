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
 * An end-to-end test of graces. We start tests/pong with a simulated
 * path MTU of 1300 and check that:
 *
 *	- pong receives our heartbeats.
 *	- MTU discovery settles on 1202 (1280 minus the tunnel overhead),
 *	  the largest probe that fits the path.
 *	- a packet with DF set that is too large for that MTU is answered
 *	  with an icmp fragmentation needed carrying that MTU.
 *	- an MTU probe from pong larger than our MTU is acked with our MTU,
 *	  so a peer with a larger MTU settles on ours instead of a smaller
 *	  size it probes later.
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

#include "vigil.h"

#define TEST_PORT	"45998"
#define TEST_MTU	1202
#define TEST_PROBE	"1440"
#define TEST_SECRET	"0f1e2d3c4b5a69788796a5b4c3d2e1f0" \
			"00112233445566778899aabbccddeeff"

#define TEST_CONFIG	"vigil 1\n"				\
			"instance grace\n"			\
			"tunnel 10.77.0.2/24 auto\n"		\
			"peer 127.0.0.1:" TEST_PORT "\n"	\
			"secret " TEST_SECRET "\n"

static void		*test_run(void *);
static void		test_pong_output(void);
static int		test_wait_mtu(u_int16_t, int);
static u_int16_t	test_csum(const u_int8_t *, size_t);
static int		test_ping(int, size_t, int, u_int8_t *, size_t);

extern int	host_quiet;

static int	tun[2];
static int	out[2];
static int	result = -1;
static u_int64_t	run_id = 0;
static int	heartbeats = 0;
static int	probes = 0;
static int	acked = 0;

int
main(void)
{
	pid_t		pid;
	ssize_t		ret;
	pthread_t	thread;
	u_int8_t	reply[2048];
	int		ok, idx, status;

	host_quiet = getenv("VIGIL_TEST_VERBOSE") == NULL;

	if (pipe(out) == -1)
		fatal("pipe: %s", errno_s);

	pid = fork();
	if (pid == -1)
		fatal("fork: %s", errno_s);

	if (pid == 0) {
		if (dup2(out[1], STDOUT_FILENO) == -1)
			exit(1);

		execl("./tests/pong", "pong", TEST_PORT, TEST_SECRET,
		    "mtu", "1300", "probe", TEST_PROBE, NULL);
		exit(1);
	}

	(void)close(out[1]);
	vigil_socket_nonblock(out[0]);

	if (socketpair(AF_UNIX, SOCK_DGRAM, 0, tun) == -1)
		fatal("socketpair: %s", errno_s);

	run_id = vigil_run_id();

	if (pthread_create(&thread, NULL, test_run, NULL) != 0)
		fatal("pthread_create");

	ok = 1;

	for (idx = 0; idx < 30; idx++) {
		if (test_ping(tun[1], 64, 0, reply, sizeof(reply)) > 0)
			break;
	}

	if (idx == 30) {
		printf("FAIL tunnel did not come up\n");
		ok = 0;
	}

	if (ok && test_wait_mtu(TEST_MTU, 40) == -1) {
		printf("FAIL MTU discovery did not settle on %u\n", TEST_MTU);
		ok = 0;
	}

	if (ok) {
		ret = test_ping(tun[1], 1300, 1, reply, sizeof(reply));
		if (ret != 20 + 8 + 20 + 8 || reply[9] != 1 ||
		    reply[20] != 3 || reply[21] != 4 ||
		    ((reply[26] << 8) | reply[27]) != TEST_MTU) {
			printf("FAIL no fragmentation needed (%zd)\n", ret);
			ok = 0;
		}
	}

	if (ok && test_ping(tun[1], TEST_MTU, 1, reply, sizeof(reply)) !=
	    TEST_MTU) {
		printf("FAIL a packet of %u did not make it\n", TEST_MTU);
		ok = 0;
	}

	for (idx = 0; idx < 20 && acked == 0; idx++) {
		test_pong_output();
		(void)poll(NULL, 0, 500);
	}

	if (ok && acked != VIGIL_MTU_DEFAULT) {
		printf("FAIL pong its probe of %s was acked with %d\n",
		    TEST_PROBE, acked);
		ok = 0;
	}

	if (ok && heartbeats < 2) {
		printf("FAIL pong saw %d heartbeats\n", heartbeats);
		ok = 0;
	}

	if (ok && probes < 3) {
		printf("FAIL pong saw %d mtu probes\n", probes);
		ok = 0;
	}

	vigil_stop(run_id);
	pthread_join(thread, NULL);

	kill(pid, SIGTERM);
	(void)waitpid(pid, &status, 0);

	if (result != 0) {
		printf("FAIL vigil_run returned %d\n", result);
		return (1);
	}

	if (!ok)
		return (1);

	printf("grace_test: ok (%d heartbeats, %d probes, mtu %u)\n",
	    heartbeats, probes, TEST_MTU);

	return (0);
}

/*
 * Run the vigil core on our end of the socketpair.
 */
static void *
test_run(void *arg)
{
	result = vigil_run(run_id, tun[0], TEST_CONFIG,
	    sizeof(TEST_CONFIG) - 1, NULL, 0, NULL);

	return (NULL);
}

/*
 * Count the graces pong reported seeing.
 */
static void
test_pong_output(void)
{
	char		*p;
	ssize_t		ret;
	static size_t	off = 0;
	static char	buf[65536];

	while (off < sizeof(buf) - 1) {
		ret = read(out[0], buf + off, sizeof(buf) - 1 - off);
		if (ret <= 0)
			break;

		off += ret;
	}

	buf[off] = '\0';

	probes = 0;
	heartbeats = 0;

	for (p = buf; (p = strstr(p, "grace heartbeat")) != NULL; p++)
		heartbeats++;

	for (p = buf; (p = strstr(p, "grace mtu probe")) != NULL; p++)
		probes++;

	if ((p = strstr(buf, "grace mtu ack ")) != NULL)
		acked = atoi(p + strlen("grace mtu ack "));
}

/*
 * Wait up to the given number of seconds for the peer MTU to become mtu.
 */
static int
test_wait_mtu(u_int16_t mtu, int seconds)
{
	int		idx;
	char		*buf, want[32];

	buf = calloc(1, VIGIL_STATUS_MAX);
	if (buf == NULL)
		fatal("calloc");

	(void)snprintf(want, sizeof(want), "\"mtu\":%u,", mtu);

	for (idx = 0; idx < seconds * 4; idx++) {
		(void)vigil_status(buf, VIGIL_STATUS_MAX);
		if (strstr(buf, want) != NULL) {
			free(buf);
			return (0);
		}

		usleep(250 * 1000);
	}

	free(buf);

	return (-1);
}

/*
 * Send an icmp echo request of the given size into the tunnel and wait
 * up to a second for whatever comes back, returns its length.
 */
static int
test_ping(int fd, size_t len, int df, u_int8_t *reply, size_t replylen)
{
	struct pollfd	pfd;
	ssize_t		ret;
	u_int16_t	sum;
	u_int8_t	pkt[1500];

	VERIFY(len <= sizeof(pkt) && len >= 28);

	memset(pkt, 0, sizeof(pkt));

	pkt[0] = 0x45;
	pkt[2] = len >> 8;
	pkt[3] = len & 0xff;
	pkt[6] = df ? 0x40 : 0;
	pkt[8] = 64;
	pkt[9] = 1;
	memcpy(&pkt[12], "\x0a\x4d\x00\x02", 4);
	memcpy(&pkt[16], "\x0a\x4d\x00\x01", 4);

	sum = test_csum(pkt, 20);
	pkt[10] = sum >> 8;
	pkt[11] = sum & 0xff;

	pkt[20] = 8;
	pkt[24] = 0x6a;
	pkt[25] = 0x76;

	sum = test_csum(&pkt[20], len - 20);
	pkt[22] = sum >> 8;
	pkt[23] = sum & 0xff;

	if (write(fd, pkt, len) == -1)
		fatal("write: %s", errno_s);

	pfd.fd = fd;
	pfd.events = POLLIN;

	if (poll(&pfd, 1, 1000) != 1)
		return (0);

	ret = read(fd, reply, replylen);
	if (ret == -1)
		fatal("read: %s", errno_s);

	return (ret);
}

/*
 * The internet checksum.
 */
static u_int16_t
test_csum(const u_int8_t *data, size_t len)
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
