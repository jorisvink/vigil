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

#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include "vigil.h"

/* Set to 1 to silence logging, used by the tests. */
int		host_quiet = 0;

/*
 * Log a message to stdout.
 */
void
vigil_platform_log(int prio, const char *msg)
{
	struct tm		tm;
	struct timespec		ts;
	char			tbuf[32];

	PRECOND(msg != NULL);

	if (host_quiet)
		return;

	(void)clock_gettime(CLOCK_REALTIME, &ts);
	(void)gmtime_r(&ts.tv_sec, &tm);

	if (strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &tm) > 0)
		printf("%s.%03ld UTC ", tbuf, ts.tv_nsec / 1000000);

	printf("[vigil] %s\n", msg);
	fflush(stdout);
}

/*
 * Nothing needs to happen to our sockets on a regular host.
 */
int
vigil_platform_socket(int fd)
{
	PRECOND(fd >= 0);

	return (0);
}

/*
 * Nothing needs to happen when we close a socket on a regular host.
 */
void
vigil_platform_socket_close(int fd)
{
	PRECOND(fd >= 0);
}

/*
 * Read a single packet from the tunnel interface.
 */
ssize_t
vigil_platform_tun_read(int fd, u_int8_t *buf, size_t len)
{
	PRECOND(fd >= 0);
	PRECOND(buf != NULL);
	PRECOND(len > 0);

	return (read(fd, buf, len));
}

/*
 * Write a single packet to the tunnel interface.
 */
ssize_t
vigil_platform_tun_write(int fd, const u_int8_t *buf, size_t len)
{
	PRECOND(fd >= 0);
	PRECOND(buf != NULL);
	PRECOND(len > 0);

	return (write(fd, buf, len));
}

/*
 * Give up hard.
 */
void
vigil_platform_fatal(void)
{
	exit(1);
}
