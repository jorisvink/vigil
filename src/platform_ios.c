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
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include <arpa/inet.h>
#include <net/if.h>

#include <os/log.h>

#include <stdio.h>
#include <unistd.h>

#include "vigil.h"
#include "vigil_ios.h"

/*
 * These are not part of the public iOS SDK headers, the values are
 * stable and taken from xnu (sys/sys_domain.h and net/if_utun.h).
 */
#if !defined(SYSPROTO_CONTROL)
#define SYSPROTO_CONTROL	2
#endif

#if !defined(UTUN_OPT_IFNAME)
#define UTUN_OPT_IFNAME		2
#endif

/* The highest descriptor we look at when searching for the utun fd. */
#define IOS_UTUN_FD_MAX		1024

/* The length of the address family header on utun packets. */
#define IOS_UTUN_HDR_LEN	4

/* The number of descriptors we want, every socket comes with an anchor. */
#define IOS_NOFILE_MAX		4096

/* Where anchors connect to, the discard port on TEST-NET-1. */
#define IOS_ANCHOR_ADDR		0xc0000201
#define IOS_ANCHOR_PORT		9

/*
 * When we sleep the kernel only hands our interfaces the ports that have
 * a connected socket on them, so each of our sockets gets an anchor on
 * its port that is connected to nowhere.
 *
 * Indexed by the socket its descriptor, only its owner touches a slot.
 */
struct ios_anchor {
	int		anchor;
	int		active;
};

static void	ios_nofile_raise(void);

static struct ios_anchor	anchors[IOS_NOFILE_MAX];

/*
 * Log a message to the unified logging system.
 */
void
vigil_platform_log(int prio, const char *msg)
{
	os_log_type_t	type;

	PRECOND(msg != NULL);

	switch (prio) {
	case LOG_ERR:
		type = OS_LOG_TYPE_ERROR;
		break;
	case LOG_NOTICE:
		type = OS_LOG_TYPE_DEFAULT;
		break;
	default:
		type = OS_LOG_TYPE_INFO;
		break;
	}

	os_log_with_type(OS_LOG_DEFAULT, type, "vigil: %{public}s", msg);
}

/*
 * Anchor the given socket so we are woken up for traffic on its port.
 */
int
vigil_platform_socket(int fd)
{
	struct sockaddr_in	sin;
	socklen_t		socklen;
	int			on, anchor;

	PRECOND(fd >= 0);

	ios_nofile_raise();

	if (fd >= IOS_NOFILE_MAX) {
		vigil_log(LOG_NOTICE, "socket %d too high to anchor", fd);
		return (0);
	}

	on = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on)) == -1) {
		vigil_log(LOG_NOTICE, "SO_REUSEPORT: %s", errno_s);
		return (0);
	}

	socklen = sizeof(sin);
	if (getsockname(fd, (struct sockaddr *)&sin, &socklen) == -1) {
		vigil_log(LOG_NOTICE, "getsockname: %s", errno_s);
		return (0);
	}

	anchor = socket(AF_INET, SOCK_DGRAM, 0);
	if (anchor == -1) {
		vigil_log(LOG_NOTICE, "anchor socket: %s", errno_s);
		return (0);
	}

	if (setsockopt(anchor, SOL_SOCKET,
	    SO_REUSEPORT, &on, sizeof(on)) == -1) {
		vigil_log(LOG_NOTICE, "anchor SO_REUSEPORT: %s", errno_s);
		(void)close(anchor);
		return (0);
	}

	if (bind(anchor, (const struct sockaddr *)&sin, sizeof(sin)) == -1) {
		vigil_log(LOG_NOTICE, "anchor bind: %s", errno_s);
		(void)close(anchor);
		return (0);
	}

	sin.sin_addr.s_addr = htonl(IOS_ANCHOR_ADDR);
	sin.sin_port = htons(IOS_ANCHOR_PORT);

	if (connect(anchor, (const struct sockaddr *)&sin, sizeof(sin)) == -1) {
		vigil_log(LOG_NOTICE, "anchor connect: %s", errno_s);
		(void)close(anchor);
		return (0);
	}

	anchors[fd].anchor = anchor;
	anchors[fd].active = 1;

	return (0);
}

/*
 * Close the anchor that belongs to the given socket, if it has one.
 */
void
vigil_platform_socket_close(int fd)
{
	PRECOND(fd >= 0);

	if (fd >= IOS_NOFILE_MAX)
		return;

	if (anchors[fd].active == 0)
		return;

	(void)close(anchors[fd].anchor);

	anchors[fd].anchor = -1;
	anchors[fd].active = 0;
}

/*
 * Read a single packet from the utun interface, stripping the address
 * family header from it.
 */
ssize_t
vigil_platform_tun_read(int fd, u_int8_t *buf, size_t len)
{
	ssize_t		ret;
	struct iovec	iov[2];
	u_int32_t	family;

	PRECOND(fd >= 0);
	PRECOND(buf != NULL);
	PRECOND(len > 0);

	iov[0].iov_base = &family;
	iov[0].iov_len = sizeof(family);

	iov[1].iov_len = len;
	iov[1].iov_base = buf;

	ret = readv(fd, iov, 2);
	if (ret == -1)
		return (-1);

	if (ret == 0)
		return (0);

	if (ret <= IOS_UTUN_HDR_LEN) {
		errno = EAGAIN;
		return (-1);
	}

	return (ret - IOS_UTUN_HDR_LEN);
}

/*
 * Write a single packet to the utun interface, prepending the address
 * family header for it.
 */
ssize_t
vigil_platform_tun_write(int fd, const u_int8_t *buf, size_t len)
{
	ssize_t		ret;
	struct iovec	iov[2];
	u_int32_t	family;

	PRECOND(fd >= 0);
	PRECOND(buf != NULL);
	PRECOND(len > 0);

	if (vigil_inet_version(buf, len) == 6)
		family = htonl(AF_INET6);
	else
		family = htonl(AF_INET);

	iov[0].iov_base = &family;
	iov[0].iov_len = sizeof(family);

	iov[1].iov_len = len;
	iov[1].iov_base = (void *)(uintptr_t)buf;

	ret = writev(fd, iov, 2);
	if (ret == -1)
		return (-1);

	if (ret < IOS_UTUN_HDR_LEN)
		return (0);

	return (ret - IOS_UTUN_HDR_LEN);
}

/*
 * Give up hard, the keys have already been erased at this point.
 */
void
vigil_platform_fatal(void)
{
	abort();
}

/*
 * Find the descriptor of the utun interface the system created for
 * our packet tunnel provider, returns -1 if it could not be found.
 */
int
vigil_ios_utun_fd(void)
{
	int		fd;
	socklen_t	len;
	char		name[IFNAMSIZ];

	for (fd = 0; fd < IOS_UTUN_FD_MAX; fd++) {
		len = sizeof(name);
		memset(name, 0, sizeof(name));

		if (getsockopt(fd, SYSPROTO_CONTROL,
		    UTUN_OPT_IFNAME, name, &len) == -1)
			continue;

		if (!strncmp(name, "utun", 4))
			return (fd);
	}

	return (-1);
}

/*
 * Raise the number of descriptors we may have open, the default of 256
 * does not fit a full flock with its anchors.
 */
static void
ios_nofile_raise(void)
{
	struct rlimit		rl;

	if (getrlimit(RLIMIT_NOFILE, &rl) == -1) {
		vigil_log(LOG_NOTICE, "getrlimit: %s", errno_s);
		return;
	}

	if (rl.rlim_cur >= IOS_NOFILE_MAX)
		return;

	if (rl.rlim_cur == rl.rlim_max)
		return;

	rl.rlim_cur = IOS_NOFILE_MAX;
	if (rl.rlim_max < rl.rlim_cur)
		rl.rlim_cur = rl.rlim_max;

	if (setrlimit(RLIMIT_NOFILE, &rl) == -1)
		vigil_log(LOG_NOTICE, "setrlimit: %s", errno_s);
}
