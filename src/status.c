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

#include <arpa/inet.h>

#include <inttypes.h>
#include <stdio.h>
#include <time.h>

#include "vigil.h"

/* Length of the time string in a log line. */
#define STATUS_WHEN_LEN		16

/* How often, in milliseconds, we render a new snapshot. */
#define STATUS_INTERVAL		250

/* The maximum size of a rendered snapshot. */
#define STATUS_MAX		(128 * 1024)

/*
 * A single log line with the wall clock time it was logged at.
 */
struct status_line {
	volatile u_int64_t	seq;
	char			when[STATUS_WHEN_LEN];
	char			line[VIGIL_LOG_LINE_MAX];
};

static void	status_publish(const char *);
static void	status_render(char *, size_t);
static void	status_call(struct vigil_buf *);

/* The last rendered snapshot, odd snapshot_seq while it is written. */
static volatile u_int64_t	snapshot_seq = 0;
static char			snapshot[STATUS_MAX] = "{\"running\":false";

/* The log ring buffer, seq keeps increasing. */
static struct status_line	lines[VIGIL_LOG_LINES];
static volatile u_int64_t	seq = 0;

/* When we last rendered, only touched from the event loop thread. */
static u_int64_t		last_render = 0;

/* Scratch space for rendering, only touched from the event loop thread. */
static char			scratch[STATUS_MAX];

/*
 * Initialise the status for a new run.
 */
void
vigil_status_init(void)
{
	last_render = 0;
}

/*
 * Reset the snapshot for a new run.
 */
void
vigil_status_reset(void)
{
	status_publish("{\"running\":false");
}

/*
 * Render a new snapshot if it is time to do so, or if force is 1.
 * Must be called from the event loop thread.
 */
void
vigil_status_update(int force)
{
	u_int64_t	now;

	PRECOND(force == 0 || force == 1);

	now = vigil_time_ms();

	if (force == 0 && last_render != 0 &&
	    (now - last_render) < STATUS_INTERVAL)
		return;

	last_render = now;
	status_render(scratch, sizeof(scratch));
	status_publish(scratch);
}

/*
 * Add a line to our log ring, may be called from any thread.
 */
void
vigil_status_log(const char *line)
{
	struct tm		tm;
	time_t			now;
	u_int64_t		idx;
	struct status_line	*sl;

	PRECOND(line != NULL);

	now = time(NULL);
	(void)localtime_r(&now, &tm);

	idx = vigil_atomic_add(&seq, 1) + 1;
	sl = &lines[(idx - 1) % VIGIL_LOG_LINES];

	vigil_atomic_write(&sl->seq, 0);
	vigil_atomic_fence();

	(void)strftime(sl->when, sizeof(sl->when), "%H:%M:%S", &tm);
	(void)snprintf(sl->line, sizeof(sl->line), "%s", line);

	vigil_atomic_write(&sl->seq, idx);
}

/*
 * Copy the current status into buf as a json object, including all log
 * lines we still have. May be called from any thread.
 *
 * Returns the length of the json in buf, or 0 if buf was too small.
 */
size_t
vigil_status(char *buf, size_t len)
{
	struct vigil_buf	out;
	struct status_line	*sl;
	size_t			size;
	int			comma;
	char			when[STATUS_WHEN_LEN];
	u_int64_t		idx, first, last, start;
	char			line[VIGIL_LOG_LINE_MAX];

	PRECOND(buf != NULL);
	PRECOND(len > 0);

	if (len < STATUS_MAX)
		return (0);

snapshot_again:
	while ((start = vigil_atomic_read(&snapshot_seq)) & 1)
		vigil_cpu_pause();

	size = strnlen(snapshot, STATUS_MAX - 1);
	memcpy(buf, snapshot, size);
	vigil_atomic_fence();

	if (vigil_atomic_read(&snapshot_seq) != start)
		goto snapshot_again;

	vigil_buf_init(&out, buf + size, len - size);
	vigil_buf_appendf(&out, ",\"log\":[");

	last = vigil_atomic_read(&seq);

	if (last > VIGIL_LOG_LINES)
		first = last - VIGIL_LOG_LINES + 1;
	else
		first = 1;

	comma = 0;

	for (idx = first; idx <= last; idx++) {
		sl = &lines[(idx - 1) % VIGIL_LOG_LINES];

		if (vigil_atomic_read(&sl->seq) != idx)
			continue;

		memcpy(when, sl->when, sizeof(when));
		memcpy(line, sl->line, sizeof(line));
		vigil_atomic_fence();

		if (vigil_atomic_read(&sl->seq) != idx)
			continue;

		when[sizeof(when) - 1] = '\0';
		line[sizeof(line) - 1] = '\0';

		vigil_buf_appendf(&out,
		    "%s{\"seq\":%" PRIu64 ",\"when\":", comma ? "," : "", idx);
		vigil_buf_quote(&out, when);
		vigil_buf_appendf(&out, ",\"line\":");
		vigil_buf_quote(&out, line);
		vigil_buf_appendf(&out, "}");

		comma = 1;
	}

	vigil_buf_appendf(&out, "]}");

	if (out.flags & VIGIL_BUF_FULL)
		return (0);

	return (size + out.offset);
}

/*
 * Append a liturgy of the given kind with its traffic as a json object.
 */
void
vigil_status_liturgy(struct vigil_buf *buf, const char *kind,
    u_int16_t group, struct vigil_cathedral *cat, u_int64_t rx, u_int64_t tx)
{
	PRECOND(buf != NULL);
	PRECOND(kind != NULL);
	PRECOND(cat != NULL);

	vigil_buf_appendf(buf,
	    "{\"kind\":\"%s\",\"group\":\"%04x\",\"alive\":%s,",
	    kind, group, cat->alive ? "true" : "false");

	if (cat->addr.sin_port == 0) {
		vigil_buf_appendf(buf, "\"cathedral\":null,");
	} else {
		vigil_buf_appendf(buf, "\"cathedral\":\"%s\",",
		    vigil_address(&cat->addr));
	}

	vigil_buf_appendf(buf,
	    "\"rx\":%" PRIu64 ",\"tx\":%" PRIu64 "}", rx, tx);
}

/*
 * Publish a new snapshot, readers retry their copy while we write it.
 */
static void
status_publish(const char *buf)
{
	PRECOND(buf != NULL);

	vigil_atomic_add(&snapshot_seq, 1);
	(void)snprintf(snapshot, sizeof(snapshot), "%s", buf);
	vigil_atomic_add(&snapshot_seq, 1);
}

/*
 * Render the state of the event loop into buf, without the closing brace
 * as vigil_status() appends the log lines to it.
 */
static void
status_render(char *buf, size_t len)
{
	int			idx;
	struct vigil_buf	out;
	struct in_addr		tun, net;
	struct vigil_config	*cfg;
	struct vigil_peer	*peer;
	struct sockaddr_in	sin, *addr;
	char			ip[INET_ADDRSTRLEN];

	PRECOND(buf != NULL);
	PRECOND(len > 0);

	vigil_buf_init(&out, buf, len);

	if (vigil == NULL || !(vigil->flags & VIGIL_FLAG_RUNNING) ||
	    vigil->config == NULL) {
		vigil_buf_appendf(&out, "{\"running\":false");
		return;
	}

	cfg = vigil->config;

	vigil_buf_appendf(&out, "{\"running\":true,\"instance\":");
	vigil_buf_quote(&out, cfg->instance);
	vigil_buf_appendf(&out, ",\"mode\":\"%s\"", vigil_config_mode(cfg));

	if (cfg->flags & VIGIL_CONFIG_FLAG_TIER6)
		vigil_buf_appendf(&out, ",\"tier6\":true");
	vigil_buf_appendf(&out, ",\"uptime\":%" PRIu64,
	    (vigil->now - vigil->started) / 1000);

	(void)inet_ntop(AF_INET, &vigil->ip, ip, sizeof(ip));
	vigil_buf_appendf(&out, ",\"address\":\"%s\"", ip);

	if (cfg->mode != VIGIL_MODE_TUNNEL &&
	    vigil->cathedral.addr.sin_port != 0) {
		vigil_buf_appendf(&out, ",\"cathedral\":\"%s\"",
		    vigil_address(&vigil->cathedral.addr));
	}

	if (cfg->mode == VIGIL_MODE_LITURGY) {
		vigil_buf_appendf(&out, ",\"liturgy_alive\":%s",
		    vigil_liturgy_alive() ? "true" : "false");

		addr = vigil_liturgy_cathedral();
		if (addr != NULL) {
			vigil_buf_appendf(&out,
			    ",\"liturgy_cathedral\":\"%s\"",
			    vigil_address(addr));
		}

	}

	if (vigil->gospel != NULL) {
		vigil_buf_appendf(&out,
		    ",\"gospel\":{\"rx_bytes\":%" PRIu64
		    ",\"tx_bytes\":%" PRIu64 "}",
		    vigil->gospel_rx, vigil->gospel_tx);

		status_call(&out);
		vigil_chat_status(&out);
	}

	if (cfg->mode != VIGIL_MODE_TUNNEL) {
		vigil_buf_appendf(&out, ",\"remembrances\":[");

		for (idx = 0; idx < KYRKA_CATHEDRALS_MAX; idx++) {
			if (vigil_remembrance_address(&vigil->remembrance,
			    idx, &sin) == -1)
				break;

			vigil_buf_appendf(&out, "%s\"%s\"",
			    idx > 0 ? "," : "", vigil_address(&sin));
		}

		vigil_buf_appendf(&out, "]");
	}

	if (cfg->flags & VIGIL_CONFIG_FLAG_TIER6)
		vigil_inet_tier6(&net, cfg->cathedral_flock, 0);

	vigil_buf_appendf(&out, ",\"peers\":[");

	LIST_FOREACH(peer, vigil_peer_list(), list) {
		vigil_buf_appendf(&out,
		    "%s{\"id\":\"%02x\",\"address\":\"%s\"",
		    peer == LIST_FIRST(vigil_peer_list()) ? "" : ",",
		    peer->id, vigil_address(&peer->addr));

		if (cfg->mode != VIGIL_MODE_TUNNEL) {
			vigil_buf_appendf(&out,
			    ",\"cathedral\":\"%s\"",
			    vigil_address(&peer->cathedral.addr));
		}

		if (cfg->flags & VIGIL_CONFIG_FLAG_TIER6) {
			tun.s_addr = net.s_addr | htonl(peer->id);
			(void)inet_ntop(AF_INET, &tun, ip, sizeof(ip));
			vigil_buf_appendf(&out, ",\"tunnel\":\"%s\"", ip);
		} else if (cfg->mode == VIGIL_MODE_LITURGY) {
			vigil_inet_liturgy(&tun, &cfg->liturgy_prefix,
			    peer->id, cfg->spi & 0xff);
			(void)inet_ntop(AF_INET, &tun, ip, sizeof(ip));
			vigil_buf_appendf(&out, ",\"tunnel\":\"%s\"", ip);
		}

		vigil_buf_appendf(&out,
		    ",\"online\":%s,\"p2p\":%s,\"cathedral_alive\":%s",
		    (peer->flags & VIGIL_PEER_FLAG_ONLINE) ? "true" : "false",
		    (peer->flags & VIGIL_PEER_FLAG_P2P) ? "true" : "false",
		    peer->cathedral.alive ? "true" : "false");

		vigil_buf_appendf(&out,
		    ",\"tx_spi\":\"%08x\",\"rx_spi\":\"%08x\""
		    ",\"ambry\":\"%08x\"",
		    peer->tx_spi, peer->rx_spi, peer->ambry);

		vigil_buf_appendf(&out, ",\"mtu\":%u",
		    peer->mtu != 0 ? peer->mtu : cfg->mtu);

		vigil_buf_appendf(&out,
		    ",\"rx_bytes\":%" PRIu64 ",\"tx_bytes\":%" PRIu64
		    ",\"rx_pkts\":%" PRIu64 ",\"tx_pkts\":%" PRIu64
		    ",\"idle\":%" PRIu64 "}",
		    peer->rx_bytes, peer->tx_bytes,
		    peer->rx_pkts, peer->tx_pkts,
		    (vigil->now - peer->alive) / 1000);
	}

	vigil_buf_appendf(&out, "]");
}

/*
 * Append the signaling state for calls, which peer we are calling
 * and which peers are calling us.
 */
static void
status_call(struct vigil_buf *buf)
{
	struct sockaddr_in	*sin;
	int			idx, state, first;

	PRECOND(buf != NULL);

	vigil_buf_appendf(buf, ",\"call\":{\"alive\":%s",
	    vigil_call_alive() ? "true" : "false");

	vigil_call_liturgies(buf);

	sin = vigil_call_cathedral();
	if (sin != NULL) {
		vigil_buf_appendf(buf, ",\"cathedral\":\"%s\"",
		    vigil_address(sin));
	}

	first = 1;
	vigil_buf_appendf(buf, ",\"peers\":[");

	for (idx = 1; idx < KYRKA_PEERS_PER_FLOCK; idx++) {
		if (!(vigil_call_peer(idx) & VIGIL_CALL_PEER_ONLINE))
			continue;

		vigil_buf_appendf(buf, "%s\"%02x\"", first ? "" : ",", idx);
		first = 0;
	}

	vigil_buf_appendf(buf, "],\"dialing\":");

	for (idx = 1; idx < KYRKA_PEERS_PER_FLOCK; idx++) {
		state = vigil_call_peer(idx);
		if (state & VIGIL_CALL_PEER_WANTED)
			break;
	}

	if (idx < KYRKA_PEERS_PER_FLOCK) {
		vigil_buf_appendf(buf, "\"%02x\",\"reachable\":%s",
		    idx, (state & VIGIL_CALL_PEER_PRESENT) ? "true" : "false");
	} else {
		vigil_buf_appendf(buf, "null");
	}

	first = 1;
	vigil_buf_appendf(buf, ",\"incoming\":[");

	for (idx = 1; idx < KYRKA_PEERS_PER_FLOCK; idx++) {
		state = vigil_call_peer(idx) & VIGIL_CALL_PEER_SIGNALING;
		if (state != VIGIL_CALL_PEER_PRESENT)
			continue;

		vigil_buf_appendf(buf, "%s\"%02x\"", first ? "" : ",", idx);
		first = 0;
	}

	vigil_buf_appendf(buf, "]}");
}
