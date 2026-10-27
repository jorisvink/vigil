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
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#include <inttypes.h>
#include <poll.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include "vigil.h"

#define TEST_MARKER	"vigil-link-test-plaintext"
#define TEST_BIG	"big"
#define TEST_QUIT	0xff
#define TEST_REOPEN	0xfe
#define TEST_PING	"ping"

/* The SA soft and hard lifetimes in libkyrka. */
#define TEST_SA_SOFT	3500
#define TEST_SA_HARD	3600

static void		test_child(int, const u_int8_t *);
static int		test_keys(int);
static int		test_ping(int, int, int);
static void		test_send(int, const u_int8_t *, size_t);
static size_t		test_recv(int, u_int8_t *, size_t);
static void		test_fill(u_int8_t *, size_t);
static void		test_secret(u_int8_t *, u_int8_t);
static int		test_seen(const u_int8_t *, size_t);
static u_int64_t	test_sum(const u_int8_t *, size_t);
static ssize_t		test_request(int, const u_int8_t *, size_t, int);

extern int	host_quiet;

static u_int8_t		frame[VIGIL_LINK_BATCH_MAX + 1];
static u_int8_t		reply[VIGIL_LINK_BATCH_MAX + 1];
static u_int8_t		answer[VIGIL_LINK_MESSAGE_MAX];
static u_int8_t		saved[VIGIL_LINK_BATCH_MAX + 1];
static size_t		saved_len = 0;
static volatile time_t	*skew = NULL;
static int		child_ready = 0;
static int		leaked = 0;

int
main(void)
{
	ssize_t			len;
	pid_t			pid;
	u_int8_t		*data;
	int			fds[2], status;
	char			want[64];
	u_int8_t		wrong[VIGIL_LINK_SECRET_LEN];
	u_int8_t		secret[VIGIL_LINK_SECRET_LEN];

	host_quiet = 1;

	skew = mmap(NULL, sizeof(*skew), PROT_READ | PROT_WRITE,
	    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (skew == MAP_FAILED)
		fatal("mmap: %s", errno_s);

	*skew = 0;

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == -1)
		fatal("socketpair: %s", errno_s);

	test_secret(secret, 0x5a);
	test_secret(wrong, 0xa5);

	if ((pid = fork()) == -1)
		fatal("fork: %s", errno_s);

	if (pid == 0) {
		(void)close(fds[0]);
		if (vigil_link_open(secret, sizeof(secret)) == -1)
			fatal("child: vigil_link_open");
		test_child(fds[1], secret);
		exit(0);
	}

	(void)close(fds[1]);

	if (vigil_link_open(wrong, sizeof(wrong)) == -1)
		fatal("vigil_link_open");

	if (test_keys(fds[0]) != -1) {
		printf("FAIL keys with the wrong secret\n");
		return (1);
	}

	if (vigil_link_open(secret, sizeof(secret)) == -1)
		fatal("vigil_link_open");

	if (test_keys(fds[0]) == -1) {
		printf("FAIL no keys with the right secret\n");
		return (1);
	}

	if ((data = malloc(VIGIL_LINK_MESSAGE_MAX)) == NULL)
		fatal("malloc");

	test_fill(data, VIGIL_CONFIG_MAX);
	(void)snprintf(want, sizeof(want), "%d %016" PRIx64,
	    VIGIL_CONFIG_MAX, test_sum(data, VIGIL_CONFIG_MAX));

	len = test_request(fds[0], data, VIGIL_CONFIG_MAX, 1);
	if (len != (ssize_t)strlen(want) || memcmp(answer, want, len)) {
		printf("FAIL the request did not arrive intact\n");
		return (1);
	}

	test_fill(data, VIGIL_LINK_MESSAGE_MAX);

	len = test_request(fds[0], (const u_int8_t *)TEST_BIG,
	    sizeof(TEST_BIG) - 1, 0);
	if (len != VIGIL_LINK_MESSAGE_MAX ||
	    test_sum(answer, len) != test_sum(data, VIGIL_LINK_MESSAGE_MAX)) {
		printf("FAIL the large reply did not arrive intact\n");
		return (1);
	}

	if (test_request(fds[0], data, 512, -1) != 0) {
		printf("FAIL a tampered request was answered\n");
		return (1);
	}

	test_send(fds[0], saved, saved_len);
	len = test_recv(fds[0], reply, sizeof(reply));

	if (vigil_link_input(&reply[1], len - 1, answer, sizeof(answer)) != 0) {
		printf("FAIL a replayed request was answered\n");
		return (1);
	}

	if (test_request(fds[0], data, 512, 0) <= 0) {
		printf("FAIL the link did not carry on\n");
		return (1);
	}

	*skew += TEST_SA_SOFT - 10;

	if (test_ping(fds[0], 200, 2) == -1) {
		printf("FAIL the link did not rekey past the SA limits\n");
		return (1);
	}

	*skew += TEST_SA_HARD + 100;

	if (test_ping(fds[0], 20, 5) == -1) {
		printf("FAIL the link did not recover after being idle\n");
		return (1);
	}

	frame[0] = TEST_REOPEN;
	test_send(fds[0], frame, 1);
	(void)test_recv(fds[0], reply, sizeof(reply));

	if (test_ping(fds[0], 20, 5) == -1) {
		printf("FAIL the link did not recover after a reopen\n");
		return (1);
	}

	if (leaked) {
		printf("FAIL plaintext seen on the socketpair\n");
		return (1);
	}

	frame[0] = TEST_QUIT;
	test_send(fds[0], frame, 1);

	if (waitpid(pid, &status, 0) == -1)
		fatal("waitpid: %s", errno_s);

	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		printf("FAIL the child exited badly\n");
		return (1);
	}

	vigil_link_close();
	free(data);

	printf("link_test: ok\n");

	return (0);
}

/*
 * The provider side, a request for TEST_BIG is answered with a message
 * of VIGIL_LINK_MESSAGE_MAX, any other with its length and sum.
 */
static void
test_child(int fd, const u_int8_t *secret)
{
	ssize_t		n, len;
	u_int8_t	*msg, *big;
	size_t		rlen;
	char		ack[64];

	if ((msg = malloc(VIGIL_LINK_MESSAGE_MAX)) == NULL)
		fatal("malloc");

	if ((big = malloc(VIGIL_LINK_MESSAGE_MAX)) == NULL)
		fatal("malloc");

	test_fill(big, VIGIL_LINK_MESSAGE_MAX);

	for (;;) {
		n = test_recv(fd, frame, sizeof(frame));

		if (frame[0] == TEST_QUIT)
			break;

		if (frame[0] == TEST_REOPEN) {
			if (vigil_link_open(secret, VIGIL_LINK_SECRET_LEN) == -1)
				fatal("child: vigil_link_open");
			reply[0] = 0;
			test_send(fd, reply, 1);
			continue;
		}

		len = vigil_link_input(&frame[1], n - 1, msg,
		    VIGIL_LINK_MESSAGE_MAX);

		if (len == sizeof(TEST_BIG) - 1 &&
		    !memcmp(msg, TEST_BIG, len)) {
			n = vigil_link_output(big, VIGIL_LINK_MESSAGE_MAX,
			    &reply[1], VIGIL_LINK_BATCH_MAX);
		} else if (len > 0) {
			(void)snprintf(ack, sizeof(ack), "%zd %016" PRIx64,
			    len, test_sum(msg, len));
			n = vigil_link_output((const u_int8_t *)ack,
			    strlen(ack), &reply[1], VIGIL_LINK_BATCH_MAX);
		} else {
			n = vigil_link_output(NULL, 0, &reply[1],
			    VIGIL_LINK_BATCH_MAX);
		}

		if (n == -1)
			fatal("child: vigil_link_output");

		rlen = n;
		reply[0] = vigil_link_ready();

		test_send(fd, reply, rlen + 1);
	}

	vigil_link_close();

	free(msg);
	free(big);
}

/*
 * Pump empty requests until both sides have keys, for about 5 seconds.
 */
static int
test_keys(int fd)
{
	int		idx;

	for (idx = 0; idx < 100; idx++) {
		(void)test_request(fd, NULL, 0, 0);

		if (child_ready && vigil_link_ready())
			return (0);

		(void)poll(NULL, 0, 50);
	}

	return (-1);
}

/*
 * Ping once a second for the given rounds, a ping may go unanswered at
 * most lost times and the last 10 rounds must all be answered.
 */
static int
test_ping(int fd, int rounds, int lost)
{
	int		idx, missed;

	missed = 0;

	for (idx = 0; idx < rounds; idx++) {
		*skew += 1;

		if (test_request(fd, (const u_int8_t *)TEST_PING,
		    sizeof(TEST_PING) - 1, 0) > 0)
			continue;

		if (idx >= rounds - 10)
			return (-1);

		if (++missed > lost)
			return (-1);
	}

	return (0);
}

/*
 * Our clock, skewed by the test.
 */
int
clock_gettime(clockid_t clk, struct timespec *ts)
{
	int		ret;

	ret = syscall(SYS_clock_gettime, clk, ts);

	if (ret == 0 && skew != NULL)
		ts->tv_sec += *skew;

	return (ret);
}

/*
 * Send one request and take the message in its reply into answer. With
 * keep the request batch is saved, with -1 it is tampered with first.
 */
static ssize_t
test_request(int fd, const u_int8_t *msg, size_t len, int keep)
{
	ssize_t		n;
	u_int16_t	plen;

	n = vigil_link_output(msg, len, &frame[1], VIGIL_LINK_BATCH_MAX);
	if (n == -1)
		n = vigil_link_output(NULL, 0, &frame[1], VIGIL_LINK_BATCH_MAX);

	if (n == -1)
		fatal("vigil_link_output");

	if (test_seen(&frame[1], n))
		leaked = 1;

	frame[0] = 0;

	if (keep == 1) {
		memcpy(saved, frame, n + 1);
		saved_len = n + 1;
	}

	if (keep == -1 && n > (ssize_t)sizeof(plen)) {
		memcpy(&plen, &frame[1], sizeof(plen));
		frame[1 + sizeof(plen) + be16toh(plen) / 2] ^= 0x01;
	}

	test_send(fd, frame, n + 1);
	n = test_recv(fd, reply, sizeof(reply));

	if (test_seen(&reply[1], n - 1))
		leaked = 1;

	child_ready = reply[0];

	return (vigil_link_input(&reply[1], n - 1, answer, sizeof(answer)));
}

/*
 * Send a frame, preceded by its length.
 */
static void
test_send(int fd, const u_int8_t *data, size_t len)
{
	ssize_t		ret;
	u_int32_t	flen;
	size_t		off;

	flen = htobe32(len);
	if (write(fd, &flen, sizeof(flen)) != sizeof(flen))
		fatal("write: %s", errno_s);

	for (off = 0; off < len; off += ret) {
		if ((ret = write(fd, &data[off], len - off)) <= 0)
			fatal("write: %s", errno_s);
	}
}

/*
 * Receive a frame, returns its length.
 */
static size_t
test_recv(int fd, u_int8_t *data, size_t max)
{
	ssize_t		ret;
	u_int32_t	flen;
	size_t		off, len;

	for (off = 0; off < sizeof(flen); off += ret) {
		if ((ret = read(fd, (u_int8_t *)&flen + off,
		    sizeof(flen) - off)) <= 0)
			fatal("read: %s", errno_s);
	}

	len = be32toh(flen);
	if (len == 0 || len > max)
		fatal("bad frame length %zu", len);

	for (off = 0; off < len; off += ret) {
		if ((ret = read(fd, &data[off], len - off)) <= 0)
			fatal("read: %s", errno_s);
	}

	return (len);
}

/*
 * Make a secret where every byte differs.
 */
static void
test_secret(u_int8_t *secret, u_int8_t seed)
{
	size_t		idx;

	for (idx = 0; idx < VIGIL_LINK_SECRET_LEN; idx++)
		secret[idx] = seed ^ (idx * 7);
}

/*
 * Fill data with our marker so we can look for it on the socketpair.
 */
static void
test_fill(u_int8_t *data, size_t len)
{
	size_t		off, chunk;

	for (off = 0; off < len; off += chunk) {
		chunk = len - off;
		if (chunk > sizeof(TEST_MARKER) - 1)
			chunk = sizeof(TEST_MARKER) - 1;
		memcpy(&data[off], TEST_MARKER, chunk);
	}
}

/*
 * Returns 1 if our marker is in the given data.
 */
static int
test_seen(const u_int8_t *data, size_t len)
{
	size_t		idx, mlen;

	mlen = sizeof(TEST_MARKER) - 1;

	for (idx = 0; idx + mlen <= len; idx++) {
		if (!memcmp(&data[idx], TEST_MARKER, mlen))
			return (1);
	}

	return (0);
}

/*
 * A 64-bit FNV-1a over the given data.
 */
static u_int64_t
test_sum(const u_int8_t *data, size_t len)
{
	size_t		idx;
	u_int64_t	sum;

	sum = 0xcbf29ce484222325ULL;

	for (idx = 0; idx < len; idx++) {
		sum ^= data[idx];
		sum *= 0x100000001b3ULL;
	}

	return (sum);
}
