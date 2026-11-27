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

/*
 * The additional kdf input for a peer needs a 256-bit
 * kyrka_additional_kdf_input(), see dist-build/patches.
 */
#if KYRKA_ADDITIONAL_INPUT_LENGTH != VIGIL_ADDITIONAL_INPUT_LEN
#error "libkyrka without 256-bit additional input, see dist-build/patches"
#endif

/* The maximum number of milliseconds we block waiting for i/o. */
#define VIGIL_IO_WAIT_MAX	500

/* The milliseconds we block waiting for i/o in a call or moving data. */
#define VIGIL_IO_WAIT_BUSY	10

/* The milliseconds we wait before we retry resolving a hostname. */
#define VIGIL_RESOLVE_RETRY	3000

static void	vigil_cleanup(void);
static int	vigil_sleep(u_int64_t);
static void	vigil_tun_read(void *);
static void	vigil_poke_handle(void);
static int	vigil_hosts_resolve(void);
static void	vigil_kyrka_erase(void);
static void	vigil_kyrka_fatal(const char *, va_list);

static void	vigil_wake_init(void);
static void	vigil_wake_read(void *);

static void	vigil_gospel_drop(void);
static void	vigil_gospel_handle(void);
static void	vigil_gospel_detach(u_int8_t);
static void	vigil_gospel_detach_all(void);
static void	vigil_gospel_set(const char *, size_t);
static void	vigil_gospel_attach(struct vigil_config *, const u_int8_t *);
static void	vigil_gospel_digest(const char *, size_t, u_int8_t *, size_t);

static struct vigil_config	*vigil_gospel_parse(const char *, size_t);

/* The length of the digest we keep of each Gospel configuration. */
#define VIGIL_DIGEST_LEN	32

/*
 * The set of Gospel configurations handed to us by vigil_gospel() for the
 * running core, each one ends in a NUL. An empty set detaches them all.
 */
struct vigil_gospel {
	size_t		len;
	char		set[VIGIL_GOSPEL_SET_MAX];
};

/*
 * The additional kdf input for a peer, only used once it is set.
 */
struct vigil_input {
	int		set;
	u_int8_t	ikm[VIGIL_ADDITIONAL_INPUT_LEN];
};

/*
 * The additional kdf inputs for all peers in a flock, a flock of 0 means
 * the entry is free.
 */
struct vigil_inputs {
	u_int64_t		flock;
	struct vigil_input	peers[KYRKA_PEERS_PER_FLOCK];
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

/* The Gospel configurations the event loop should pick up, if any. */
static struct vigil_gospel	*gospel = NULL;

/* The digest of the configuration in each Gospel slot. */
static u_int8_t			digests[VIGIL_GOSPEL_MAX][VIGIL_DIGEST_LEN];

/* The flock in each Gospel slot, 0 if free, see vigil_gospel_slot(). */
static u_int64_t		flocks[VIGIL_GOSPEL_MAX];
static pthread_mutex_t		flock_lock = PTHREAD_MUTEX_INITIALIZER;

/* Our wakeup pipe, created by the first run and never closed. */
static volatile int		wake[2] = { -1, -1 };

/* Held by the thread that uses libkyrka, see vigil_kyrka_enter(). */
static pthread_mutex_t		kyrka_lock = PTHREAD_MUTEX_INITIALIZER;

/* Set on the thread that holds kyrka_lock. */
static __thread int		kyrka_held = 0;

/* The additional kdf input per peer, see vigil_additional_input(). */
static struct vigil_inputs	inputs[VIGIL_GOSPEL_MAX];
static pthread_mutex_t		input_lock = PTHREAD_MUTEX_INITIALIZER;

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
 * If a set of Gospel configurations is given we call and chat with them
 * next to the tunnel, each one ends in a NUL. A broken Gospel configuration
 * does not stop the tunnel. A Gospel configuration can also run on its own,
 * without a tunnel interface (fd -1), the set then holds the others.
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
	u_int8_t	digest[VIGIL_DIGEST_LEN];

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
	vigil->datadir = datadir;

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

	if (vigil->config->mode == VIGIL_MODE_COMPANION) {
		if (fd != -1) {
			vigil_log(LOG_ERR,
			    "a companion configuration runs without a tunnel");
			vigil_cleanup();
			return (-1);
		}

		if (vigil_chat_companion(cfg, cfglen) == -1) {
			vigil_cleanup();
			return (-1);
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

		if (vigil->config->mode == VIGIL_MODE_COMPANION)
			vigil_chat_init();

		if (vigil->config->mode == VIGIL_MODE_GOSPEL) {
			vigil_gospel_digest(cfg, cfglen,
			    digest, sizeof(digest));
			vigil_gospel_attach(vigil->config, digest);
		}

		if (gcfg != NULL)
			vigil_gospel_set(gcfg, gcfglen);

		vigil_log(LOG_INFO, "up and running");
	}

	while (running && (vigil->flags & VIGIL_FLAG_RUNNING)) {
		vigil_status_update(0);

		if (vigil_voice_busy() || vigil_chat_busy() ||
		    vigil_channel_busy())
			vigil_io_wait(VIGIL_IO_WAIT_BUSY);
		else
			vigil_io_wait(VIGIL_IO_WAIT_MAX);

		vigil->now = vigil_time_ms();

		if (vigil_rebind_due(&rebound, &next_rebind, vigil->now)) {
			vigil_log(LOG_NOTICE, "network changed, new sockets");
			vigil_peer_rebind();

			if (vigil->config->mode == VIGIL_MODE_LITURGY)
				vigil_liturgy_rebind();

			vigil_call_rebind();
			vigil_chat_rebind();
			vigil_voice_rebind();
			vigil_channel_rebind();
		}

		vigil_poke_handle();
		vigil_gospel_handle();
		vigil_peer_update();

		if (vigil->config->mode == VIGIL_MODE_LITURGY)
			vigil_liturgy_update();

		vigil_call_update();
		vigil_chat_update();
		vigil_voice_update();
		vigil_channel_update();

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
 * Configure a libkyrka context for the cathedrals in the given config,
 * loading our device kek too if kek is 1.
 */
void
vigil_kyrka_cathedral(KYRKA *ctx, struct kyrka_cathedral_cfg *cfg,
    const struct vigil_config *config, int kek, u_int16_t mtu)
{
	PRECOND(ctx != NULL);
	PRECOND(cfg != NULL);
	PRECOND(config != NULL);
	PRECOND(kek == 0 || kek == 1);

	if (config->flags & VIGIL_CONFIG_FLAG_SHROUD) {
		if (kyrka_shroud_enable(ctx) == -1) {
			fatal("failed to enable shroud: %d",
			    kyrka_last_error(ctx));
		}
	}

	if (kyrka_cathedral_config(ctx, cfg) == -1) {
		fatal("failed to configure cathedral: %d",
		    kyrka_last_error(ctx));
	}

	if (kek) {
		if (kyrka_device_kek_load(ctx, config->kek,
		    sizeof(config->kek)) == -1) {
			fatal("kyrka_device_kek_load: %d",
			    kyrka_last_error(ctx));
		}
	}

	if (kyrka_cathedral_secret_load(ctx, config->cathedral_secret,
	    sizeof(config->cathedral_secret)) == -1) {
		fatal("kyrka_cathedral_secret_load: %d",
		    kyrka_last_error(ctx));
	}

	if (kyrka_cathedral_cosk_load(ctx, config->cathedral_cosk,
	    sizeof(config->cathedral_cosk)) == -1) {
		fatal("kyrka_cathedral_cosk_load: %d",
		    kyrka_last_error(ctx));
	}

	if (kyrka_mtu_size(ctx, mtu) == -1) {
		fatal("failed to set libkyrka mtu size: %d",
		    kyrka_last_error(ctx));
	}
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
 * Hand the running core the set of Gospel configurations it should run,
 * each one ends in a NUL. The core keeps those it runs already, detaches
 * the ones that are not in the set and attaches the new ones, without
 * touching the tunnel. An empty set detaches them all. May be called
 * from any thread, returns -1 if no core runs or the set is too large.
 */
int
vigil_gospel(const char *set, size_t len)
{
	struct vigil_gospel	*req, *old;

	PRECOND(set != NULL || len == 0);

	if (len > VIGIL_GOSPEL_SET_MAX)
		return (-1);

	if (!vigil_atomic_read(&active))
		return (-1);

	req = calloc(1, sizeof(*req));
	if (req == NULL)
		fatal("calloc: failed to allocate gospel request");

	req->len = len;
	if (len > 0)
		memcpy(req->set, set, len);

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
 * Set the additional kdf input for the given peer, NULL drops it.
 * May be called from any thread, our chat tunnel with the peer is torn
 * down so the next one uses the new input.
 */
void
vigil_additional_input(u_int64_t flock, u_int8_t peer, const void *ikm,
    size_t len)
{
	struct vigil_inputs	*entry;
	int			idx, used;

	PRECOND(flock != 0);
	PRECOND(peer > 0 && peer < KYRKA_PEERS_PER_FLOCK);
	PRECOND(ikm != NULL || len == 0);
	PRECOND(ikm == NULL || len == VIGIL_ADDITIONAL_INPUT_LEN);

	entry = NULL;
	vigil_lock(&input_lock);

	for (idx = 0; idx < VIGIL_GOSPEL_MAX; idx++) {
		if (inputs[idx].flock == flock) {
			entry = &inputs[idx];
			break;
		}
	}

	if (entry == NULL && ikm != NULL) {
		for (idx = 0; idx < VIGIL_GOSPEL_MAX; idx++) {
			if (inputs[idx].flock == 0) {
				entry = &inputs[idx];
				entry->flock = flock;
				break;
			}
		}
	}

	if (entry == NULL) {
		vigil_unlock(&input_lock);
		if (ikm != NULL)
			vigil_log(LOG_NOTICE, "no room for the input of %02x",
			    peer);
		return;
	}

	if (ikm == NULL) {
		nyfe_mem_zero(&entry->peers[peer], sizeof(entry->peers[peer]));

		used = 0;
		for (idx = 0; idx < KYRKA_PEERS_PER_FLOCK; idx++)
			used |= entry->peers[idx].set;

		if (used == 0)
			entry->flock = 0;
	} else {
		entry->peers[peer].set = 1;
		memcpy(entry->peers[peer].ikm, ikm, len);
	}

	vigil_unlock(&input_lock);
	vigil_chat_additional_input(flock, peer);
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
 * touch a KYRKA context.
 */
void
vigil_kyrka_enter(void)
{
	PRECOND(kyrka_held == 0);

	vigil_lock(&kyrka_lock);

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

	vigil_unlock(&kyrka_lock);
}

/*
 * Take the given lock, we cannot continue if this fails.
 */
void
vigil_lock(pthread_mutex_t *lock)
{
	int		ret;

	PRECOND(lock != NULL);

	if ((ret = pthread_mutex_lock(lock)) != 0)
		fatal("pthread_mutex_lock: %s", strerror(ret));
}

/*
 * Release the given lock, we cannot continue if this fails.
 */
void
vigil_unlock(pthread_mutex_t *lock)
{
	int		ret;

	PRECOND(lock != NULL);

	if ((ret = pthread_mutex_unlock(lock)) != 0)
		fatal("pthread_mutex_unlock: %s", strerror(ret));
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
	case VIGIL_MODE_COMPANION:
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

	vigil_gospel_detach_all();

	if (vigil->config != NULL &&
	    vigil->config->mode == VIGIL_MODE_COMPANION)
		vigil_chat_cleanup();

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

	vigil_call_poke();
	vigil_chat_poke();
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
 * If vigil_gospel() handed us a set of configurations, run that set. A
 * companion runs without Gospel.
 */
static void
vigil_gospel_handle(void)
{
	struct vigil_gospel	*req, *none;

	none = NULL;

	req = vigil_atomic_read(&gospel);
	if (req == NULL)
		return;

	if (!vigil_atomic_cas(&gospel, &req, &none))
		return;

	if (vigil->config->mode == VIGIL_MODE_COMPANION)
		vigil_log(LOG_NOTICE, "[gospel] a companion runs without it");
	else
		vigil_gospel_set(req->set, req->len);

	nyfe_mem_zero(req, sizeof(*req));
	free(req);
}

/*
 * Run the given set of Gospel configurations. We detach the ones we run
 * that are not in the set or that changed, then attach the new ones. An
 * invalid configuration in the set is skipped, a Gospel configuration
 * that runs on its own always stays.
 */
static void
vigil_gospel_set(const char *set, size_t len)
{
	struct vigil_config	*cfg;
	const char		*end;
	size_t			off, cfglen;
	int			idx, count;
	u_int8_t		slot;
	u_int8_t		digest[VIGIL_DIGEST_LEN];
	struct vigil_config	*cfgs[VIGIL_GOSPEL_MAX];
	u_int8_t		sums[VIGIL_GOSPEL_MAX][VIGIL_DIGEST_LEN];

	PRECOND(set != NULL || len == 0);

	off = 0;
	count = 0;

	while (off < len) {
		end = memchr(set + off, '\0', len - off);
		if (end == NULL) {
			vigil_log(LOG_ERR, "[gospel] the set does not end");
			break;
		}

		cfglen = end - (set + off);
		if (cfglen == 0) {
			off++;
			continue;
		}

		cfg = vigil_gospel_parse(set + off, cfglen);
		if (cfg == NULL) {
			off += cfglen + 1;
			continue;
		}

		for (idx = 0; idx < count; idx++) {
			if (cfgs[idx]->cathedral_flock == cfg->cathedral_flock)
				break;
		}

		if (idx != count || count == VIGIL_GOSPEL_MAX) {
			vigil_log(LOG_ERR,
			    "[gospel] skipping '%s', flock %" PRIx64
			    " is in the set already or the set is full",
			    cfg->instance, cfg->cathedral_flock);
			vigil_config_free(cfg);
			off += cfglen + 1;
			continue;
		}

		vigil_gospel_digest(set + off, cfglen,
		    sums[count], sizeof(sums[count]));

		cfgs[count++] = cfg;
		off += cfglen + 1;
	}

	for (slot = 0; slot < VIGIL_GOSPEL_MAX; slot++) {
		if (vigil->gospels[slot] == NULL)
			continue;

		if (vigil->gospels[slot] == vigil->config)
			continue;

		for (idx = 0; idx < count; idx++) {
			if (cfgs[idx]->cathedral_flock !=
			    vigil->gospels[slot]->cathedral_flock)
				continue;

			if (!memcmp(sums[idx], digests[slot],
			    sizeof(sums[idx])))
				break;
		}

		if (idx == count)
			vigil_gospel_detach(slot);
	}

	for (idx = 0; idx < count; idx++) {
		if (vigil_gospel_find(cfgs[idx]->cathedral_flock, NULL) != -1) {
			vigil_config_free(cfgs[idx]);
			continue;
		}

		memcpy(digest, sums[idx], sizeof(digest));
		vigil_gospel_attach(cfgs[idx], digest);
	}

	nyfe_mem_zero(sums, sizeof(sums));
	nyfe_mem_zero(digest, sizeof(digest));
}

/*
 * Attach the given Gospel configuration in a free slot, the slot owns the
 * configuration from here on.
 */
static void
vigil_gospel_attach(struct vigil_config *cfg, const u_int8_t *digest)
{
	int		first;
	u_int8_t	slot;

	PRECOND(cfg != NULL);
	PRECOND(digest != NULL);
	PRECOND(cfg->mode == VIGIL_MODE_GOSPEL);

	first = 1;

	for (slot = 0; slot < VIGIL_GOSPEL_MAX; slot++) {
		if (vigil->gospels[slot] != NULL)
			first = 0;
	}

	for (slot = 0; slot < VIGIL_GOSPEL_MAX; slot++) {
		if (vigil->gospels[slot] == NULL)
			break;
	}

	VERIFY(slot < VIGIL_GOSPEL_MAX);

	vigil->gospels[slot] = cfg;
	memcpy(digests[slot], digest, sizeof(digests[slot]));

	if (first && vigil->config->mode != VIGIL_MODE_COMPANION)
		vigil_chat_init();

	vigil_call_attach(slot);
	vigil_chat_attach(slot);

	vigil_lock(&flock_lock);
	flocks[slot] = cfg->cathedral_flock;
	vigil_unlock(&flock_lock);

	vigil_log(LOG_INFO, "[gospel] attached '%s' for flock %" PRIx64,
	    cfg->instance, cfg->cathedral_flock);
}

/*
 * Stop the Gospel parts of the core for the given slot and drop its
 * configuration. Once the last one is gone our chats stop too.
 */
static void
vigil_gospel_detach(u_int8_t slot)
{
	int		idx, last;

	PRECOND(slot < VIGIL_GOSPEL_MAX);
	PRECOND(vigil->gospels[slot] != NULL);

	vigil_log(LOG_INFO, "[gospel] detaching flock %" PRIx64,
	    vigil->gospels[slot]->cathedral_flock);

	vigil_lock(&flock_lock);
	flocks[slot] = 0;
	vigil_unlock(&flock_lock);

	vigil_voice_detach(slot);
	vigil_channel_detach(slot);
	vigil_safety_detach(slot);
	vigil_chat_detach(slot);
	vigil_call_detach(slot);

	if (vigil->gospels[slot] != vigil->config)
		vigil_config_free(vigil->gospels[slot]);

	vigil->gospels[slot] = NULL;
	nyfe_mem_zero(digests[slot], sizeof(digests[slot]));

	last = 1;

	for (idx = 0; idx < VIGIL_GOSPEL_MAX; idx++) {
		if (vigil->gospels[idx] != NULL)
			last = 0;
	}

	if (last && vigil->config->mode != VIGIL_MODE_COMPANION)
		vigil_chat_cleanup();
}

/*
 * Detach all Gospel configurations we run.
 */
static void
vigil_gospel_detach_all(void)
{
	u_int8_t	slot;

	for (slot = 0; slot < VIGIL_GOSPEL_MAX; slot++) {
		if (vigil->gospels[slot] != NULL)
			vigil_gospel_detach(slot);
	}
}

/*
 * Find the slot of the Gospel configuration we run for the given flock,
 * returns -1 if we do not run one. Only called from the event loop.
 */
int
vigil_gospel_find(u_int64_t flock, u_int8_t *out)
{
	u_int8_t	slot;

	for (slot = 0; slot < VIGIL_GOSPEL_MAX; slot++) {
		if (vigil->gospels[slot] == NULL)
			continue;

		if (vigil->gospels[slot]->cathedral_flock == flock) {
			if (out != NULL)
				*out = slot;
			return (0);
		}
	}

	return (-1);
}

/*
 * Find the slot of the Gospel configuration that runs for the given
 * flock and keep our flock lock held, so the slot cannot change under
 * the caller until it calls vigil_gospel_release(). Returns -1 without
 * the lock if no Gospel configuration runs for the flock. May be called
 * from any thread.
 */
int
vigil_gospel_slot(u_int64_t flock, u_int8_t *out)
{
	u_int8_t	slot;

	PRECOND(out != NULL);

	if (flock == 0)
		return (-1);

	vigil_lock(&flock_lock);

	for (slot = 0; slot < VIGIL_GOSPEL_MAX; slot++) {
		if (flocks[slot] == flock) {
			*out = slot;
			return (0);
		}
	}

	vigil_unlock(&flock_lock);

	return (-1);
}

/*
 * Release the flock lock that vigil_gospel_slot() took.
 */
void
vigil_gospel_release(void)
{
	vigil_unlock(&flock_lock);
}

/*
 * Write the digest of the given configuration into out.
 */
static void
vigil_gospel_digest(const char *cfg, size_t len, u_int8_t *out, size_t outlen)
{
	struct nyfe_sha3	ctx;

	PRECOND(cfg != NULL);
	PRECOND(len > 0);
	PRECOND(out != NULL);
	PRECOND(outlen == VIGIL_DIGEST_LEN);

	nyfe_sha3_init256(&ctx);
	nyfe_sha3_update(&ctx, cfg, len);
	nyfe_sha3_final(&ctx, out, outlen);
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
 * Load the additional kdf input for the given peer into the given context,
 * if we have one. Returns 1 if we loaded an input, 0 if not.
 */
int
vigil_kyrka_additional_input(KYRKA *ctx, u_int64_t flock, u_int8_t peer)
{
	struct vigil_input	*input;
	int			idx, ret, set;

	PRECOND(ctx != NULL);
	PRECOND(flock != 0);
	PRECOND(peer > 0 && peer < KYRKA_PEERS_PER_FLOCK);

	ret = 0;
	set = 0;

	vigil_lock(&input_lock);

	for (idx = 0; idx < VIGIL_GOSPEL_MAX; idx++) {
		if (inputs[idx].flock != flock)
			continue;

		input = &inputs[idx].peers[peer];
		set = input->set;

		if (set) {
			ret = kyrka_additional_kdf_input(ctx,
			    input->ikm, sizeof(input->ikm));
		}
		break;
	}

	vigil_unlock(&input_lock);

	if (ret == -1)
		fatal("kyrka_additional_kdf_input: %d", kyrka_last_error(ctx));

	return (set);
}

/*
 * Erase all libkyrka contexts. We take kyrka_lock and never give it
 * back, anyone else wanting libkyrka blocks until the process is gone.
 */
static void
vigil_kyrka_erase(void)
{
	if (kyrka_held == 0) {
		(void)pthread_mutex_lock(&kyrka_lock);
		kyrka_held = 1;
	}

	kyrka_emergency_erase();
	nyfe_mem_zero(inputs, sizeof(inputs));
	vigil_safety_erase();
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
