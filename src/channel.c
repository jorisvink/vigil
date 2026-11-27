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
#include <netinet/in.h>

#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <opus/opus.h>

#include "vigil.h"

#include <libkyrka/libnyfe.h>

/* The milliseconds between key management and cathedral updates. */
#define CHANNEL_MANAGE_INTERVAL		(1 * 1000)
#define CHANNEL_NOTIFY_INTERVAL		(5 * 1000)

/* The milliseconds between heartbeats while nobody talks. */
#define CHANNEL_HEARTBEAT		(15 * 1000)

/* The milliseconds without hearing from a peer before it is offline. */
#define CHANNEL_RX_TIMEOUT		(40 * 1000)

/*
 * The milliseconds a new p2p path gets to prove it works and the least
 * time before we try a p2p path that failed again.
 */
#define CHANNEL_P2P_VERIFY		(30 * 1000)
#define CHANNEL_P2P_RETRY		(30 * 1000)
#define CHANNEL_P2P_RETRY_MAX		(1800 * 1000)

/* The milliseconds of p2p silence before we go back to the cathedral. */
#define CHANNEL_P2P_SILENT		(1 * 1000)

/* How long and how often we send heartbeats on a new p2p path. */
#define CHANNEL_HOLEPUNCH		(10 * 1000)
#define CHANNEL_HOLEPUNCH_INTERVAL	(1 * 1000)

/* The milliseconds a peer shows as talking after its last voice frame. */
#define CHANNEL_TALK_HOLD		500

/* The milliseconds we keep our audio after the last voice we had. */
#define CHANNEL_AUDIO_LINGER		(30 * 1000)

/* The number of over and leave frames we send. */
#define CHANNEL_BURST			3

/* The frames of a peer we let queue up before we start playing them. */
#define CHANNEL_PLAYBACK_PRIME		3

/* The silence we play once our audio started, the output may swallow it. */
#define CHANNEL_WARMUP			(VIGIL_VOICE_SAMPLE_RATE * 300 / 1000)

/* The beep we play once our microphone captures. */
#define CHANNEL_BEEP_HZ			880
#define CHANNEL_BEEP_LEVEL		6000
#define CHANNEL_BEEP_SAMPLES		(VIGIL_VOICE_SAMPLE_RATE * 100 / 1000)

/* The captured frames we let queue up. */
#define CHANNEL_CAPTURE_FRAMES		16

/* The milliseconds we block waiting for i/o when we run on our own. */
#define CHANNEL_IO_BUSY			10
#define CHANNEL_IO_IDLE			500

/* The milliseconds we retry resolving the cathedral. */
#define CHANNEL_RESOLVE_TIMEOUT		(15 * 1000)

/* What we tie the keys of our tunnels to our group with. */
#define CHANNEL_BIND_LABEL		"VIGIL.GROUP.VOICE"

/* What the status shows for a peer. */
#define CHANNEL_MEMBER_NONE		0
#define CHANNEL_MEMBER_LISTED		1
#define CHANNEL_MEMBER_ONLINE		2

/*
 * A peer in our channel and the tunnel we have with it.
 */
struct channel_peer {
	struct vigil_io		io;
	u_int8_t		id;
	int			left;

	KYRKA			*ctx;
	OpusDecoder		*decoder;
	struct vigil_cathedral	cathedral;

	struct sockaddr_in	addr;
	struct sockaddr_in	from;
	struct sockaddr_in	p2p_peer;
	struct sockaddr_in	p2p_failed;

	u_int16_t		rx_seq;

	u_int64_t		online;
	u_int64_t		last_rx;
	u_int64_t		p2p_rx;
	u_int64_t		holepunch;
	u_int64_t		p2p_retry;
	u_int64_t		p2p_verify;
	u_int64_t		p2p_backoff;
	u_int64_t		next_manage;
	u_int64_t		next_notify;
	u_int64_t		next_heartbeat;
};

/*
 * The channel we are in, only touched by the vigil core.
 */
struct channel_state {
	int			open;
	int			live;
	int			talking;
	int			embedded;
	u_int8_t		slot;
	u_int8_t		owner;
	u_int8_t		local;
	u_int16_t		tx_seq;
	u_int16_t		liturgy;
	u_int64_t		now;
	u_int64_t		flock;
	u_int64_t		group;
	u_int64_t		heard;
	struct sockaddr_in	home;

	struct vigil_io		io;
	KYRKA			*ctx;
	OpusEncoder		*encoder;
	struct vigil_config	*cfg;
	struct vigil_cathedral	cathedral;

	u_int64_t		rx;
	u_int64_t		tx;
	u_int64_t		next_notify;

	u_int8_t		listed[KYRKA_PEERS_PER_FLOCK];
	u_int8_t		roster[VIGIL_CHAT_GROUP_ROSTER];
	struct channel_peer	*peers[KYRKA_PEERS_PER_FLOCK];
};

static void	channel_send(void);
static void	channel_step(void);
static void	channel_loop(void);
static void	channel_close(void);
static void	channel_rebind(void);
static void	channel_publish(void);
static int	channel_stopped(void);
static void	channel_bind(KYRKA *);
static void	channel_burst(u_int8_t);
static void	channel_encode(const int16_t *);
static void	channel_members(const u_int8_t *);
static void	channel_ring_drop(u_int8_t);
static void	channel_beep(int16_t *, size_t);
static void	channel_ring_mix(u_int8_t, int32_t *, size_t);
static void	channel_roster(const u_int8_t *);
static int	channel_resolve(struct vigil_config *, const char *);
static void	channel_open(struct vigil_config *, u_int8_t, u_int64_t,
		    const u_int8_t *);
static u_int64_t	channel_request(u_int64_t *, u_int8_t *, u_int64_t *,
			    u_int8_t *);

static void	channel_io_add(struct vigil_io *);
static void	channel_io_del(struct vigil_io *);

static void	channel_wake(void);
static void	channel_wake_init(void);
static void	channel_wake_drain(void);

static void	channel_audio_update(void);
static void	channel_audio_want(int, int);

static void	channel_liturgy_check(void);
static void	channel_liturgy_read(void *);

static void	channel_kyrka_event(KYRKA *, union kyrka_event *, void *);
static void	channel_kyrka_send(struct kyrka_packet *, u_int64_t, void *);

static void	channel_peer_new(u_int8_t);
static void	channel_peer_read(void *);
static void	channel_peer_free(struct channel_peer *);
static int	channel_peer_online(struct channel_peer *);
static void	channel_peer_manage(struct channel_peer *);
static void	channel_peer_rebind(struct channel_peer *);
static void	channel_peer_event(KYRKA *, union kyrka_event *, void *);
static void	channel_peer_heaven(struct kyrka_packet *, u_int64_t, void *);
static void	channel_peer_frame(struct channel_peer *, u_int8_t,
		    const u_int8_t *, size_t);
static void	channel_peer_purgatory(struct kyrka_packet *, u_int64_t,
		    void *);
static void	channel_peer_cathedral(struct kyrka_packet *, u_int64_t,
		    void *);

static void	channel_p2p_fail(struct channel_peer *);
static void	channel_p2p_follow(struct channel_peer *);
static void	channel_p2p_abandon(struct channel_peer *);

static void	channel_cathedral_alive(struct channel_peer *);
static void	channel_cathedral_check(struct channel_peer *);
static int	channel_cathedral_pick(struct vigil_cathedral *);

/* Our current channel state. */
static struct channel_state	channel;

/* The channel we were asked to be in, may be set from any thread. */
static volatile u_int64_t	request_seq = 0;
static u_int8_t			request_owner = 0;
static u_int64_t		request_flock = 0;
static u_int64_t		request_group = 0;
static u_int8_t			request_roster[VIGIL_CHAT_GROUP_ROSTER];

/* Protects the request fields above, request_seq aside. */
static pthread_mutex_t		request_lock = PTHREAD_MUTEX_INITIALIZER;

/* The last request the vigil core handled. */
static u_int64_t		request_handled = 0;

/* Set by the platform, from any thread. */
static volatile int		muted = 0;
static volatile int		talk = 0;

/* Our wakeup pipe, created by the first run and never closed. */
static volatile int		wake[2] = { -1, -1 };

/* Set to 1 from the start of vigil_channel_run() until it returns. */
static volatile int		running = 0;

/* The id of the running channel and the highest id we were asked to stop. */
static volatile u_int64_t	current = 0;
static volatile u_int64_t	stopped = 0;

/* The last id handed out by vigil_channel_run_id(). */
static volatile u_int64_t	last_id = 0;

/* What we show in vigil_channel_status(). */
static volatile int		shown_alive = 0;
static volatile int		shown_ducked = 0;
static volatile int		shown_ready = 0;
static volatile int		shown_talking = 0;
static volatile u_int8_t	shown_owner = 0;
static volatile u_int64_t	shown_flock = 0;
static volatile u_int64_t	shown_group = 0;
static volatile u_int8_t	members[KYRKA_PEERS_PER_FLOCK];
static volatile u_int64_t	spoke[KYRKA_PEERS_PER_FLOCK];

/* Protects the platform audio we run against a call taking it over. */
static pthread_mutex_t		audio_lock = PTHREAD_MUTEX_INITIALIZER;

/* Set while we run the platform audio, and if we capture with it. */
static volatile int		audio_on = 0;
static int			audio_recording = 0;

/* The captured frames and the frames of each peer we play. */
static struct vigil_voice_ring	capture;
static struct vigil_voice_ring	*rings[KYRKA_PEERS_PER_FLOCK];

/* Set by the vigil core to have the platform audio beep. */
static volatile int		beep = 0;

/* Only touched by the platform audio. */
static size_t			warmup = 0;
static size_t			beeping = 0;
static size_t			capture_offset = 0;
static int16_t			*capture_frame = NULL;
static int			primed[KYRKA_PEERS_PER_FLOCK];
static size_t			offsets[KYRKA_PEERS_PER_FLOCK];
static int16_t			*playing[KYRKA_PEERS_PER_FLOCK];

/*
 * Turn on the voice of the group with the given owner and id in the given
 * flock, the roster says who is in the group. We leave the voice we are in
 * first, unless it is the same group, then we only take the new roster.
 * May be called from any thread.
 */
void
vigil_channel_join(u_int64_t flock, u_int8_t owner, u_int64_t group,
    const u_int8_t *roster)
{
	PRECOND(flock != 0);
	PRECOND(owner > 0 && owner < KYRKA_PEERS_PER_FLOCK);
	PRECOND(group != 0);
	PRECOND(roster != NULL);

	vigil_lock(&request_lock);

	request_owner = owner;
	request_flock = flock;
	request_group = group;
	memcpy(request_roster, roster, sizeof(request_roster));

	vigil_unlock(&request_lock);

	vigil_atomic_add(&request_seq, 1);
	channel_wake();
}

/*
 * Turn off the voice we are in, may be called from any thread.
 */
void
vigil_channel_leave(void)
{
	vigil_lock(&request_lock);

	request_owner = 0;
	request_flock = 0;
	request_group = 0;
	memset(request_roster, 0, sizeof(request_roster));

	vigil_unlock(&request_lock);

	vigil_atomic_add(&request_seq, 1);
	channel_wake();
}

/*
 * Stop or start playing what the peers in our channel say, we stay in the
 * channel either way. May be called from any thread.
 */
void
vigil_channel_mute(int on)
{
	PRECOND(on == 0 || on == 1);

	vigil_atomic_write(&muted, on);
	channel_wake();
}

/*
 * Start or stop talking in our channel, may be called from any thread.
 */
void
vigil_channel_talk(int on)
{
	PRECOND(on == 0 || on == 1);

	vigil_atomic_write(&talk, on);
	channel_wake();
}

/*
 * Run the voice we turned on with vigil_channel_join() on our own, using
 * the given Gospel configuration for the keys. We go via the given
 * cathedral (ip:port), or the one in the configuration if none is given.
 *
 * Each run needs an id from vigil_channel_run_id(), vigil_channel_stop()
 * stops the run with that id and all runs before it.
 *
 * This blocks until we are stopped or turned to the voice of another
 * group, returns -1 if we could not run.
 */
int
vigil_channel_run(u_int64_t id, const char *cfg, size_t cfglen,
    const char *cathedral)
{
	int			ret;
	u_int8_t		owner;
	struct vigil_config	*config;
	char			reason[128];
	u_int64_t		flock, group;
	u_int8_t		roster[VIGIL_CHAT_GROUP_ROSTER];

	PRECOND(id > 0);
	PRECOND(cfg != NULL);

	if (!vigil_atomic_cas_simple(&running, 0, 1)) {
		vigil_log(LOG_NOTICE, "[channel] a channel already runs");
		return (-1);
	}

	vigil_atomic_write(&current, id);

	if (channel_stopped()) {
		vigil_atomic_write(&running, 0);
		return (0);
	}

	if (wake[0] == -1)
		channel_wake_init();

	channel_wake_drain();

	config = vigil_config_parse(cfg, cfglen, reason, sizeof(reason));
	if (config == NULL) {
		vigil_log(LOG_ERR, "[channel] invalid configuration: %s",
		    reason);
		vigil_atomic_write(&running, 0);
		return (-1);
	}

	ret = -1;
	vigil_kyrka_enter();

	memset(&channel, 0, sizeof(channel));
	channel.now = vigil_time_ms();

	request_handled = channel_request(&flock, &owner, &group, roster);

	if (config->mode != VIGIL_MODE_GOSPEL) {
		vigil_log(LOG_ERR, "[channel] needs a gospel configuration");
	} else if (group == 0 || flock != config->cathedral_flock) {
		vigil_log(LOG_ERR, "[channel] no voice for this configuration");
	} else if (channel_resolve(config, cathedral) != -1) {
		channel_open(config, owner, group, roster);

		if (channel.open) {
			ret = 0;
			channel_loop();
			channel_close();
		}
	}

	vigil_kyrka_leave();
	vigil_config_free(config);
	vigil_atomic_write(&running, 0);

	return (ret);
}

/*
 * Returns a new id for vigil_channel_run(), higher than all those before
 * it. May be called from any thread.
 */
u_int64_t
vigil_channel_run_id(void)
{
	return (vigil_atomic_add(&last_id, 1) + 1);
}

/*
 * Request the run with the given id, and those before it, to stop.
 * May be called from any thread.
 */
void
vigil_channel_stop(u_int64_t id)
{
	u_int64_t	last;

	PRECOND(id > 0);

stop_again:
	last = vigil_atomic_read(&stopped);

	if (id > last && !vigil_atomic_cas(&stopped, &last, &id))
		goto stop_again;

	channel_wake();
}

/*
 * Copy the state of our channel into buf as a json object.
 * May be called from any thread.
 *
 * Returns the length of the json in buf, or 0 if buf was too small.
 */
size_t
vigil_channel_status(char *buf, size_t len)
{
	struct vigil_buf	out;
	u_int64_t		flock, now, at;
	u_int8_t		idx, state, sep;

	PRECOND(buf != NULL);
	PRECOND(len > 0);

	vigil_buf_init(&out, buf, len);
	flock = vigil_atomic_read(&shown_flock);

	if (flock == 0) {
		vigil_buf_appendf(&out, "{\"joined\":false}");
		if (out.flags & VIGIL_BUF_FULL)
			return (0);
		return (out.offset);
	}

	now = vigil_time_ms();

	vigil_buf_appendf(&out, "{\"joined\":true,\"flock\":\"%" PRIx64 "\","
	    "\"chat\":\"%" PRIx64 "-g%02x%016" PRIx64 "\",\"alive\":%s,"
	    "\"muted\":%s,\"talking\":%s,\"ready\":%s,\"ducked\":%s,"
	    "\"members\":[", flock, flock, vigil_atomic_read(&shown_owner),
	    vigil_atomic_read(&shown_group),
	    vigil_atomic_read(&shown_alive) ? "true" : "false",
	    vigil_atomic_read(&muted) ? "true" : "false",
	    vigil_atomic_read(&shown_talking) ? "true" : "false",
	    vigil_atomic_read(&shown_ready) ? "true" : "false",
	    vigil_atomic_read(&shown_ducked) ? "true" : "false");

	sep = 0;

	for (idx = 1; idx < KYRKA_PEERS_PER_FLOCK; idx++) {
		state = vigil_atomic_read(&members[idx]);
		if (state == CHANNEL_MEMBER_NONE)
			continue;

		at = vigil_atomic_read(&spoke[idx]);

		vigil_buf_appendf(&out, "%s{\"peer\":\"%" PRIx64 "-%02x\","
		    "\"online\":%s,\"talking\":%s}", sep ? "," : "", flock,
		    idx, state == CHANNEL_MEMBER_ONLINE ? "true" : "false",
		    (at != 0 && (now - at) < CHANNEL_TALK_HOLD) ?
		    "true" : "false");

		sep = 1;
	}

	vigil_buf_appendf(&out, "]}");

	if (out.flags & VIGIL_BUF_FULL)
		return (0);

	return (out.offset);
}

/*
 * Called from the vigil core event loop, we pick up a new channel to be
 * in and keep the one we are in running.
 */
void
vigil_channel_update(void)
{
	u_int8_t		slot, owner;
	u_int64_t		flock, group;
	u_int8_t		roster[VIGIL_CHAT_GROUP_ROSTER];

	PRECOND(vigil != NULL);

	if (vigil_atomic_read(&request_seq) != request_handled) {
		request_handled = channel_request(&flock, &owner, &group,
		    roster);

		if (channel.open && channel.embedded &&
		    channel.flock == flock && channel.owner == owner &&
		    channel.group == group) {
			channel.now = vigil->now;
			channel_roster(roster);
		} else {
			if (channel.open && channel.embedded)
				channel_close();

			if (group != 0 &&
			    vigil_gospel_find(flock, &slot) == -1) {
				vigil_log(LOG_NOTICE,
				    "[channel] no gospel for that flock");
			} else if (group != 0 && !channel.open) {
				memset(&channel, 0, sizeof(channel));

				channel.slot = slot;
				channel.embedded = 1;
				channel.now = vigil->now;

				channel_open(vigil->gospels[slot],
				    owner, group, roster);
			}
		}
	}

	if (!channel.open || !channel.embedded)
		return;

	channel.now = vigil->now;
	channel_step();
}

/*
 * Run our liturgy and our tunnels, send what we captured and publish how
 * our channel is doing.
 */
static void
channel_step(void)
{
	u_int8_t		idx;
	struct channel_peer	*peer;

	PRECOND(channel.open);

	channel_liturgy_check();

	if (channel.now >= channel.next_notify) {
		channel.next_notify = channel.now + VIGIL_CALL_NOTIFY;

		if (kyrka_cathedral_liturgy(channel.ctx, NULL, 0) == -1) {
			vigil_log(LOG_NOTICE, "[channel] liturgy: %d",
			    kyrka_last_error(channel.ctx));
		}
	}

	for (idx = 1; idx < KYRKA_PEERS_PER_FLOCK; idx++) {
		if ((peer = channel.peers[idx]) != NULL)
			channel_peer_manage(peer);
	}

	channel_audio_update();
	channel_send();
	channel_publish();
}

/*
 * Run our channel on our own until we are stopped.
 */
static void
channel_loop(void)
{
	int			ret;
	nfds_t			idx;
	nfds_t			count;
	u_int8_t		owner;
	u_int64_t		seen, next;
	u_int64_t		flock, group;
	struct pollfd		pfd[2 + KYRKA_PEERS_PER_FLOCK];
	u_int8_t		roster[VIGIL_CHAT_GROUP_ROSTER];
	struct vigil_io		*ios[2 + KYRKA_PEERS_PER_FLOCK];

	PRECOND(channel.open);
	PRECOND(!channel.embedded);
	PRECOND(wake[0] != -1);

	next = 0;
	seen = vigil_rebind_generation();

	while (!channel_stopped()) {
		count = 0;

		pfd[count].fd = wake[0];
		pfd[count].events = POLLIN;
		ios[count++] = NULL;

		for (idx = 1; idx < KYRKA_PEERS_PER_FLOCK; idx++) {
			if (channel.peers[idx] == NULL)
				continue;

			pfd[count].fd = channel.peers[idx]->io.fd;
			pfd[count].events = POLLIN;
			ios[count++] = &channel.peers[idx]->io;
		}

		pfd[count].fd = channel.io.fd;
		pfd[count].events = POLLIN;
		ios[count++] = &channel.io;

		vigil_kyrka_leave();
		ret = poll(pfd, count,
		    channel.talking ? CHANNEL_IO_BUSY : CHANNEL_IO_IDLE);
		vigil_kyrka_enter();

		if (ret == -1 && errno != EINTR)
			fatal("poll: %s", errno_s);

		channel.now = vigil_time_ms();

		if (pfd[0].revents & POLLIN)
			channel_wake_drain();

		for (idx = 1; idx < count; idx++) {
			if (!(pfd[idx].revents & (POLLIN | POLLERR | POLLHUP)))
				continue;

			ios[idx]->flags |= VIGIL_IO_READABLE;
			ios[idx]->handle(ios[idx]);
		}

		if (vigil_atomic_read(&request_seq) != request_handled) {
			request_handled = channel_request(&flock, &owner,
			    &group, roster);

			if (channel.flock != flock || channel.owner != owner ||
			    channel.group != group)
				break;

			channel_roster(roster);
		}

		if (vigil_rebind_due(&seen, &next, channel.now))
			channel_rebind();

		channel_step();
	}
}

/*
 * Give our liturgy and our tunnels new sockets, the network changed.
 */
void
vigil_channel_rebind(void)
{
	if (channel.open && channel.embedded)
		channel_rebind();
}

/*
 * Give our liturgy and our tunnels new sockets.
 */
static void
channel_rebind(void)
{
	int			fd;
	u_int8_t		idx;

	PRECOND(channel.open);

	if ((fd = vigil_socket_udp()) != -1) {
		channel_io_del(&channel.io);
		vigil_socket_close(channel.io.fd);

		channel.io.fd = fd;
		channel_io_add(&channel.io);
	}

	channel.next_notify = 0;

	for (idx = 1; idx < KYRKA_PEERS_PER_FLOCK; idx++) {
		if (channel.peers[idx] != NULL)
			channel_peer_rebind(channel.peers[idx]);
	}
}

/*
 * The Gospel configuration in the given slot goes away, so does our
 * channel if it runs over it.
 */
void
vigil_channel_detach(u_int8_t slot)
{
	PRECOND(slot < VIGIL_GOSPEL_MAX);

	if (channel.open && channel.embedded && channel.slot == slot)
		channel_close();
}

/*
 * Returns 1 while we talk in our channel, the vigil core then picks up
 * what we capture without delay.
 */
int
vigil_channel_busy(void)
{
	return (channel.open && channel.talking);
}

/*
 * Returns 1 once our microphone captures while we talk.
 * May be called from any thread.
 */
int
vigil_channel_ready(void)
{
	return (vigil_atomic_read(&shown_ready));
}

/*
 * Returns 1 while we run the platform audio. May be called from any thread.
 */
int
vigil_channel_audio(void)
{
	return (vigil_atomic_read(&audio_on));
}

/*
 * Stop the platform audio if we run it, a call is about to start it.
 * May be called from any thread.
 */
void
vigil_channel_audio_drop(void)
{
	vigil_lock(&audio_lock);

	if (vigil_atomic_read(&audio_on)) {
		vigil_log(LOG_INFO, "[channel] a call takes our audio");
		vigil_platform_audio_stop();
		vigil_atomic_write(&audio_on, 0);
		audio_recording = 0;
	}

	vigil_unlock(&audio_lock);
}

/*
 * Called by the platform audio when it wants samples to play, we mix
 * the frames of all peers that talk. While muted we drop what we hold.
 */
void
vigil_channel_playback(int16_t *pcm, size_t samples)
{
	u_int8_t	idx;
	int16_t		*out;
	size_t		count, sample, total;
	int32_t		mix[VIGIL_VOICE_SAMPLE_COUNT];

	PRECOND(pcm != NULL);

	out = pcm;
	total = samples;

	if (vigil_atomic_read(&muted)) {
		memset(pcm, 0, samples * sizeof(*pcm));

		for (idx = 1; idx < KYRKA_PEERS_PER_FLOCK; idx++)
			channel_ring_drop(idx);

		channel_beep(out, total);
		return;
	}

	if (warmup > 0) {
		count = samples;
		if (count > warmup)
			count = warmup;

		memset(pcm, 0, count * sizeof(*pcm));

		pcm += count;
		samples -= count;
		warmup -= count;
	}

	while (samples > 0) {
		count = samples;
		if (count > VIGIL_VOICE_SAMPLE_COUNT)
			count = VIGIL_VOICE_SAMPLE_COUNT;

		memset(mix, 0, count * sizeof(mix[0]));

		for (idx = 1; idx < KYRKA_PEERS_PER_FLOCK; idx++)
			channel_ring_mix(idx, mix, count);

		for (sample = 0; sample < count; sample++) {
			if (mix[sample] > INT16_MAX)
				pcm[sample] = INT16_MAX;
			else if (mix[sample] < INT16_MIN)
				pcm[sample] = INT16_MIN;
			else
				pcm[sample] = mix[sample];
		}

		pcm += count;
		samples -= count;
	}

	channel_beep(out, total);
}

/*
 * Called by the platform audio with captured samples, we queue them up
 * in frames for the vigil core to encode and send.
 */
void
vigil_channel_capture(const int16_t *pcm, size_t samples)
{
	size_t		count;

	PRECOND(pcm != NULL);
	PRECOND(capture_frame == NULL ||
	    capture_offset < VIGIL_VOICE_SAMPLE_COUNT);

	while (samples > 0) {
		if (capture_frame == NULL) {
			capture_offset = 0;
			capture_frame = vigil_voice_ring_produce(&capture);
			if (capture_frame == NULL)
				return;
		}

		count = VIGIL_VOICE_SAMPLE_COUNT - capture_offset;
		if (samples < count)
			count = samples;

		memcpy(&capture_frame[capture_offset], pcm,
		    count * sizeof(*pcm));

		pcm += count;
		samples -= count;
		capture_offset += count;

		if (capture_offset == VIGIL_VOICE_SAMPLE_COUNT) {
			capture_frame = NULL;
			vigil_voice_ring_produced(&capture);
		}
	}
}

/*
 * Turn on the voice of the given group using the given Gospel
 * configuration. We set up our liturgy, the tunnels follow once the
 * cathedral tells us who else has it on.
 */
static void
channel_open(struct vigil_config *config, u_int8_t owner, u_int64_t group,
    const u_int8_t *roster)
{
	struct kyrka_cathedral_cfg	cfg;

	PRECOND(config != NULL);
	PRECOND(owner > 0 && owner < KYRKA_PEERS_PER_FLOCK);
	PRECOND(group != 0);
	PRECOND(roster != NULL);
	PRECOND(channel.open == 0);

	channel.cfg = config;
	channel.owner = owner;
	channel.group = group;
	channel.liturgy = group ^ (group >> 16) ^ (group >> 32) ^
	    (group >> 48) ^ (owner << 8);

	memcpy(channel.roster, roster, sizeof(channel.roster));
	channel.local = channel.cfg->spi & 0xff;
	channel.flock = channel.cfg->cathedral_flock;

	if ((channel.io.fd = vigil_socket_udp()) == -1)
		return;

	channel.io.handle = channel_liturgy_read;
	channel_io_add(&channel.io);

	if ((channel.ctx = kyrka_ctx_alloc(channel_kyrka_event, NULL)) == NULL)
		fatal("failed to create channel context");

	memset(&cfg, 0, sizeof(cfg));

	cfg.group = channel.liturgy;
	cfg.send = channel_kyrka_send;
	cfg.tunnel = channel.local;
	cfg.identity = channel.cfg->cathedral_id;
	cfg.flock_src = channel.flock | VIGIL_CHANNEL_FLOCK_DOMAIN;
	cfg.flock_dst = cfg.flock_src;

	vigil_kyrka_cathedral(channel.ctx, &cfg, channel.cfg, 0,
	    channel.cfg->mtu);

	vigil_voice_codec_open(&channel.encoder, NULL);

	channel.open = 1;

	vigil_log(LOG_INFO, "[channel] voice on in %" PRIx64 "-g%02x%016"
	    PRIx64 " liturgy %04x", channel.flock, owner, group,
	    channel.liturgy);
}

/*
 * Leave our channel, we tell the peers we have a tunnel with first.
 */
static void
channel_close(void)
{
	u_int8_t		idx;
	struct vigil_voice_ring	*ring;

	PRECOND(channel.open);

	vigil_atomic_write(&talk, 0);
	channel_burst(VIGIL_CHANNEL_TYPE_LEAVE);

	for (idx = 1; idx < KYRKA_PEERS_PER_FLOCK; idx++) {
		if (channel.peers[idx] != NULL)
			channel_peer_free(channel.peers[idx]);
	}

	channel_audio_want(0, 0);

	if (channel.ctx != NULL) {
		channel_io_del(&channel.io);
		vigil_socket_close(channel.io.fd);
		kyrka_ctx_free(channel.ctx);
	}

	if (channel.encoder != NULL)
		opus_encoder_destroy(channel.encoder);

	vigil_log(LOG_INFO, "[channel] voice off in %" PRIx64 "-g%02x%016"
	    PRIx64, channel.flock, channel.owner, channel.group);

	for (idx = 1; idx < KYRKA_PEERS_PER_FLOCK; idx++) {
		ring = rings[idx];
		vigil_atomic_write(&rings[idx], NULL);
		free(ring);
	}

	nyfe_mem_zero(&channel, sizeof(channel));
	channel_publish();
}

/*
 * Send what we captured while we talk, the first frame tells us our
 * microphone is ready and we beep.
 */
static void
channel_send(void)
{
	int16_t		*pcm;

	PRECOND(channel.open);

	while ((pcm = vigil_voice_ring_consume(&capture)) != NULL) {
		if (channel.talking) {
			if (!channel.live) {
				vigil_log(LOG_INFO,
				    "[channel] microphone ready");
				channel.live = 1;
				vigil_atomic_write(&beep, 1);
			}

			channel_encode(pcm);
		}

		vigil_voice_ring_consumed(&capture);
	}
}

/*
 * Encode the given frame and send it to every peer we have a tunnel with.
 */
static void
channel_encode(const int16_t *pcm)
{
	int			len;
	u_int8_t		idx;
	struct channel_peer	*peer;
	u_int8_t		opus[VIGIL_VOICE_OPUS_MAX];

	PRECOND(pcm != NULL);
	PRECOND(channel.encoder != NULL);

	len = opus_encode(channel.encoder, pcm, VIGIL_VOICE_SAMPLE_COUNT,
	    opus, sizeof(opus));
	if (len < 0) {
		vigil_log(LOG_NOTICE, "[channel] opus_encode: %d", len);
		return;
	}

	channel.tx_seq++;
	if (channel.tx_seq == 0)
		channel.tx_seq = 1;

	channel.heard = channel.now;

	for (idx = 1; idx < KYRKA_PEERS_PER_FLOCK; idx++) {
		peer = channel.peers[idx];
		if (peer == NULL || peer->left || peer->online == 0)
			continue;

		channel_peer_frame(peer, VIGIL_CHANNEL_TYPE_VOICE, opus, len);
	}
}

/*
 * Start or stop talking and run the platform audio while we talk or
 * while we heard a peer not long ago.
 */
static void
channel_audio_update(void)
{
	int		ducked, sending, play, rec;

	PRECOND(channel.open);

	ducked = vigil_voice_active();

	if (vigil_atomic_read(&talk) && !ducked)
		sending = 1;
	else
		sending = 0;

	if (sending && !channel.talking) {
		vigil_log(LOG_INFO, "[channel] talking");
		(void)opus_encoder_ctl(channel.encoder, OPUS_RESET_STATE);
		channel.live = 0;
		channel.talking = 1;
		channel.heard = channel.now;
	}

	if (!sending && channel.talking) {
		vigil_log(LOG_INFO, "[channel] over");
		channel.live = 0;
		channel.talking = 0;
		channel_burst(VIGIL_CHANNEL_TYPE_OVER);
	}

	rec = sending;

	play = 0;

	if (!ducked && !vigil_atomic_read(&muted) &&
	    (channel.now - channel.heard) < CHANNEL_AUDIO_LINGER)
		play = 1;

	if (rec)
		play = 1;

	if (!channel.embedded && !ducked)
		play = 1;

	channel_audio_want(play, rec);
}

/*
 * Run the platform audio if play is 1, with capture if rec is 1. We do
 * not start it while a call runs.
 */
static void
channel_audio_want(int play, int rec)
{
	int		ret;
	u_int8_t	idx;

	PRECOND(play == 0 || play == 1);
	PRECOND(rec == 0 || rec == 1);

	vigil_lock(&audio_lock);

	if (vigil_atomic_read(&audio_on)) {
		if (play && audio_recording == rec) {
			vigil_unlock(&audio_lock);
			return;
		}

		vigil_platform_audio_stop();
		vigil_atomic_write(&audio_on, 0);
		audio_recording = 0;
	}

	if (play == 0 || vigil_voice_active()) {
		vigil_unlock(&audio_lock);
		return;
	}

	vigil_atomic_write(&capture.head, 0);
	vigil_atomic_write(&capture.tail, 0);

	capture.max = CHANNEL_CAPTURE_FRAMES;
	capture_frame = NULL;
	capture_offset = 0;

	beeping = 0;
	warmup = CHANNEL_WARMUP;

	for (idx = 1; idx < KYRKA_PEERS_PER_FLOCK; idx++) {
		primed[idx] = 0;
		offsets[idx] = 0;
		playing[idx] = NULL;
	}

	vigil_atomic_fence();
	vigil_atomic_write(&audio_on, 1);

	ret = vigil_platform_audio_start(rec);

	if (ret == -1 && rec) {
		vigil_log(LOG_NOTICE, "[channel] no microphone");
		rec = 0;
		ret = vigil_platform_audio_start(rec);
	}

	if (ret == -1) {
		vigil_log(LOG_ERR, "[channel] no audio");
		vigil_atomic_write(&audio_on, 0);
	} else {
		audio_recording = rec;
	}

	vigil_unlock(&audio_lock);
}

/*
 * Send a burst of frames of the given type to every peer we have a
 * tunnel with.
 */
static void
channel_burst(u_int8_t type)
{
	u_int8_t		idx;
	int			count;
	struct channel_peer	*peer;

	PRECOND(type == VIGIL_CHANNEL_TYPE_OVER ||
	    type == VIGIL_CHANNEL_TYPE_LEAVE);

	for (idx = 1; idx < KYRKA_PEERS_PER_FLOCK; idx++) {
		peer = channel.peers[idx];
		if (peer == NULL || peer->online == 0)
			continue;

		for (count = 0; count < CHANNEL_BURST; count++)
			channel_peer_frame(peer, type, NULL, 0);
	}
}

/*
 * Publish the state of our channel for vigil_channel_status().
 */
static void
channel_publish(void)
{
	u_int8_t		idx;
	struct channel_peer	*peer;
	u_int8_t		state;

	vigil_atomic_write(&shown_owner, channel.owner);
	vigil_atomic_write(&shown_group, channel.group);
	vigil_atomic_write(&shown_ready, channel.live);
	vigil_atomic_write(&shown_talking, channel.talking);
	vigil_atomic_write(&shown_alive, channel.cathedral.alive);

	if (channel.open)
		vigil_atomic_write(&shown_ducked, vigil_voice_active());
	else
		vigil_atomic_write(&shown_ducked, 0);

	for (idx = 1; idx < KYRKA_PEERS_PER_FLOCK; idx++) {
		peer = channel.peers[idx];

		if (peer == NULL || peer->left)
			state = CHANNEL_MEMBER_NONE;
		else if (channel_peer_online(peer))
			state = CHANNEL_MEMBER_ONLINE;
		else
			state = CHANNEL_MEMBER_LISTED;

		vigil_atomic_write(&members[idx], state);

		if (state == CHANNEL_MEMBER_NONE)
			vigil_atomic_write(&spoke[idx], 0);
	}

	if (channel.open)
		vigil_atomic_write(&shown_flock, channel.flock);
	else
		vigil_atomic_write(&shown_flock, 0);
}

/*
 * The cathedral told us who has the voice of our group on. We have a
 * tunnel with each of them that is in the roster of the group.
 */
static void
channel_members(const u_int8_t *listed)
{
	u_int8_t	idx;
	int		want;

	PRECOND(listed != NULL);
	PRECOND(channel.open);

	if (listed != channel.listed)
		memcpy(channel.listed, listed, sizeof(channel.listed));

	for (idx = 1; idx < KYRKA_PEERS_PER_FLOCK; idx++) {
		want = 0;

		if (idx != channel.local && channel.listed[idx] != 0 &&
		    (channel.roster[idx / 8] & (1 << (idx % 8))))
			want = 1;

		if (want && channel.peers[idx] == NULL)
			channel_peer_new(idx);

		if (!want && channel.peers[idx] != NULL)
			channel_peer_free(channel.peers[idx]);
	}
}

/*
 * Take the given roster of our group, tunnels with peers that are no
 * longer in it go away.
 */
static void
channel_roster(const u_int8_t *roster)
{
	PRECOND(roster != NULL);
	PRECOND(channel.open);

	memcpy(channel.roster, roster, sizeof(channel.roster));
	channel_members(channel.listed);
}

/*
 * Copy what we were asked for, a group of 0 means no voice at all.
 * Returns the request sequence we copied.
 */
static u_int64_t
channel_request(u_int64_t *flock, u_int8_t *owner, u_int64_t *group,
    u_int8_t *roster)
{
	u_int64_t	seq;

	PRECOND(flock != NULL);
	PRECOND(owner != NULL);
	PRECOND(group != NULL);
	PRECOND(roster != NULL);

	vigil_lock(&request_lock);

	seq = vigil_atomic_read(&request_seq);

	*owner = request_owner;
	*flock = request_flock;
	*group = request_group;
	memcpy(roster, request_roster, sizeof(request_roster));

	vigil_unlock(&request_lock);

	return (seq);
}

/*
 * Mix our beep into the given samples if one is due.
 */
static void
channel_beep(int16_t *pcm, size_t samples)
{
	size_t		idx;
	int32_t		value;

	PRECOND(pcm != NULL);

	if (vigil_atomic_read(&beep)) {
		vigil_atomic_write(&beep, 0);
		beeping = CHANNEL_BEEP_SAMPLES;
	}

	for (idx = 0; idx < samples && beeping > 0; idx++) {
		value = pcm[idx] + CHANNEL_BEEP_LEVEL * sin(2 * M_PI *
		    CHANNEL_BEEP_HZ * (CHANNEL_BEEP_SAMPLES - beeping) /
		    VIGIL_VOICE_SAMPLE_RATE);

		if (value > INT16_MAX)
			value = INT16_MAX;
		else if (value < INT16_MIN)
			value = INT16_MIN;

		pcm[idx] = value;
		beeping--;
	}
}

/*
 * Drop all frames the given ring holds.
 */
static void
channel_ring_drop(u_int8_t idx)
{
	struct vigil_voice_ring		*ring;

	PRECOND(idx < KYRKA_PEERS_PER_FLOCK);

	if ((ring = vigil_atomic_read(&rings[idx])) == NULL)
		return;

	if (playing[idx] != NULL) {
		playing[idx] = NULL;
		vigil_voice_ring_consumed(ring);
	}

	while (vigil_voice_ring_consume(ring) != NULL)
		vigil_voice_ring_consumed(ring);

	primed[idx] = 0;
}

/*
 * Mix the frames the given ring holds into mix, a peer that went quiet
 * waits for CHANNEL_PLAYBACK_PRIME frames before it plays again.
 */
static void
channel_ring_mix(u_int8_t idx, int32_t *mix, size_t samples)
{
	struct vigil_voice_ring		*ring;
	size_t				count, sample, pending;

	PRECOND(idx < KYRKA_PEERS_PER_FLOCK);
	PRECOND(mix != NULL);

	if ((ring = vigil_atomic_read(&rings[idx])) == NULL)
		return;

	while (samples > 0) {
		if (playing[idx] == NULL) {
			pending = vigil_voice_ring_pending(ring);
			if (!primed[idx] && pending < CHANNEL_PLAYBACK_PRIME)
				return;

			offsets[idx] = 0;
			playing[idx] = vigil_voice_ring_consume(ring);
			if (playing[idx] == NULL) {
				primed[idx] = 0;
				return;
			}

			primed[idx] = 1;
		}

		count = VIGIL_VOICE_SAMPLE_COUNT - offsets[idx];
		if (samples < count)
			count = samples;

		for (sample = 0; sample < count; sample++)
			mix[sample] += playing[idx][offsets[idx] + sample];

		mix += count;
		samples -= count;
		offsets[idx] += count;

		if (offsets[idx] == VIGIL_VOICE_SAMPLE_COUNT) {
			playing[idx] = NULL;
			vigil_voice_ring_consumed(ring);
		}
	}
}

/*
 * Assign a cathedral to our liturgy, or select another one if it
 * timed out.
 */
static void
channel_liturgy_check(void)
{
	PRECOND(channel.open);

	if (channel.cathedral.addr.sin_port == 0) {
		if (channel_cathedral_pick(&channel.cathedral) == -1)
			return;

		vigil_log(LOG_INFO, "[channel] liturgy %04x via %s",
		    channel.liturgy, vigil_address(&channel.cathedral.addr));
		return;
	}

	if ((channel.now - channel.cathedral.last) <= channel.cathedral.timeout)
		return;

	if (channel.cathedral.alive) {
		vigil_log(LOG_NOTICE, "[channel] liturgy %04x timed out",
		    channel.liturgy);
	}

	channel.cathedral.alive = 0;

	if (channel_cathedral_pick(&channel.cathedral) == -1) {
		channel.cathedral.last = channel.now;
		return;
	}

	channel.next_notify = 0;

	vigil_log(LOG_NOTICE, "[channel] liturgy %04x switching to %s",
	    channel.liturgy, vigil_address(&channel.cathedral.addr));
}

/*
 * Read incoming packets for our liturgy and hand them to its context.
 */
static void
channel_liturgy_read(void *udata)
{
	int			idx;
	size_t			len;
	ssize_t			ret;
	struct kyrka_packet	pkt;
	u_int8_t		*ptr;

	PRECOND(udata == &channel.io);
	PRECOND(channel.ctx != NULL);

	if (!(channel.io.flags & VIGIL_IO_READABLE))
		return;

	channel.io.flags &= ~VIGIL_IO_READABLE;

	for (idx = 0; idx < VIGIL_IO_BATCH; idx++) {
		ptr = kyrka_packet_recvbuf(channel.ctx, &pkt, &len);
		if (ptr == NULL)
			fatal("failed to get channel receive buffer");

		if ((ret = read(channel.io.fd, ptr, len)) == -1) {
			if (errno == EINTR)
				continue;
			if (errno != EWOULDBLOCK && errno != EAGAIN)
				vigil_log(LOG_NOTICE, "[channel] read: %s",
				    errno_s);
			break;
		}

		if (ret == 0)
			continue;

		channel.rx += ret;

		if (channel.embedded)
			vigil->gospel_rx += ret;

		pkt.length = ret;
		pkt.shroud = KYRKA_PACKET_SHROUD_CATHEDRAL;

		if (kyrka_purgatory_input(channel.ctx, &pkt) == -1) {
			vigil_log(LOG_NOTICE, "[channel] purgatory input: %d",
			    kyrka_last_error(channel.ctx));
		}
	}
}

/*
 * Callback from libkyrka when an event occurred on our liturgy.
 */
static void
channel_kyrka_event(KYRKA *ctx, union kyrka_event *evt, void *udata)
{
	PRECOND(ctx != NULL);
	PRECOND(evt != NULL);
	PRECOND(udata == NULL);

	switch (evt->type) {
	case KYRKA_EVENT_LITURGY_RECEIVED:
		channel.cathedral.last = channel.now;
		channel.cathedral.timeout = VIGIL_CATHEDRAL_TIMEOUT;

		if (channel.cathedral.alive == 0) {
			channel.cathedral.alive = 1;
			vigil_log(LOG_INFO,
			    "[channel] liturgy %04x cathedral %s is alive",
			    channel.liturgy,
			    vigil_address(&channel.cathedral.addr));
		}

		channel_members(evt->liturgy.peers);
		break;
	case KYRKA_EVENT_LOGMSG:
		vigil_log(LOG_INFO, "[channel] libkyrka: %s", evt->logmsg.log);
		break;
	default:
		vigil_log(LOG_NOTICE,
		    "[channel] received unexpected event %u", evt->type);
		break;
	}
}

/*
 * Callback from libkyrka when our liturgy has data for the cathedral.
 */
static void
channel_kyrka_send(struct kyrka_packet *pkt, u_int64_t magic, void *udata)
{
	size_t			len;
	u_int8_t		*ptr;

	PRECOND(pkt != NULL);
	PRECOND(udata == NULL);

	if (channel.cathedral.addr.sin_port == 0)
		return;

	ptr = kyrka_packet_sendbuf(channel.ctx, pkt, &len);
	if (ptr == NULL)
		fatal("failed to get channel sendbuf");

	if (sendto(channel.io.fd, ptr, len, 0,
	    (const struct sockaddr *)&channel.cathedral.addr,
	    sizeof(channel.cathedral.addr)) == -1) {
		if (errno != EAGAIN && errno != EWOULDBLOCK &&
		    !vigil_socket_unreachable(errno, 1))
			vigil_log(LOG_NOTICE, "[channel] sendto: %s", errno_s);
		return;
	}

	channel.tx += len;

	if (channel.embedded)
		vigil->gospel_tx += len;
}

/*
 * Returns 1 if we heard from the given peer recently.
 */
static int
channel_peer_online(struct channel_peer *peer)
{
	PRECOND(peer != NULL);

	if (peer->online == 0)
		return (0);

	return ((channel.now - peer->last_rx) < CHANNEL_RX_TIMEOUT);
}

/*
 * Set up a tunnel with the given peer in our channel, via a cathedral
 * picked at random from the ones we know.
 */
static void
channel_peer_new(u_int8_t id)
{
	struct vigil_cathedral		cat;
	struct kyrka_cathedral_cfg	cfg;
	struct vigil_voice_ring		*ring;
	struct channel_peer		*peer;

	PRECOND(id > 0 && id < KYRKA_PEERS_PER_FLOCK);
	PRECOND(channel.open);
	PRECOND(channel.peers[id] == NULL);

	memset(&cat, 0, sizeof(cat));

	if (channel_cathedral_pick(&cat) == -1)
		return;

	if (rings[id] == NULL) {
		if ((ring = calloc(1, sizeof(*ring))) == NULL)
			fatal("calloc: failed to allocate channel ring");

		ring->max = VIGIL_VOICE_RING_FRAMES;
		vigil_atomic_write(&rings[id], ring);
	}

	if ((peer = calloc(1, sizeof(*peer))) == NULL)
		fatal("calloc: failed to allocate channel peer");

	peer->io.fd = vigil_socket_udp();
	if (peer->io.fd == -1) {
		free(peer);
		return;
	}

	peer->id = id;
	peer->io.handle = channel_peer_read;

	memcpy(&peer->cathedral, &cat, sizeof(cat));
	memcpy(&peer->addr, &cat.addr, sizeof(cat.addr));

	peer->ctx = kyrka_ctx_alloc(channel_peer_event, peer);
	if (peer->ctx == NULL)
		fatal("failed to create channel peer context");

	channel_bind(peer->ctx);

	memset(&cfg, 0, sizeof(cfg));

	cfg.udata = peer;
	cfg.send = channel_peer_cathedral;
	cfg.tunnel = (channel.local << 8) | id;
	cfg.identity = channel.cfg->cathedral_id;
	cfg.flock_src = channel.flock | VIGIL_CHANNEL_FLOCK_DOMAIN;
	cfg.flock_dst = cfg.flock_src;

	if (channel.cfg->flags & VIGIL_CONFIG_FLAG_COMMIXTION)
		cfg.hops = VIGIL_COMMIXTION_HOPS;

	vigil_kyrka_cathedral(peer->ctx, &cfg, channel.cfg, 1,
	    channel.cfg->mtu);

	if (kyrka_heaven_ifc(peer->ctx, channel_peer_heaven, peer) == -1)
		fatal("failed to set heaven interface");

	if (kyrka_purgatory_ifc(peer->ctx, channel_peer_purgatory, peer) == -1)
		fatal("failed to set purgatory interface");

	vigil_voice_codec_open(NULL, &peer->decoder);
	channel_io_add(&peer->io);

	channel.peers[id] = peer;

	vigil_log(LOG_INFO, "[channel] %02x tunnel via %s",
	    id, vigil_address(&cat.addr));
}

/*
 * Tie the keys of the given tunnel to our group, a peer that has the voice
 * of another group on never agrees on keys with us.
 */
static void
channel_bind(KYRKA *ctx)
{
	struct nyfe_sha3	sha;
	u_int64_t		flock, group;
	u_int8_t		input[VIGIL_KEY_LENGTH];

	PRECOND(ctx != NULL);
	PRECOND(channel.open);

	flock = htobe64(channel.flock);
	group = htobe64(channel.group);

	nyfe_sha3_init256(&sha);
	nyfe_sha3_update(&sha, CHANNEL_BIND_LABEL,
	    sizeof(CHANNEL_BIND_LABEL) - 1);
	nyfe_sha3_update(&sha, &flock, sizeof(flock));
	nyfe_sha3_update(&sha, &channel.owner, sizeof(channel.owner));
	nyfe_sha3_update(&sha, &group, sizeof(group));
	nyfe_sha3_final(&sha, input, sizeof(input));

	if (kyrka_additional_kdf_input(ctx, input, sizeof(input)) == -1)
		fatal("kyrka_additional_kdf_input: %d", kyrka_last_error(ctx));
}

/*
 * Tear down our tunnel with the given peer.
 */
static void
channel_peer_free(struct channel_peer *peer)
{
	PRECOND(peer != NULL);
	PRECOND(channel.peers[peer->id] == peer);

	channel_io_del(&peer->io);
	vigil_socket_close(peer->io.fd);

	kyrka_ctx_free(peer->ctx);
	opus_decoder_destroy(peer->decoder);

	vigil_log(LOG_INFO, "[channel] %02x tunnel closed", peer->id);

	vigil_atomic_write(&spoke[peer->id], 0);

	channel.peers[peer->id] = NULL;

	nyfe_mem_zero(peer, sizeof(*peer));
	free(peer);
}

/*
 * Manage keys, keep the cathedral informed and send a heartbeat if our
 * peer has not heard from us for a while.
 */
static void
channel_peer_manage(struct channel_peer *peer)
{
	PRECOND(peer != NULL);
	PRECOND(peer->ctx != NULL);

	channel_cathedral_check(peer);

	if (peer->p2p_verify != 0 && channel.now >= peer->p2p_verify) {
		vigil_log(LOG_INFO, "[channel] %02x p2p via %s did not work",
		    peer->id, vigil_address(&peer->addr));
		channel_p2p_fail(peer);
	}

	if (channel.now >= peer->next_notify) {
		peer->next_notify = channel.now + CHANNEL_NOTIFY_INTERVAL;

		if (kyrka_cathedral_notify(peer->ctx) == -1) {
			vigil_log(LOG_NOTICE, "[channel] cathedral notify: %d",
			    kyrka_last_error(peer->ctx));
		}

		if ((channel.cfg->flags & VIGIL_CONFIG_FLAG_P2P) &&
		    kyrka_cathedral_nat_detection(peer->ctx) == -1) {
			vigil_log(LOG_NOTICE, "[channel] nat detection: %d",
			    kyrka_last_error(peer->ctx));
		}
	}

	if (channel.now >= peer->next_manage) {
		peer->next_manage = channel.now + CHANNEL_MANAGE_INTERVAL;

		if (kyrka_key_manage(peer->ctx) == -1 &&
		    kyrka_last_error(peer->ctx) != KYRKA_ERROR_NO_SECRET) {
			vigil_log(LOG_NOTICE, "[channel] key manage: %d",
			    kyrka_last_error(peer->ctx));
		}
	}

	if (peer->online == 0)
		return;

	if (channel.now >= peer->next_heartbeat)
		channel_peer_frame(peer, VIGIL_CHANNEL_TYPE_HEARTBEAT, NULL, 0);
}

/*
 * Give the tunnel with the given peer a new socket and go back to the
 * cathedral, the network changed so any p2p path we had is gone.
 */
static void
channel_peer_rebind(struct channel_peer *peer)
{
	int		fd;

	PRECOND(peer != NULL);
	PRECOND(peer->ctx != NULL);

	if ((fd = vigil_socket_udp()) == -1)
		return;

	channel_io_del(&peer->io);
	vigil_socket_close(peer->io.fd);

	peer->io.fd = fd;
	channel_io_add(&peer->io);

	peer->p2p_retry = 0;
	peer->p2p_verify = 0;
	peer->p2p_backoff = 0;
	peer->next_notify = 0;

	if (!vigil_inet_match(&peer->addr, &peer->cathedral.addr)) {
		memcpy(&peer->addr, &peer->cathedral.addr,
		    sizeof(peer->cathedral.addr));
		(void)kyrka_p2p_active(peer->ctx, 0);
	}
}

/*
 * Send a single frame of the given type with the given opus data to
 * the given peer, padded to VIGIL_VOICE_PAYLOAD.
 */
static void
channel_peer_frame(struct channel_peer *peer, u_int8_t type,
    const u_int8_t *data, size_t len)
{
	struct kyrka_packet	pkt;
	size_t			max;
	struct vigil_voice_hdr	*hdr;
	u_int8_t		*ptr;

	PRECOND(peer != NULL);
	PRECOND(peer->ctx != NULL);
	PRECOND(data != NULL || len == 0);
	PRECOND(len <= VIGIL_VOICE_OPUS_MAX);
	PRECOND(type == VIGIL_CHANNEL_TYPE_VOICE ||
	    type == VIGIL_CHANNEL_TYPE_OVER ||
	    type == VIGIL_CHANNEL_TYPE_HEARTBEAT ||
	    type == VIGIL_CHANNEL_TYPE_LEAVE);

	if (peer->online == 0)
		return;

	ptr = kyrka_packet_databuf(peer->ctx, &pkt, &max);
	if (ptr == NULL)
		fatal("failed to get channel data buffer");

	if (max < VIGIL_VOICE_PAYLOAD)
		fatal("channel payload does not fit");

	memset(ptr, 0, VIGIL_VOICE_PAYLOAD);

	hdr = (struct vigil_voice_hdr *)ptr;
	hdr->type = type;
	hdr->length = htons(len);

	if (type == VIGIL_CHANNEL_TYPE_VOICE)
		hdr->seq = htons(channel.tx_seq);

	if (data != NULL)
		memcpy(ptr + sizeof(*hdr), data, len);

	pkt.length = VIGIL_VOICE_PAYLOAD;

	if (vigil_inet_match(&peer->addr, &peer->cathedral.addr))
		pkt.shroud = KYRKA_PACKET_SHROUD_CATHEDRAL;
	else
		pkt.shroud = KYRKA_PACKET_SHROUD_PEER;

	if (kyrka_heaven_input(peer->ctx, &pkt) == -1 &&
	    kyrka_last_error(peer->ctx) != KYRKA_ERROR_NO_TX_KEY) {
		vigil_log(LOG_NOTICE, "[channel] kyrka_heaven_input: %d",
		    kyrka_last_error(peer->ctx));
	}

	if (channel.now < peer->holepunch) {
		peer->next_heartbeat = channel.now + CHANNEL_HOLEPUNCH_INTERVAL;
	} else {
		peer->next_heartbeat = channel.now + CHANNEL_HEARTBEAT;
	}
}

/*
 * Callback for when an event occurred on the socket of a tunnel,
 * we push what we read into its libkyrka context.
 */
static void
channel_peer_read(void *udata)
{
	int			idx;
	size_t			len;
	ssize_t			ret;
	struct kyrka_packet	pkt;
	u_int8_t		*ptr;
	struct sockaddr_in	from;
	struct channel_peer	*peer;
	socklen_t		fromlen;

	PRECOND(udata != NULL);

	peer = udata;
	PRECOND(peer->ctx != NULL);

	if (!(peer->io.flags & VIGIL_IO_READABLE))
		return;

	peer->io.flags &= ~VIGIL_IO_READABLE;

	for (idx = 0; idx < VIGIL_IO_BATCH; idx++) {
		ptr = kyrka_packet_recvbuf(peer->ctx, &pkt, &len);
		if (ptr == NULL)
			fatal("failed to get channel receive buffer");

		fromlen = sizeof(from);

		ret = recvfrom(peer->io.fd, ptr, len, 0,
		    (struct sockaddr *)&from, &fromlen);
		if (ret == -1) {
			if (errno == EINTR)
				continue;
			if (errno != EWOULDBLOCK && errno != EAGAIN)
				vigil_log(LOG_NOTICE, "[channel] read: %s",
				    errno_s);
			break;
		}

		if (ret == 0)
			continue;

		if (channel.embedded)
			vigil->gospel_rx += ret;

		pkt.length = ret;
		memcpy(&peer->from, &from, sizeof(from));

		if (vigil_inet_match(&from, &peer->cathedral.addr)) {
			channel_cathedral_alive(peer);
			pkt.shroud = KYRKA_PACKET_SHROUD_CATHEDRAL;
		} else {
			pkt.shroud = KYRKA_PACKET_SHROUD_PEER;
		}

		if (kyrka_purgatory_input(peer->ctx, &pkt) == -1 &&
		    kyrka_last_error(peer->ctx) != KYRKA_ERROR_NO_RX_KEY) {
			vigil_log(LOG_NOTICE,
			    "[channel] %02x purgatory input: %d",
			    peer->id, kyrka_last_error(peer->ctx));
		}
	}
}

/*
 * Callback from libkyrka when an event occurred on one of our tunnels.
 */
static void
channel_peer_event(KYRKA *ctx, union kyrka_event *evt, void *udata)
{
	struct sockaddr_in	sin;
	struct channel_peer	*peer;

	PRECOND(ctx != NULL);
	PRECOND(evt != NULL);
	PRECOND(udata != NULL);

	peer = udata;

	switch (evt->type) {
	case KYRKA_EVENT_KEYS_INFO:
		peer->next_notify = 0;

		if (evt->keys.tx_spi == 0)
			break;

		peer->last_rx = channel.now;

		if (peer->online != 0)
			break;

		vigil_log(LOG_INFO, "[channel] %02x online", peer->id);

		peer->online = channel.now;
		peer->next_heartbeat = 0;
		break;
	case KYRKA_EVENT_KEYS_ERASED:
		vigil_log(LOG_INFO, "[channel] %02x keys erased", peer->id);
		peer->online = 0;
		peer->p2p_verify = 0;
		memcpy(&peer->addr, &peer->cathedral.addr,
		    sizeof(peer->cathedral.addr));
		(void)kyrka_p2p_active(peer->ctx, 0);
		break;
	case KYRKA_EVENT_PEER_DISCOVERY:
		channel_cathedral_alive(peer);

		if (!(channel.cfg->flags & VIGIL_CONFIG_FLAG_P2P))
			break;

		memset(&sin, 0, sizeof(sin));
		sin.sin_family = AF_INET;
		sin.sin_port = evt->peer.port;
		sin.sin_addr.s_addr = evt->peer.ip;

		if ((evt->peer.flags & KYRKA_INFO_FLAG_SAME_EXTERNAL_IPV4) ||
		    (ntohl(sin.sin_addr.s_addr) >> 24) == 127) {
			memset(&peer->p2p_peer, 0, sizeof(peer->p2p_peer));
			memcpy(&sin, &peer->cathedral.addr, sizeof(sin));
		} else {
			memcpy(&peer->p2p_peer, &sin, sizeof(sin));
		}

		if (channel.now < peer->p2p_retry &&
		    vigil_inet_match(&sin, &peer->p2p_failed))
			memcpy(&sin, &peer->cathedral.addr, sizeof(sin));

		if (vigil_inet_match(&sin, &peer->addr))
			break;

		memcpy(&peer->addr, &sin, sizeof(sin));

		if (vigil_inet_match(&sin, &peer->cathedral.addr)) {
			peer->p2p_verify = 0;
			(void)kyrka_p2p_active(peer->ctx, 0);
		} else {
			vigil_log(LOG_INFO, "[channel] %02x p2p via %s",
			    peer->id, vigil_address(&sin));
			peer->p2p_rx = channel.now;
			peer->p2p_verify = channel.now + CHANNEL_P2P_VERIFY;
			peer->holepunch = channel.now + CHANNEL_HOLEPUNCH;
			peer->next_heartbeat = channel.now;
			(void)kyrka_p2p_active(peer->ctx, 1);
		}
		break;
	case KYRKA_EVENT_EXCHANGE_INFO:
		vigil_log(LOG_INFO, "[channel] exchange: %s",
		    evt->exchange.reason);
		break;
	case KYRKA_EVENT_LOGMSG:
		vigil_log(LOG_INFO, "[channel] libkyrka: %s", evt->logmsg.log);
		break;
	default:
		break;
	}
}

/*
 * Callback from libkyrka with a decrypted frame from one of our peers,
 * we decode the voice in it for playback unless we are muted.
 */
static void
channel_peer_heaven(struct kyrka_packet *pkt, u_int64_t seq, void *udata)
{
	const struct vigil_voice_hdr	*hdr;
	const u_int8_t			*data;
	struct channel_peer		*peer;

	PRECOND(pkt != NULL);
	PRECOND(udata != NULL);

	peer = udata;
	peer->last_rx = channel.now;

	if (vigil_inet_match(&peer->from, &peer->cathedral.addr))
		channel_p2p_abandon(peer);
	else
		channel_p2p_follow(peer);

	if (pkt->type != KYRKA_PACKET_TYPE_IP)
		return;

	if (pkt->length != VIGIL_VOICE_PAYLOAD)
		return;

	data = kyrka_packet_data(pkt);
	hdr = (const struct vigil_voice_hdr *)data;

	if (ntohs(hdr->length) > VIGIL_VOICE_OPUS_MAX)
		return;

	if (hdr->type == VIGIL_CHANNEL_TYPE_LEAVE) {
		if (!peer->left)
			vigil_log(LOG_INFO, "[channel] %02x left", peer->id);
		vigil_atomic_write(&spoke[peer->id], 0);
		peer->left = 1;
		return;
	}

	if (peer->left) {
		vigil_log(LOG_INFO, "[channel] %02x is back", peer->id);
		peer->left = 0;
	}

	switch (hdr->type) {
	case VIGIL_CHANNEL_TYPE_VOICE:
		break;
	case VIGIL_CHANNEL_TYPE_OVER:
		vigil_atomic_write(&spoke[peer->id], 0);
		return;
	default:
		return;
	}

	if (peer->online == 0)
		return;

	if ((channel.now - vigil_atomic_read(&spoke[peer->id])) >=
	    CHANNEL_TALK_HOLD)
		peer->rx_seq = 0;

	vigil_atomic_write(&spoke[peer->id], channel.now);

	if (vigil_atomic_read(&muted) || vigil_voice_active())
		return;

	channel.heard = channel.now;
	vigil_voice_decode(rings[peer->id], peer->decoder,
	    &peer->rx_seq, data);
}

/*
 * Callback from libkyrka with ciphertext to send to one of our peers.
 */
static void
channel_peer_purgatory(struct kyrka_packet *pkt, u_int64_t seq, void *udata)
{
	size_t			len;
	u_int8_t		*ptr;
	struct channel_peer	*peer;

	PRECOND(pkt != NULL);
	PRECOND(udata != NULL);

	peer = udata;

	ptr = kyrka_packet_sendbuf(peer->ctx, pkt, &len);
	if (ptr == NULL)
		fatal("failed to get channel send buffer");

	if (sendto(peer->io.fd, ptr, len, 0,
	    (const struct sockaddr *)&peer->addr, sizeof(peer->addr)) == -1) {
		if (errno != EAGAIN && errno != EWOULDBLOCK &&
		    !vigil_socket_unreachable(errno,
		    vigil_inet_match(&peer->addr, &peer->cathedral.addr)))
			vigil_log(LOG_NOTICE, "[channel] sendto: %s", errno_s);
		return;
	}

	if (channel.embedded)
		vigil->gospel_tx += len;
}

/*
 * Callback from libkyrka with data for the cathedral of a tunnel.
 */
static void
channel_peer_cathedral(struct kyrka_packet *pkt, u_int64_t magic, void *udata)
{
	size_t			len;
	struct sockaddr_in	sin;
	u_int8_t		*ptr;
	struct channel_peer	*peer;

	PRECOND(pkt != NULL);
	PRECOND(udata != NULL);

	peer = udata;

	memcpy(&sin, &peer->cathedral.addr, sizeof(sin));

	if (magic == KYRKA_CATHEDRAL_NAT_MAGIC)
		sin.sin_port = htons(channel.cfg->cathedral_nat_port);

	ptr = kyrka_packet_sendbuf(peer->ctx, pkt, &len);
	if (ptr == NULL)
		fatal("failed to get channel cathedral buffer");

	if (sendto(peer->io.fd, ptr, len, 0,
	    (const struct sockaddr *)&sin, sizeof(sin)) == -1) {
		if (errno != EAGAIN && errno != EWOULDBLOCK &&
		    !vigil_socket_unreachable(errno, 1))
			vigil_log(LOG_NOTICE, "[channel] sendto: %s", errno_s);
		return;
	}

	if (channel.embedded)
		vigil->gospel_tx += len;
}

/*
 * We got a packet from our peer that did not come via the cathedral.
 *
 * If it came in on our p2p path that path works. If we are relaying and it
 * came from where the cathedral said our peer is, our peer already uses
 * that path and we follow it.
 */
static void
channel_p2p_follow(struct channel_peer *peer)
{
	PRECOND(peer != NULL);
	PRECOND(peer->ctx != NULL);

	if (vigil_inet_match(&peer->from, &peer->addr)) {
		if (peer->p2p_verify != 0)
			peer->p2p_backoff = 0;

		peer->p2p_rx = channel.now;
		peer->p2p_retry = 0;
		peer->p2p_verify = 0;
		return;
	}

	if (!(channel.cfg->flags & VIGIL_CONFIG_FLAG_P2P))
		return;

	if (!vigil_inet_match(&peer->addr, &peer->cathedral.addr))
		return;

	if (!vigil_inet_match(&peer->from, &peer->p2p_peer))
		return;

	vigil_log(LOG_INFO, "[channel] %02x p2p via %s, following our peer",
	    peer->id, vigil_address(&peer->from));

	memcpy(&peer->addr, &peer->from, sizeof(peer->from));

	peer->p2p_rx = channel.now;
	peer->p2p_retry = 0;
	peer->p2p_verify = channel.now + CHANNEL_P2P_VERIFY;
	peer->holepunch = channel.now + CHANNEL_HOLEPUNCH;
	peer->next_heartbeat = channel.now;
	(void)kyrka_p2p_active(peer->ctx, 1);
}

/*
 * Our peer reaches us via the cathedral. If we did not hear from it over
 * p2p for a while it cannot use our p2p path, so we go back to the cathedral.
 */
static void
channel_p2p_abandon(struct channel_peer *peer)
{
	PRECOND(peer != NULL);
	PRECOND(peer->ctx != NULL);

	if (vigil_inet_match(&peer->addr, &peer->cathedral.addr))
		return;

	if (peer->p2p_verify != 0)
		return;

	if ((channel.now - peer->p2p_rx) < CHANNEL_P2P_SILENT)
		return;

	vigil_log(LOG_INFO, "[channel] %02x p2p via %s went silent",
	    peer->id, vigil_address(&peer->addr));

	channel_p2p_fail(peer);
}

/*
 * The p2p path we are on does not work, go back to the cathedral and
 * do not try it again for a while.
 */
static void
channel_p2p_fail(struct channel_peer *peer)
{
	PRECOND(peer != NULL);
	PRECOND(peer->ctx != NULL);

	if (peer->p2p_backoff == 0)
		peer->p2p_backoff = CHANNEL_P2P_RETRY;
	else
		peer->p2p_backoff *= 2;

	if (peer->p2p_backoff > CHANNEL_P2P_RETRY_MAX)
		peer->p2p_backoff = CHANNEL_P2P_RETRY_MAX;

	peer->p2p_verify = 0;
	peer->p2p_retry = channel.now + peer->p2p_backoff;

	memcpy(&peer->p2p_failed, &peer->addr, sizeof(peer->addr));
	memcpy(&peer->addr, &peer->cathedral.addr,
	    sizeof(peer->cathedral.addr));

	(void)kyrka_p2p_active(peer->ctx, 0);
}

/*
 * The cathedral the tunnel with the given peer goes via answered us.
 */
static void
channel_cathedral_alive(struct channel_peer *peer)
{
	PRECOND(peer != NULL);

	peer->cathedral.last = channel.now;
	peer->cathedral.timeout = VIGIL_CATHEDRAL_TIMEOUT;

	if (peer->cathedral.alive)
		return;

	peer->cathedral.alive = 1;
	vigil_log(LOG_INFO, "[channel] %02x cathedral %s is alive",
	    peer->id, vigil_address(&peer->cathedral.addr));
}

/*
 * Move the tunnel with the given peer to another cathedral if the one
 * it goes via stopped answering us.
 */
static void
channel_cathedral_check(struct channel_peer *peer)
{
	int		relayed;

	PRECOND(peer != NULL);

	if ((channel.now - peer->cathedral.last) <= peer->cathedral.timeout)
		return;

	if (peer->cathedral.alive) {
		vigil_log(LOG_NOTICE, "[channel] %02x cathedral %s timed out",
		    peer->id, vigil_address(&peer->cathedral.addr));
	}

	peer->cathedral.alive = 0;
	relayed = vigil_inet_match(&peer->addr, &peer->cathedral.addr);

	if (channel_cathedral_pick(&peer->cathedral) == -1) {
		peer->cathedral.last = channel.now;
		return;
	}

	vigil_log(LOG_NOTICE, "[channel] %02x switching to cathedral %s",
	    peer->id, vigil_address(&peer->cathedral.addr));

	if (relayed) {
		memcpy(&peer->addr, &peer->cathedral.addr,
		    sizeof(peer->cathedral.addr));
	}

	peer->next_notify = 0;
}

/*
 * Select a cathedral for our liturgy or a tunnel. The vigil core picks
 * one from what it knows, on our own we only have the one we were given.
 */
static int
channel_cathedral_pick(struct vigil_cathedral *cat)
{
	PRECOND(cat != NULL);

	if (channel.embedded)
		return (vigil_call_cathedral_pick(channel.slot, cat));

	cat->alive = 0;
	cat->last = channel.now;
	cat->timeout = VIGIL_CATHEDRAL_TIMEOUT_INIT;

	memcpy(&cat->addr, &channel.home, sizeof(channel.home));

	return (0);
}

/*
 * Resolve the given cathedral (ip:port), or the one in the given Gospel
 * configuration, we keep trying for a bit as we may just have woken up.
 */
static int
channel_resolve(struct vigil_config *config, const char *cathedral)
{
	unsigned long	port;
	char		*p, *ep;
	char		host[VIGIL_HOST_MAX];

	PRECOND(config != NULL);

	if (cathedral != NULL) {
		(void)snprintf(host, sizeof(host), "%s", cathedral);

		if ((p = strrchr(host, ':')) != NULL) {
			*(p)++ = '\0';

			errno = 0;
			port = strtoul(p, &ep, 10);

			if (errno == 0 && *ep == '\0' && port > 0 &&
			    port <= USHRT_MAX &&
			    vigil_resolve(host, port, &channel.home) != -1)
				return (0);
		}

		vigil_log(LOG_NOTICE, "[channel] bad cathedral %s", cathedral);
	}

	while (vigil_resolve(config->cathedral_host,
	    config->cathedral_port, &channel.home) == -1) {
		if (channel_stopped())
			return (-1);

		if ((vigil_time_ms() - channel.now) > CHANNEL_RESOLVE_TIMEOUT)
			return (-1);

		vigil_kyrka_leave();
		(void)poll(NULL, 0, CHANNEL_MANAGE_INTERVAL);
		vigil_kyrka_enter();
	}

	return (0);
}

/*
 * Have the given i/o watched by the vigil core if it runs us, on our own
 * channel_loop() polls it.
 */
static void
channel_io_add(struct vigil_io *io)
{
	PRECOND(io != NULL);

	if (channel.embedded)
		vigil_io_schedule(io);
}

/*
 * Stop watching the given i/o.
 */
static void
channel_io_del(struct vigil_io *io)
{
	PRECOND(io != NULL);

	if (channel.embedded)
		vigil_io_unschedule(io);
}

/*
 * Returns 1 if the running channel was asked to stop.
 */
static int
channel_stopped(void)
{
	return (vigil_atomic_read(&current) <= vigil_atomic_read(&stopped));
}

/*
 * Wake up whoever runs our channel, the vigil core or channel_loop().
 */
static void
channel_wake(void)
{
	int		fd;
	u_int8_t	byte;

	vigil_wake_signal();

	fd = vigil_atomic_read(&wake[1]);

	if (fd == -1 || !vigil_atomic_read(&running))
		return;

	byte = 1;

	if (write(fd, &byte, sizeof(byte)) == -1 &&
	    errno != EAGAIN && errno != EWOULDBLOCK)
		vigil_log(LOG_NOTICE, "[channel] wakeup write: %s", errno_s);
}

/*
 * Create our wakeup pipe.
 */
static void
channel_wake_init(void)
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
 * Read everything that was written to our wakeup pipe.
 */
static void
channel_wake_drain(void)
{
	u_int8_t	buf[32];

	PRECOND(wake[0] != -1);

	while (read(wake[0], buf, sizeof(buf)) > 0)
		;
}
