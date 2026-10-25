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
 * An end-to-end test of a direct tunnel. We start tests/pong as our
 * peer, run the vigil core on one end of a socketpair that acts as
 * the tunnel interface and ping through it.
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

#include "vigil.h"

#define TEST_PORT	"45999"
#define TEST_SECRET	"8f2b1c3d4e5f60718293a4b5c6d7e8f9" \
			"0123456789abcdef0123456789abcdef"

#define TEST_CONFIG	"vigil 1\n"				\
			"instance test\n"			\
			"tunnel 10.77.0.2/24\n"			\
			"peer 127.0.0.1:" TEST_PORT "\n"	\
			"secret " TEST_SECRET "\n"		\
			"route 10.77.0.0/24\n"

static void		*test_run(void *);
static int		test_ping(int, u_int16_t);
static u_int16_t	test_csum(const u_int8_t *, size_t);

extern int	host_quiet;

static int	tun[2];
static int	result = -1;
static u_int64_t	run_id = 0;

int
main(void)
{
	pid_t		pid;
	u_int16_t	seq;
	char		*buf;
	pthread_t	thread;
	int		replies, status;

	host_quiet = getenv("VIGIL_TEST_VERBOSE") == NULL;

	pid = fork();
	if (pid == -1)
		fatal("fork: %s", errno_s);

	if (pid == 0) {
		if (host_quiet && freopen("/dev/null", "w", stdout) == NULL)
			exit(1);

		execl("./tests/pong", "pong", TEST_PORT, TEST_SECRET, NULL);
		exit(1);
	}

	if (socketpair(AF_UNIX, SOCK_DGRAM, 0, tun) == -1)
		fatal("socketpair: %s", errno_s);

	run_id = vigil_run_id();

	if (pthread_create(&thread, NULL, test_run, NULL) != 0)
		fatal("pthread_create");

	replies = 0;

	for (seq = 1; seq <= 30 && replies < 3; seq++) {
		if (test_ping(tun[1], seq) == 0)
			replies++;
	}

	buf = calloc(1, VIGIL_STATUS_MAX);
	if (buf == NULL)
		fatal("calloc");

	(void)vigil_status(buf, VIGIL_STATUS_MAX);

	if (strstr(buf, "\"online\":true") == NULL) {
		printf("FAIL tunnel not online: %s\n", buf);
		replies = 0;
	}

	vigil_stop(run_id);
	pthread_join(thread, NULL);

	(void)vigil_status(buf, VIGIL_STATUS_MAX);

	if (strncmp(buf, "{\"running\":false", 16)) {
		printf("FAIL still running after stop\n");
		replies = 0;
	}

	free(buf);

	kill(pid, SIGTERM);
	(void)waitpid(pid, &status, 0);

	if (result != 0) {
		printf("FAIL vigil_run returned %d\n", result);
		return (1);
	}

	if (replies < 3) {
		printf("FAIL only got %d replies\n", replies);
		return (1);
	}

	printf("tunnel_test: ok (%d replies, %u pings)\n", replies, seq - 1);

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
 * Send a single icmp echo request into the tunnel and wait up to a
 * second for its reply. Returns 0 if we got a matching reply.
 */
static int
test_ping(int fd, u_int16_t seq)
{
	struct pollfd	pfd;
	ssize_t		ret;
	u_int16_t	sum;
	u_int8_t	pkt[64], reply[2048];

	memset(pkt, 0, sizeof(pkt));

	pkt[0] = 0x45;
	pkt[3] = sizeof(pkt);
	pkt[8] = 64;
	pkt[9] = 1;
	memcpy(&pkt[12], "\x0a\x4d\x00\x02", 4);
	memcpy(&pkt[16], "\x0a\x4d\x00\x01", 4);

	sum = test_csum(pkt, 20);
	pkt[10] = sum >> 8;
	pkt[11] = sum & 0xff;

	pkt[20] = 8;
	pkt[24] = 0x13;
	pkt[25] = 0x37;
	pkt[26] = seq >> 8;
	pkt[27] = seq & 0xff;
	memcpy(&pkt[28], "vigil says hello", 17);

	sum = test_csum(&pkt[20], sizeof(pkt) - 20);
	pkt[22] = sum >> 8;
	pkt[23] = sum & 0xff;

	if (write(fd, pkt, sizeof(pkt)) == -1)
		fatal("write: %s", errno_s);

	pfd.fd = fd;
	pfd.events = POLLIN;

	while (poll(&pfd, 1, 1000) == 1) {
		ret = read(fd, reply, sizeof(reply));
		if (ret == -1)
			fatal("read: %s", errno_s);

		if (ret != sizeof(pkt))
			continue;

		if (reply[20] != 0 || reply[26] != pkt[26] ||
		    reply[27] != pkt[27])
			continue;

		if (memcmp(&reply[12], &pkt[16], 4) ||
		    memcmp(&reply[16], &pkt[12], 4))
			continue;

		if (test_csum(reply, 20) != 0 ||
		    test_csum(&reply[20], ret - 20) != 0)
			continue;

		return (0);
	}

	return (-1);
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
