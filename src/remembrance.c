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
#include <sys/stat.h>
#include <sys/socket.h>

#include <libkyrka/libnyfe.h>

#include <arpa/inet.h>
#include <netinet/in.h>

#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "vigil.h"

/* The format for the remembrance path in the data directory. */
#define REMEMBRANCE_PATH_FMT	"%s/remembrance-%016" PRIx64

/*
 * Initialise the given remembrances for the given flock, loading them
 * from the data directory if we have been given one.
 */
void
vigil_remembrance_init(struct vigil_remembrance *rem, const char *datadir,
    u_int64_t flock)
{
	struct stat	st;
	ssize_t		ret;
	int		fd, len;

	PRECOND(rem != NULL);

	rem->path[0] = '\0';
	memset(&rem->list, 0, sizeof(rem->list));

	if (datadir == NULL)
		return;

	len = snprintf(rem->path, sizeof(rem->path), REMEMBRANCE_PATH_FMT,
	    datadir, flock);
	if (len < 0 || (size_t)len >= sizeof(rem->path)) {
		vigil_log(LOG_NOTICE, "remembrance path too long");
		rem->path[0] = '\0';
		return;
	}

	fd = open(rem->path, O_RDONLY);
	if (fd == -1) {
		if (errno != ENOENT) {
			vigil_log(LOG_NOTICE, "failed to open %s: %s",
			    rem->path, errno_s);
		}

		return;
	}

	if (fstat(fd, &st) == -1) {
		vigil_log(LOG_NOTICE, "stat(%s): %s", rem->path, errno_s);
		goto cleanup;
	}

	if ((size_t)st.st_size != sizeof(rem->list)) {
		vigil_log(LOG_NOTICE,
		    "remembrance has wrong size (%zu vs %zu)",
		    (size_t)st.st_size, sizeof(rem->list));
		goto cleanup;
	}

	for (;;) {
		ret = read(fd, &rem->list, sizeof(rem->list));
		if (ret == -1) {
			if (errno == EINTR)
				continue;
			vigil_log(LOG_NOTICE,
			    "failed to read remembrances: %s", errno_s);
			memset(&rem->list, 0, sizeof(rem->list));
			goto cleanup;
		}

		if ((size_t)ret != sizeof(rem->list)) {
			vigil_log(LOG_NOTICE,
			    "short read on remembrances %zd/%zu",
			    ret, sizeof(rem->list));
			memset(&rem->list, 0, sizeof(rem->list));
			goto cleanup;
		}

		break;
	}

	vigil_log(LOG_INFO, "loaded remembrances from %s", rem->path);

cleanup:
	(void)close(fd);
}

/*
 * Forget about the given remembrances we have in memory.
 */
void
vigil_remembrance_cleanup(struct vigil_remembrance *rem)
{
	PRECOND(rem != NULL);

	rem->path[0] = '\0';
	memset(&rem->list, 0, sizeof(rem->list));
}

/*
 * Save the received remembrance in the given remembrances and persist
 * it if we can.
 */
void
vigil_remembrance_save(struct vigil_remembrance *rem,
    struct kyrka_event_remembrance *evt)
{
	ssize_t		ret;
	int		fd, len;
	char		tmp[1024];

	PRECOND(rem != NULL);
	PRECOND(evt != NULL);

	if (!memcmp(&rem->list, evt, sizeof(*evt)))
		return;

	memcpy(&rem->list, evt, sizeof(*evt));
	vigil_log(LOG_INFO, "received new remembrances");

	if (rem->path[0] == '\0')
		return;

	len = snprintf(tmp, sizeof(tmp), "%s.tmp", rem->path);
	if (len < 0 || (size_t)len >= sizeof(tmp)) {
		vigil_log(LOG_NOTICE,
		    "remembrance path '%s' too big", rem->path);
		return;
	}

	if (unlink(tmp) == -1 && errno != ENOENT) {
		vigil_log(LOG_NOTICE, "failed to unlink %s: %s", tmp, errno_s);
		return;
	}

	fd = open(tmp, O_CREAT | O_EXCL | O_TRUNC | O_WRONLY, 0600);
	if (fd == -1) {
		vigil_log(LOG_NOTICE, "failed to open %s: %s", tmp, errno_s);
		return;
	}

	for (;;) {
		ret = write(fd, evt, sizeof(*evt));
		if (ret == -1) {
			if (errno == EINTR)
				continue;
			vigil_log(LOG_NOTICE,
			    "write error on %s: %s", tmp, errno_s);
			goto cleanup;
		}

		if ((size_t)ret != sizeof(*evt)) {
			vigil_log(LOG_NOTICE, "short write on %s (%zd/%zu)",
			    tmp, ret, sizeof(*evt));
			goto cleanup;
		}

		break;
	}

	if (close(fd) == -1) {
		fd = -1;
		vigil_log(LOG_NOTICE, "failed to close %s: %s", tmp, errno_s);
		goto cleanup;
	}

	fd = -1;

	if (rename(tmp, rem->path) == -1) {
		vigil_log(LOG_NOTICE, "failed to rename %s: %s", tmp, errno_s);
		goto cleanup;
	}

	return;

cleanup:
	if (fd != -1)
		(void)close(fd);

	if (unlink(tmp) == -1 && errno != ENOENT)
		vigil_log(LOG_NOTICE, "failed to remove %s: %s", tmp, errno_s);
}

/*
 * Returns the address of the remembrance at the given index in sin,
 * or -1 if there is none.
 */
int
vigil_remembrance_address(struct vigil_remembrance *rem, int idx,
    struct sockaddr_in *sin)
{
	PRECOND(rem != NULL);
	PRECOND(idx >= 0);
	PRECOND(sin != NULL);

	if (idx >= KYRKA_CATHEDRALS_MAX)
		return (-1);

	if (rem->list.ips[idx] == 0 || rem->list.ports[idx] == 0)
		return (-1);

	memset(sin, 0, sizeof(*sin));

	sin->sin_family = AF_INET;
	sin->sin_port = rem->list.ports[idx];
	sin->sin_addr.s_addr = rem->list.ips[idx];

	return (0);
}

/*
 * Randomly select a new cathedral from the given remembrances, if possible.
 */
int
vigil_remembrance_cathedral(struct vigil_remembrance *rem,
    struct vigil_cathedral *cat)
{
	u_int32_t		idx;
	int			count, attempts;

	PRECOND(rem != NULL);
	PRECOND(cat != NULL);

	count = 0;

	for (idx = 0; idx < KYRKA_CATHEDRALS_MAX; idx++) {
		if (rem->list.ips[idx] == 0 || rem->list.ports[idx] == 0)
			break;

		count++;
	}

	if (count == 0)
		return (-1);

	for (attempts = 0; attempts < KYRKA_CATHEDRALS_MAX; attempts++) {
		nyfe_random_bytes(&idx, sizeof(idx));
		idx = idx % count;

		if (cat->addr.sin_addr.s_addr != rem->list.ips[idx] ||
		    cat->addr.sin_port != rem->list.ports[idx])
			break;
	}

	if (attempts == KYRKA_CATHEDRALS_MAX)
		return (-1);

	cat->alive = 0;
	cat->last = vigil->now;
	cat->timeout = VIGIL_CATHEDRAL_TIMEOUT_INIT;

	cat->addr.sin_family = AF_INET;
	cat->addr.sin_port = rem->list.ports[idx];
	cat->addr.sin_addr.s_addr = rem->list.ips[idx];

	return (0);
}
