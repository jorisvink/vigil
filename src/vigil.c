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
#include <sys/socket.h>

#include <arpa/inet.h>

#include <fcntl.h>
#include <inttypes.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include "vigil.h"

#include <libkyrka/libnyfe.h>

/* The maximum number of milliseconds we block waiting for i/o. */
#define VIGIL_IO_WAIT_MAX	500

/* The milliseconds we block waiting for i/o in a call or moving data. */
#define VIGIL_IO_WAIT_BUSY	10

/* The milliseconds we wait before we retry resolving a hostname. */
#define VIGIL_RESOLVE_RETRY	3000

/* The milliseconds an emergency erase waits for the libkyrka owner. */
#define VIGIL_ERASE_WAIT	2000

static void	vigil_cleanup(void);
static int	vigil_sleep(u_int64_t);
static void	vigil_tun_read(void *);
static void	vigil_poke_handle(void);
static int	vigil_hosts_resolve(void);
static void	vigil_kyrka_erase(void);
static void	vigil_kyrka_fatal(const char *, va_list);

static void	vigil_wake_init(void);
static void	vigil_wake_read(void *);

static void			vigil_gospel_drop(void);
static void			vigil_gospel_handle(void);
static void			vigil_gospel_detach(void);
static struct vigil_config	*vigil_gospel_parse(const char *, size_t);

/*
 * A Gospel configuration handed to us by vigil_gospel() to attach to
 * the running core, an empty one detaches the one we have.
 */
struct vigil_gospel {
	size_t		len;
	char		cfg[VIGIL_CONFIG_MAX];
};

/* The global vigil state. */
struct vigil_state		*vigil = NULL;

/* The i/o events for the tunnel interface and the wakeup pipe. */
static struct vigil_io		tun_io;
static struct vigil_io		wake_io;

/* Bumped each time we should tell everyone we are here right away. */
static volatile u_int64_t	pokes = 0;

/* The poke we last handled for the running tunnel. */
static u_int64_t		poked = 0;

/* Set to 1 from the start of vigil_run() until all is cleaned up. */
static volatile int		active = 0;

/* The id of the active run and the highest id we were asked to stop. */
static volatile u_int64_t	current = 0;
static volatile u_int64_t	stopped = 0;

/* The last id handed out by vigil_run_id(). */
static volatile u_int64_t	last_id = 0;

/* Bumped each time our sockets should be reopened. */
static volatile u_int64_t	rebinds = 0;

/* The rebind we last did for the running tunnel and when we may again. */
static u_int64_t		rebound = 0;
static u_int64_t		next_rebind = 0;

/* Where the running core keeps its remembrances. */
static const char		*datadir_run = NULL;

/* The Gospel configuration the event loop should pick up, if any. */
static struct vigil_gospel	*volatile gospel = NULL;

/* Our wakeup pipe, created by the first run and never closed. */
static volatile int		wake[2] = { -1, -1 };

/* Held by the thread that uses libkyrka, see vigil_kyrka_enter(). */
static pthread_mutex_t		kyrka_lock = PTHREAD_MUTEX_INITIALIZER;

/* Set on the thread that holds kyrka_lock. */
static __thread int		kyrka_held = 0;

/* Set once an emergency erase wants kyrka_lock, it is never cleared. */
static volatile int		kyrka_dying = 0;

/*
 * Returns the vigil version string.
 */
const char *
vigil_version(void)
{
	static __thread char	version[128];

	(void)snprintf(version, sizeof(version), "%s (%s)",
	    vigil_build_rev, vigil_build_date);

	return (version);
}

/*
 * Run a tunnel using the given configuration on the given tunnel interface.
 *
 * Each run needs an id from vigil_run_id(), vigil_stop() stops the run
 * with that id and all runs before it, even if they did not start yet.
 *
 * If a Gospel configuration is given we call and chat with it next to
 * the tunnel, a broken Gospel configuration does not stop the tunnel.
 * A Gospel configuration can also run on its own, without a tunnel
 * interface (fd -1).
 *
 * This blocks until vigil_stop() is called or an unrecoverable error
 * occurred during setup. Returns 0 on a normal stop, -1 on error.
 */
int
vigil_run(u_int64_t id, int fd, const char *cfg, size_t cfglen,
    const char *gcfg, size_t gcfglen, const char *datadir)
{
	int		running;
	char		err[128];

	PRECOND(id > 0);
	PRECOND(fd >= 0 || fd == -1);
	PRECOND(cfg != NULL);

	if (!vigil_atomic_cas_simple(&active, 0, 1)) {
		vigil_log(LOG_NOTICE, "a tunnel is already running");
		return (-1);
	}

	vigil_atomic_write(&current, id);

	if (id <= vigil_atomic_read(&stopped)) {
		vigil_atomic_write(&active, 0);
		return (0);
	}

	vigil_status_reset();

	vigil = calloc(1, sizeof(*vigil));
	if (vigil == NULL)
		fatal("failed to allocate vigil state");

	datadir_run = datadir;
	vigil->fd = fd;

	vigil_gospel_drop();

	vigil->now = vigil_time_ms();
	vigil->started = vigil->now;

	vigil->config = vigil_config_parse(cfg, cfglen, err, sizeof(err));
	if (vigil->config == NULL) {
		vigil_log(LOG_ERR, "invalid configuration: %s", err);
		vigil_cleanup();
		return (-1);
	}

	if (vigil->config->mode == VIGIL_MODE_GOSPEL && fd != -1) {
		vigil_log(LOG_ERR,
		    "a gospel configuration runs without a tunnel");
		vigil_cleanup();
		return (-1);
	}

	if (vigil->config->mode == VIGIL_MODE_GOSPEL) {
		vigil->gospel = vigil->config;
	} else if (gcfg != NULL) {
		vigil->gospel = vigil_config_parse(gcfg, gcfglen,
		    err, sizeof(err));

		if (vigil->gospel == NULL) {
			vigil_log(LOG_ERR, "invalid gospel configuration: %s",
			    err);
		} else if (vigil->gospel->mode != VIGIL_MODE_GOSPEL) {
			vigil_log(LOG_ERR, "not a gospel configuration");
			vigil_config_free(vigil->gospel);
			vigil->gospel = NULL;
		}
	}

	if (vigil->config->mode == VIGIL_MODE_LITURGY &&
	    !(vigil->config->flags & VIGIL_CONFIG_FLAG_TIER6)) {
		vigil_inet_liturgy(&vigil->ip, &vigil->config->liturgy_prefix,
		    vigil->config->spi & 0xff, vigil->config->spi & 0xff);
	} else {
		vigil->ip = vigil->config->ip;
	}

	if (wake[0] == -1)
		vigil_wake_init();

	if (vigil->fd != -1)
		vigil_socket_nonblock(vigil->fd);

	if (id > vigil_atomic_read(&stopped))
		running = 1;
	else
		running = 0;

	vigil_kyrka_enter();
	kyrka_fatal_callback(vigil_kyrka_fatal);

	vigil_io_init();
	vigil_status_init();

	wake_io.fd = wake[0];
	wake_io.handle = vigil_wake_read;
	vigil_io_schedule(&wake_io);

	if (vigil->fd != -1) {
		tun_io.fd = vigil->fd;
		tun_io.handle = vigil_tun_read;
		vigil_io_schedule(&tun_io);
	}

	vigil->flags |= VIGIL_FLAG_RUNNING;

	next_rebind = 0;
	rebound = vigil_rebind_generation();
	poked = vigil_atomic_read(&pokes);

	vigil_log(LOG_INFO, "vigil %s, libkyrka %s",
	    vigil_version(), kyrka_version());
	vigil_log(LOG_INFO, "starting %s '%s'",
	    vigil_config_mode(vigil->config), vigil->config->instance);

	if (running && vigil_hosts_resolve() == -1)
		running = 0;

	if (running) {
		if (vigil->config->flags & VIGIL_CONFIG_FLAG_REMEMBRANCE)
			vigil_remembrance_init(&vigil->remembrance, datadir,
			    vigil->config->cathedral_flock);

		vigil_peer_init();

		switch (vigil->config->mode) {
		case VIGIL_MODE_TUNNEL:
			vigil_peer_state(1, 1);
			break;
		case VIGIL_MODE_CATHEDRAL:
			vigil_peer_state(vigil->config->spi & 0xff, 1);
			break;
		case VIGIL_MODE_LITURGY:
			vigil_liturgy_init();
			break;
		}

		if (vigil->gospel != NULL) {
			vigil_call_init(datadir);
			vigil_chat_init();
		}

		vigil_log(LOG_INFO, "up and running");
	}

	while (running && (vigil->flags & VIGIL_FLAG_RUNNING)) {
		vigil_status_update(0);

		if (vigil_voice_busy() || vigil_chat_busy())
			vigil_io_wait(VIGIL_IO_WAIT_BUSY);
		else
			vigil_io_wait(VIGIL_IO_WAIT_MAX);

		vigil->now = vigil_time_ms();

		if (vigil_rebind_due(&rebound, &next_rebind, vigil->now)) {
			vigil_log(LOG_NOTICE, "network changed, new sockets");
			vigil_peer_rebind();

			if (vigil->config->mode == VIGIL_MODE_LITURGY)
				vigil_liturgy_rebind();

			if (vigil->gospel != NULL) {
				vigil_call_rebind();
				vigil_chat_rebind();
				vigil_voice_rebind();
			}
		}

		vigil_poke_handle();
		vigil_gospel_handle();
		vigil_peer_update();

		if (vigil->config->mode == VIGIL_MODE_LITURGY)
			vigil_liturgy_update();

		if (vigil->gospel != NULL) {
			vigil_call_update();
			vigil_chat_update();
			vigil_voice_update();
		}

		vigil_peer_prune();
	}

	vigil_log(LOG_INFO, "shutting down");
	vigil_cleanup();
	vigil_kyrka_leave();

	return (0);
}

/*
 * Returns a new id for vigil_run(), higher than all those before it.
 * May be called from any thread.
 */
u_int64_t
vigil_run_id(void)
{
	return (vigil_atomic_add(&last_id, 1) + 1);
}

/*
 * Request the run with the given id, and those before it, to stop.
 * May be called from any thread.
 */
void
vigil_stop(u_int64_t id)
{
	int		fd;
	u_int8_t	byte;
	u_int64_t	last;

stop_again:
	last = vigil_atomic_read(&stopped);

	if (id > last && !vigil_atomic_cas(&stopped, &last, &id))
		goto stop_again;

	fd = vigil_atomic_read(&wake[1]);

	if (fd != -1 && vigil_atomic_read(&active) &&
	    vigil_atomic_read(&current) <= vigil_atomic_read(&stopped)) {
		byte = 1;

		if (write(fd, &byte, sizeof(byte)) == -1 &&
		    errno != EAGAIN && errno != EWOULDBLOCK)
			vigil_log(LOG_NOTICE, "wakeup write: %s", errno_s);
	}
}

/*
 * Log a message to the platform log and our status log.
 */
void
vigil_log(int prio, const char *fmt, ...)
{
	va_list		args;

	PRECOND(prio >= 0);
	PRECOND(fmt != NULL);

	va_start(args, fmt);
	vigil_logv(prio, fmt, args);
	va_end(args);
}

/*
 * Log a message to the platform log and our status log, the variadic variant.
 */
void
vigil_logv(int prio, const char *fmt, va_list args)
{
	int		len;
	char		buf[VIGIL_LOG_LINE_MAX];

	PRECOND(prio >= 0);
	PRECOND(fmt != NULL);

	len = vsnprintf(buf, sizeof(buf), fmt, args);
	if (len < 0)
		return;

	vigil_platform_log(prio, buf);
	vigil_status_log(buf);
}

/*
 * Helper function to mark a given fd as non-blocking.
 */
void
vigil_socket_nonblock(int fd)
{
	int		flags;

	PRECOND(fd >= 0);

	flags = fcntl(fd, F_GETFL, 0);
	if (flags == -1)
		fatal("fcntl: %s", errno_s);

	flags |= O_NONBLOCK;

	if (fcntl(fd, F_SETFL, flags) == -1)
		fatal("fcntl: %s", errno_s);
}

/*
 * Create a new non-blocking udp socket bound to a port of its own
 * that bypasses our tunnel, returns -1 if we could not.
 */
int
vigil_socket_udp(void)
{
	int			fd;
	struct sockaddr_in	sin;

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd == -1) {
		vigil_log(LOG_NOTICE, "socket: %s", errno_s);
		return (-1);
	}

	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;

	if (bind(fd, (const struct sockaddr *)&sin, sizeof(sin)) == -1) {
		vigil_log(LOG_NOTICE, "bind: %s", errno_s);
		(void)close(fd);
		return (-1);
	}

	if (vigil_platform_socket(fd) == -1) {
		(void)close(fd);
		return (-1);
	}

	vigil_socket_nonblock(fd);

	return (fd);
}

/*
 * Close a socket we got from vigil_socket_udp().
 */
void
vigil_socket_close(int fd)
{
	PRECOND(fd >= 0);

	vigil_platform_socket_close(fd);
	(void)close(fd);
}

/*
 * Check if a send failed because the destination is unreachable, returns
 * 1 if so and the caller stays quiet. If rebind is 1 the destination was
 * a cathedral, the network we were on went away so we ask for new sockets.
 */
int
vigil_socket_unreachable(int err, int rebind)
{
	PRECOND(rebind == 0 || rebind == 1);

	switch (err) {
	case ENETDOWN:
	case ENETUNREACH:
	case EHOSTUNREACH:
	case EADDRNOTAVAIL:
		if (rebind)
			vigil_rebind();
		return (1);
	}

	return (0);
}

/*
 * Ask for all sockets to be reopened, for example because the network
 * changed. May be called from any thread.
 */
void
vigil_rebind(void)
{
	vigil_atomic_add(&rebinds, 1);
}

/*
 * Ask the running tunnel to send heartbeats and tell the cathedrals about
 * us right away, for example because we just woke up. May be called from
 * any thread.
 */
void
vigil_poke(void)
{
	vigil_atomic_add(&pokes, 1);
	vigil_wake_signal();
}

/*
 * Attach the given Gospel configuration to the running core, or detach
 * the one it has if len is 0, without touching the tunnel. May be called
 * from any thread, returns -1 if no core runs or the configuration is
 * too large.
 */
int
vigil_gospel(const char *cfg, size_t len)
{
	struct vigil_gospel	*req, *old;

	PRECOND(cfg != NULL || len == 0);

	if (len > VIGIL_CONFIG_MAX)
		return (-1);

	if (!vigil_atomic_read(&active))
		return (-1);

	req = calloc(1, sizeof(*req));
	if (req == NULL)
		fatal("calloc: failed to allocate gospel request");

	req->len = len;
	if (len > 0)
		memcpy(req->cfg, cfg, len);

	old = vigil_atomic_read(&gospel);

gospel_again:
	if (!vigil_atomic_cas(&gospel, &old, &req))
		goto gospel_again;

	if (old != NULL) {
		nyfe_mem_zero(old, sizeof(*old));
		free(old);
	}

	vigil_wake_signal();

	return (0);
}

/*
 * Returns the current rebind generation, may be called from any thread.
 */
u_int64_t
vigil_rebind_generation(void)
{
	return (vigil_atomic_read(&rebinds));
}

/*
 * Returns 1 if a rebind was asked for since the generation in seen and
 * we are allowed to do one at now, updating seen and next if so.
 */
int
vigil_rebind_due(u_int64_t *seen, u_int64_t *next, u_int64_t now)
{
	u_int64_t	gen;

	PRECOND(seen != NULL);
	PRECOND(next != NULL);

	gen = vigil_atomic_read(&rebinds);

	if (gen == *seen)
		return (0);

	if (now < *next)
		return (0);

	*seen = gen;
	*next = now + VIGIL_REBIND_INTERVAL;

	return (1);
}

/*
 * Returns the current monotonic time in milliseconds.
 *
 * On Linux (and thus Android) CLOCK_MONOTONIC stops while suspended,
 * we must see the time a phone was asleep so use CLOCK_BOOTTIME there.
 */
u_int64_t
vigil_time_ms(void)
{
	struct timespec		ts;

#if defined(__linux__)
	(void)clock_gettime(CLOCK_BOOTTIME, &ts);
#else
	(void)clock_gettime(CLOCK_MONOTONIC, &ts);
#endif

	return ((u_int64_t)ts.tv_sec * 1000 + (ts.tv_nsec / 1000000));
}

/*
 * Check if the two given inet addresses and ports match.
 */
int
vigil_inet_match(struct sockaddr_in *s1, struct sockaddr_in *s2)
{
	PRECOND(s1 != NULL);
	PRECOND(s2 != NULL);

	if (s1->sin_addr.s_addr == s2->sin_addr.s_addr &&
	    s1->sin_port == s2->sin_port)
		return (1);

	return (0);
}

/*
 * Resolve the given host and port into an ipv4 address.
 * Returns 0 on success, -1 if we could not resolve it.
 */
int
vigil_resolve(const char *host, u_int16_t port, struct sockaddr_in *sin)
{
	int			ret;
	char			str[8];
	struct addrinfo		hints, *res, *rp;

	PRECOND(host != NULL);
	PRECOND(port != 0);
	PRECOND(sin != NULL);

	(void)snprintf(str, sizeof(str), "%u", port);

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;

	vigil_kyrka_leave();
	ret = getaddrinfo(host, str, &hints, &res);
	vigil_kyrka_enter();

	if (ret != 0) {
		vigil_log(LOG_NOTICE, "failed to resolve %s: %s",
		    host, gai_strerror(ret));
		return (-1);
	}

	for (rp = res; rp != NULL; rp = rp->ai_next) {
		if (rp->ai_family == AF_INET &&
		    rp->ai_socktype == SOCK_DGRAM)
			break;
	}

	if (rp == NULL) {
		vigil_log(LOG_NOTICE, "%s has no ipv4 address", host);
		freeaddrinfo(res);
		return (-1);
	}

	VERIFY(rp->ai_addrlen == sizeof(*sin));
	memcpy(sin, rp->ai_addr, rp->ai_addrlen);
	freeaddrinfo(res);

	return (0);
}

/*
 * Helper function that takes a sockaddr_in and returns a pointer
 * to a human readable ipv4 address:port string.
 */
const char *
vigil_address(struct sockaddr_in *sin)
{
	int			len;
	static __thread char	buf[64];
	char			ip[INET_ADDRSTRLEN];

	PRECOND(sin != NULL);

	if (inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip)) == NULL)
		fatal("inet_ntop: %s", errno_s);

	len = snprintf(buf, sizeof(buf), "%s:%u", ip, be16toh(sin->sin_port));
	if (len < 0 || (size_t)len >= sizeof(buf))
		fatal("failed to create ip:port address string");

	return (buf);
}

/* Bad juju happened. */
void
fatal(const char *fmt, ...)
{
	va_list		args;

	vigil_kyrka_erase();

	va_start(args, fmt);
	vigil_logv(LOG_ERR, fmt, args);
	va_end(args);

	vigil_platform_fatal();
}

/*
 * Take ownership of libkyrka, only the thread holding kyrka_lock may
 * touch a KYRKA context. An emergency erase from any other thread first
 * waits for us to let go, so we are never in the middle of using one.
 */
void
vigil_kyrka_enter(void)
{
	PRECOND(kyrka_held == 0);

	while (vigil_atomic_read(&kyrka_dying))
		(void)poll(NULL, 0, 1000);

	if (pthread_mutex_lock(&kyrka_lock) != 0)
		fatal("failed to take the kyrka lock");

	kyrka_held = 1;
}

/*
 * Let go of libkyrka, called before we block on anything.
 */
void
vigil_kyrka_leave(void)
{
	PRECOND(kyrka_held == 1);

	kyrka_held = 0;

	if (pthread_mutex_unlock(&kyrka_lock) != 0)
		fatal("failed to release the kyrka lock");
}

/*
 * Resolve the hosts in our configuration, we keep trying until we either
 * manage to resolve them or are asked to stop.
 */
static int
vigil_hosts_resolve(void)
{
	u_int16_t		port;
	struct sockaddr_in	*sin;
	const char		*host;

	switch (vigil->config->mode) {
	case VIGIL_MODE_TUNNEL:
		host = vigil->config->peer_host;
		port = vigil->config->peer_port;
		break;
	case VIGIL_MODE_CATHEDRAL:
	case VIGIL_MODE_LITURGY:
		host = vigil->config->cathedral_host;
		port = vigil->config->cathedral_port;
		break;
	case VIGIL_MODE_GOSPEL:
		return (0);
	default:
		fatal("unknown mode %d", vigil->config->mode);
	}

	if (vigil->config->mode == VIGIL_MODE_TUNNEL)
		sin = &vigil->peer;
	else
		sin = &vigil->cathedral.addr;

	while (vigil_resolve(host, port, sin) == -1) {
		if (vigil_sleep(VIGIL_RESOLVE_RETRY) == -1)
			return (-1);
	}

	vigil->cathedral.last = 0;
	vigil->cathedral.timeout = 0;

	vigil_log(LOG_INFO, "%s resolved to %s", host, vigil_address(sin));

	return (0);
}

/*
 * Sleep for the given number of milliseconds while still processing
 * i/o, returns -1 if we were asked to stop while sleeping.
 */
static int
vigil_sleep(u_int64_t ms)
{
	u_int64_t	end;

	end = vigil_time_ms() + ms;

	while (vigil->flags & VIGIL_FLAG_RUNNING) {
		vigil_status_update(0);
		vigil_io_wait(VIGIL_IO_WAIT_MAX);

		vigil->now = vigil_time_ms();
		if (vigil->now >= end)
			return (0);
	}

	return (-1);
}

/*
 * Release all resources tied to the current vigil state.
 */
static void
vigil_cleanup(void)
{
	PRECOND(vigil != NULL);

	if (vigil->config != NULL &&
	    vigil->config->mode == VIGIL_MODE_LITURGY)
		vigil_liturgy_cleanup();

	vigil_gospel_detach();
	vigil_peer_cleanup();
	vigil_remembrance_cleanup(&vigil->remembrance);
	vigil_io_cleanup();

	vigil->flags &= ~VIGIL_FLAG_RUNNING;
	vigil_status_update(1);

	vigil_config_free(vigil->config);

	free(vigil);
	vigil = NULL;
	datadir_run = NULL;

	vigil_gospel_drop();

	vigil_atomic_write(&active, 0);
}

/*
 * Callback for when there are packets to be read from our tunnel interface.
 * We read them and hand them off to the peer layer for encryption.
 */
static void
vigil_tun_read(void *udata)
{
	int		idx;
	ssize_t		ret;
	u_int8_t	pkt[KYRKA_PACKET_DATA_LEN];

	PRECOND(udata == &tun_io);

	if (!(tun_io.flags & VIGIL_IO_READABLE))
		return;

	for (idx = 0; idx < VIGIL_IO_BATCH; idx++) {
		ret = vigil_platform_tun_read(vigil->fd, pkt, sizeof(pkt));
		if (ret == -1) {
			if (errno == EINTR)
				continue;
			if (errno == EWOULDBLOCK || errno == EAGAIN) {
				tun_io.flags &= ~VIGIL_IO_READABLE;
				break;
			}
			vigil_log(LOG_ERR, "tunnel read: %s", errno_s);
			vigil->flags &= ~VIGIL_FLAG_RUNNING;
			break;
		}

		if (ret == 0) {
			vigil_log(LOG_NOTICE, "tunnel interface went away");
			vigil->flags &= ~VIGIL_FLAG_RUNNING;
			break;
		}

		vigil_peer_output(pkt, ret);
	}
}

/*
 * Create our wakeup pipe.
 */
static void
vigil_wake_init(void)
{
	int		fds[2];

	PRECOND(wake[0] == -1);

	if (pipe(fds) == -1)
		fatal("pipe: %s", errno_s);

	vigil_socket_nonblock(fds[0]);
	vigil_socket_nonblock(fds[1]);

	vigil_atomic_write(&wake[0], fds[0]);
	vigil_atomic_write(&wake[1], fds[1]);
}

/*
 * If we were poked, have everyone send heartbeats and tell their
 * cathedral about us right away.
 */
static void
vigil_poke_handle(void)
{
	u_int64_t	gen;

	gen = vigil_atomic_read(&pokes);
	if (gen == poked)
		return;

	poked = gen;
	vigil_log(LOG_INFO, "poked, telling everyone we are here");

	vigil_peer_poke();

	if (vigil->config->mode == VIGIL_MODE_LITURGY)
		vigil_liturgy_poke();

	if (vigil->gospel != NULL) {
		vigil_call_poke();
		vigil_chat_poke();
	}
}

/*
 * Wake up the event loop of the running core, if there is one, so it
 * picks up whatever we handed it right away. May be called from any thread.
 */
void
vigil_wake_signal(void)
{
	int		fd;
	u_int8_t	byte;

	fd = vigil_atomic_read(&wake[1]);

	if (fd != -1 && vigil_atomic_read(&active)) {
		byte = 1;

		if (write(fd, &byte, sizeof(byte)) == -1 &&
		    errno != EAGAIN && errno != EWOULDBLOCK)
			vigil_log(LOG_NOTICE, "wakeup write: %s", errno_s);
	}
}

/*
 * If vigil_gospel() handed us a configuration, detach the Gospel parts
 * we run and attach the new ones. An invalid configuration changes
 * nothing, a Gospel configuration that runs on its own cannot be swapped.
 */
static void
vigil_gospel_handle(void)
{
	struct vigil_config	*cfg;
	struct vigil_gospel	*req, *none;

	none = NULL;

	req = vigil_atomic_read(&gospel);
	if (req == NULL)
		return;

	if (!vigil_atomic_cas(&gospel, &req, &none))
		return;

	if (vigil->config->mode == VIGIL_MODE_GOSPEL) {
		vigil_log(LOG_NOTICE, "[gospel] runs on its own, not swapped");
	} else if (req->len == 0) {
		vigil_gospel_detach();
		vigil_log(LOG_INFO, "[gospel] detached");
	} else {
		cfg = vigil_gospel_parse(req->cfg, req->len);

		if (cfg != NULL) {
			vigil_gospel_detach();

			vigil->gospel = cfg;
			vigil_call_init(datadir_run);
			vigil_chat_init();

			vigil_log(LOG_INFO, "[gospel] attached '%s'",
			    cfg->instance);
		}
	}

	nyfe_mem_zero(req, sizeof(*req));
	free(req);
}

/*
 * Parse the given Gospel configuration, returns NULL if it is invalid
 * or not a Gospel configuration.
 */
static struct vigil_config *
vigil_gospel_parse(const char *data, size_t len)
{
	struct vigil_config	*cfg;
	char			err[128];

	PRECOND(data != NULL);
	PRECOND(len > 0);

	cfg = vigil_config_parse(data, len, err, sizeof(err));
	if (cfg == NULL) {
		vigil_log(LOG_ERR, "invalid gospel configuration: %s", err);
		return (NULL);
	}

	if (cfg->mode != VIGIL_MODE_GOSPEL) {
		vigil_log(LOG_ERR, "not a gospel configuration");
		vigil_config_free(cfg);
		return (NULL);
	}

	return (cfg);
}

/*
 * Drop a request from vigil_gospel() we did not get to, it was meant
 * for a run that is gone.
 */
static void
vigil_gospel_drop(void)
{
	struct vigil_gospel	*req, *none;

	none = NULL;
	req = vigil_atomic_read(&gospel);

drop_again:
	if (req == NULL)
		return;

	if (!vigil_atomic_cas(&gospel, &req, &none))
		goto drop_again;

	nyfe_mem_zero(req, sizeof(*req));
	free(req);
}

/*
 * Stop the Gospel parts of the core and drop its configuration.
 */
static void
vigil_gospel_detach(void)
{
	if (vigil->gospel == NULL)
		return;

	vigil_voice_cleanup();
	vigil_chat_cleanup();
	vigil_call_cleanup();

	if (vigil->gospel != vigil->config)
		vigil_config_free(vigil->gospel);

	vigil->gospel = NULL;
}

/*
 * Callback for when someone wrote to our wakeup pipe.
 */
static void
vigil_wake_read(void *udata)
{
	ssize_t		ret;
	u_int8_t	buf[32];

	PRECOND(udata == &wake_io);

	for (;;) {
		ret = read(wake[0], buf, sizeof(buf));
		if (ret == -1) {
			if (errno == EINTR)
				continue;
			if (errno == EWOULDBLOCK || errno == EAGAIN) {
				wake_io.flags &= ~VIGIL_IO_READABLE;
				break;
			}
			fatal("wakeup read: %s", errno_s);
		}

		if (ret == 0)
			break;
	}

	if (vigil_atomic_read(&current) <= vigil_atomic_read(&stopped))
		vigil->flags &= ~VIGIL_FLAG_RUNNING;
}

/*
 * Erase all libkyrka contexts. If another thread owns them we wait until
 * it lets go and never give the lock back, so it cannot touch them after
 * they were wiped. The owner parks in vigil_kyrka_enter() once it sees
 * kyrka_dying. We do not wait forever, the owner may be blocked on
 * something the caller holds and in that case it never gets to run again.
 */
static void
vigil_kyrka_erase(void)
{
	int		ms;

	if (kyrka_held == 0) {
		vigil_atomic_write(&kyrka_dying, 1);

		for (ms = 0; ms < VIGIL_ERASE_WAIT; ms += 10) {
			if (pthread_mutex_trylock(&kyrka_lock) == 0)
				break;
			(void)poll(NULL, 0, 10);
		}

		kyrka_held = 1;
	}

	kyrka_emergency_erase();
}

/*
 * Bad juju happened.
 */
static void
vigil_kyrka_fatal(const char *fmt, va_list args)
{
	vigil_kyrka_erase();

	vigil_logv(LOG_ERR, fmt, args);
	vigil_platform_fatal();
}
