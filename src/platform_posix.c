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

#include <unistd.h>

#include "vigil.h"

/*
 * Nothing needs to happen when we close a socket.
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
