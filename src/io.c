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

#include <poll.h>

#include "vigil.h"

/* The maximum number of descriptors we watch, peers + chats + tun + misc. */
#define IO_MAX		((KYRKA_PEERS_PER_FLOCK * 2) + 8)

/* The number of scheduled events. */
static int			count = 0;

/* The pollfds we hand to poll(). */
static struct pollfd		pfds[IO_MAX];

/* The scheduled i/o events, the index matches that of pfds. */
static struct vigil_io		*ios[IO_MAX];

/*
 * Initialise the i/o layer.
 */
void
vigil_io_init(void)
{
	count = 0;

	memset(ios, 0, sizeof(ios));
	memset(pfds, 0, sizeof(pfds));
}

/*
 * Cleanup the i/o layer, the owners of the descriptors close them.
 */
void
vigil_io_cleanup(void)
{
	count = 0;
}

/*
 * Wait for any i/o to occur on previously scheduled descriptors for
 * at most the given number of milliseconds.
 */
void
vigil_io_wait(int ms)
{
	struct vigil_io		*io;
	int			idx, nfd;

	PRECOND(ms >= 0);

	vigil_kyrka_leave();
	nfd = poll(pfds, count, ms);
	vigil_kyrka_enter();

	if (nfd == -1) {
		if (errno == EINTR)
			return;
		fatal("poll: %s", errno_s);
	}

	vigil->now = vigil_time_ms();

	if (nfd == 0)
		return;

	for (idx = 0; idx < count; idx++) {
		if (pfds[idx].revents == 0)
			continue;

		io = ios[idx];

		if (pfds[idx].revents & POLLNVAL)
			fatal("poll: fd %d is not open", pfds[idx].fd);

		if (pfds[idx].revents & (POLLIN | POLLERR | POLLHUP))
			io->flags |= VIGIL_IO_READABLE;

		pfds[idx].revents = 0;
		io->handle(io);

		if (idx < count && ios[idx] != io)
			idx--;
	}
}

/*
 * Schedule the given i/o event into our event loop.
 */
void
vigil_io_schedule(struct vigil_io *io)
{
	PRECOND(io != NULL);
	PRECOND(io->fd >= 0);
	PRECOND(io->handle != NULL);

	if (count >= IO_MAX)
		fatal("too many scheduled i/o events");

	io->flags = 0;

	ios[count] = io;

	pfds[count].fd = io->fd;
	pfds[count].revents = 0;
	pfds[count].events = POLLIN;

	count++;
}

/*
 * Remove the given i/o event from our event loop.
 */
void
vigil_io_unschedule(struct vigil_io *io)
{
	int		idx;

	PRECOND(io != NULL);

	for (idx = 0; idx < count; idx++) {
		if (ios[idx] == io)
			break;
	}

	if (idx == count)
		return;

	count--;

	if (idx != count) {
		ios[idx] = ios[count];
		pfds[idx] = pfds[count];
	}

	ios[count] = NULL;
	memset(&pfds[count], 0, sizeof(pfds[count]));
}
