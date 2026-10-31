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
#include <stdio.h>
#include <unistd.h>

#include <opus/opus.h>

#include "vigil.h"

#include <libkyrka/libnyfe.h>

/* The milliseconds we block waiting for i/o. */
#define VOICE_IO_WAIT			10

/* The milliseconds between key management and cathedral updates. */
#define VOICE_MANAGE_INTERVAL		(1 * 1000)
#define VOICE_NOTIFY_INTERVAL		(5 * 1000)

/* The milliseconds we wait for the peer to answer. */
#define VOICE_CONNECT_TIMEOUT		(60 * 1000)

/* The milliseconds a peer calling us gets to show up on our tunnel. */
#define VOICE_RING_VERIFY		(10 * 1000)

/* The milliseconds we wait for a peer we declined to show up again. */
#define VOICE_DECLINE_WAIT		(5 * 1000)

/* The milliseconds without hearing from the peer before we give up. */
#define VOICE_RX_TIMEOUT		(20 * 1000)

/* The milliseconds a peer that calls us may be gone from signaling. */
#define VOICE_SIGNAL_GRACE		(5 * 1000)

/*
 * The milliseconds a new p2p path gets to prove it works, our peer may
 * only hear about it from the cathedral a notify interval after us.
 * A p2p path that failed is not tried again for at least VOICE_P2P_RETRY.
 */
#define VOICE_P2P_VERIFY		(10 * 1000)
#define VOICE_P2P_RETRY			(30 * 1000)
#define VOICE_P2P_RETRY_MAX		(1800 * 1000)

/*
 * The milliseconds we go without hearing from our peer over p2p while it
 * reaches us via the cathedral before we go back to the cathedral too.
 */
#define VOICE_P2P_SILENT		(1 * 1000)

/* The milliseconds we retry resolving the cathedral. */
#define VOICE_RESOLVE_TIMEOUT		(15 * 1000)

/* The number of hangup frames we send, in case some get lost. */
#define VOICE_HANGUP_COUNT		3

/*
 * We cap our voice and thus video at around 1200 bytes on the wire,
 * which translates to like 1100 ish mtu max.
 */
#define VOICE_MTU_MAX			1100

/* Our voice socket buffers, video may arrive in bursts. */
#define VOICE_SOCKET_BUFFER		(2 * 1024 * 1024)

/* How long and how often we tell a peer we declined its call. */
#define VOICE_DECLINE_LINGER		1000
#define VOICE_DECLINE_INTERVAL		200

/* The number of frames we queue up, must be a power of 2. */
#define VOICE_RING_FRAMES		16

/* The frames we let queue up before we start playing. */
#define VOICE_PLAYBACK_PRIME		3

/*
 * The tone we play until we hear our peer, a ringback when we call
 * and short pips once we answered.
 */
#define VOICE_TONE_HZ			425
#define VOICE_TONE_LEVEL		4000
#define VOICE_TONE_NONE			0
#define VOICE_TONE_RINGBACK		1
#define VOICE_TONE_PIPS			2

/* The milliseconds a tone is on and its period. */
#define VOICE_RINGBACK_ON		1000
#define VOICE_RINGBACK_PERIOD		5000
#define VOICE_PIPS_ON			150
#define VOICE_PIPS_PERIOD		1000

/*
 * A single producer, single consumer ring of audio frames between the
 * voice thread and the platform audio.
 */
struct voice_ring {
	volatile u_int32_t	head;
	volatile u_int32_t	tail;
	int16_t			frames[VOICE_RING_FRAMES]
				    [VIGIL_VOICE_SAMPLE_COUNT];
};

/*
 * The state of the running voice session, only touched by its thread.
 */
struct voice_session {
	int			fd;
	int			end;
	int			mode;
	int			rang;
	u_int8_t		peer;
	int			audio;
	int			owned;

	KYRKA			*ctx;
	struct vigil_config	*config;

	OpusEncoder		*encoder;
	OpusDecoder		*decoder;

	struct sockaddr_in	addr;
	struct sockaddr_in	from;
	struct sockaddr_in	cathedral;
	struct sockaddr_in	p2p_peer;
	struct sockaddr_in	p2p_failed;

	u_int16_t		mtu;
	u_int16_t		rx_seq;
	u_int16_t		tx_seq;

	u_int64_t		now;
	u_int64_t		online;
	u_int64_t		last_rx;
	u_int64_t		started;
	u_int64_t		declined;
	u_int64_t		p2p_rx;
	u_int64_t		p2p_retry;
	u_int64_t		p2p_verify;
	u_int64_t		p2p_backoff;
	u_int64_t		next_hangup;
	u_int64_t		next_manage;
	u_int64_t		next_notify;
};

static void	voice_session_loop(struct voice_session *);
static int	voice_session_setup(struct voice_session *);
static void	voice_session_close(struct voice_session *);
static void	voice_session_rebind(struct voice_session *);
static void	voice_session_answer(struct voice_session *);
static void	voice_session_hangup(struct voice_session *);
static void	voice_session_manage(struct voice_session *);
static void	voice_session_cleanup(struct voice_session *);
static int	voice_session_resolve(struct voice_session *, const char *);
static int	voice_session_open(struct voice_session *,
		    struct vigil_config *, u_int8_t, int);

static int	voice_cathedral_resolve(struct voice_session *, const char *);
static void	voice_cathedral_send(struct kyrka_packet *, u_int64_t, void *);

static void	voice_io_event(void *);
static void	voice_io_read(struct voice_session *);

static const char	*voice_end_name(int);
static int		voice_stop_check(void);
static void		voice_rings_reset(void);
static void		voice_tone_fill(int16_t *, size_t);
static void		voice_p2p_fail(struct voice_session *);
static void		voice_p2p_follow(struct voice_session *);
static void		voice_p2p_abandon(struct voice_session *);
static void		voice_socket_buffers(int);
static void		voice_audio_send(struct voice_session *);
static void		voice_video_update(struct voice_session *);
static void		voice_video_link(struct voice_session *,
			    struct vigil_video_link *);
static void		voice_hangup_send(struct voice_session *);
static void		voice_path_publish(struct voice_session *);
static void		voice_kyrka_event(KYRKA *, union kyrka_event *, void *);
static void		voice_heaven_input(struct kyrka_packet *, u_int64_t,
			    void *);
static void		voice_purgatory_input(struct kyrka_packet *, u_int64_t,
			    void *);
static void		voice_frame_send(struct voice_session *, u_int8_t,
			    const u_int8_t *, size_t);

static void	voice_wake_init(void);
static void	voice_wake_drain(void);

static size_t	voice_ring_pending(struct voice_ring *);
static void	voice_ring_produced(struct voice_ring *);
static void	voice_ring_consumed(struct voice_ring *);
static int16_t	*voice_ring_produce(struct voice_ring *);
static int16_t	*voice_ring_consume(struct voice_ring *);

/* Our wakeup pipe, created by the first session and never closed. */
static volatile int		wake[2] = { -1, -1 };

/* Set to 1 from the start of vigil_voice_run() until all is cleaned up. */
static volatile int		active = 0;

/* The id of the active session and the highest id we were asked to stop. */
static volatile u_int64_t	current = 0;
static volatile u_int64_t	stopped = 0;

/* The last id handed out by vigil_voice_run_id(). */
static volatile u_int64_t	last_id = 0;

/* The session run by the vigil core and its i/o event. */
static struct voice_session	embedded;
static struct vigil_io		embedded_io;
static int			embedded_open = 0;

/* The calls requested from the vigil core, may be set from any thread. */
static volatile u_int64_t	request_seq = 0;
static volatile u_int8_t	request_peer = 0;
static volatile int		request_mode = 0;
static volatile u_int64_t	request_hangup = 0;

/* The last request the vigil core handled. */
static u_int64_t		request_handled = 0;

/* Set once a ringing session is answered, may be set from any thread. */
static volatile int		answered = 0;

/* What we show in vigil_voice_status(). */
static volatile u_int64_t	ended = 0;
static volatile int		muted = 0;
static volatile u_int8_t	peer_id = 0;
static volatile u_int64_t	talking = 0;
static volatile u_int64_t	rx_bytes = 0;
static volatile u_int64_t	tx_bytes = 0;
static volatile u_int64_t	rx_frames = 0;
static volatile u_int64_t	tx_frames = 0;
static volatile int		state = VIGIL_VOICE_STATE_IDLE;
static volatile int		ended_why = VIGIL_VOICE_END_LOCAL;

/* Where we send to and the cathedral we relay via, for vigil_voice_status(). */
static volatile u_int32_t	path_ip = 0;
static volatile u_int32_t	relay_ip = 0;
static volatile u_int16_t	path_port = 0;
static volatile u_int16_t	relay_port = 0;

/* The captured frames, filled by the platform audio. */
static struct voice_ring	capture;

/* The frames to play, filled by the voice thread. */
static struct voice_ring	playback;

/* Only touched by the platform audio capture. */
static size_t			capture_offset = 0;
static int16_t			*capture_frame = NULL;

/* The tone to play until we hear our peer. */
static volatile int		tone = VOICE_TONE_NONE;

/* Only touched by the platform audio playback. */
static u_int64_t		tone_sample = 0;
static int			playback_primed = 0;
static size_t			playback_offset = 0;
static int16_t			*playback_frame = NULL;

/*
 * Run a voice session with the given peer, using the given Gospel
 * configuration for the keys. The session goes via the given cathedral
 * (ip:port), or the one in the configuration if none is given.
 *
 * Each session needs an id from vigil_voice_run_id(), vigil_voice_stop()
 * stops the session with that id and all sessions before it.
 *
 * This blocks until the call ended and returns why (VIGIL_VOICE_END_*).
 */
int
vigil_voice_run(u_int64_t id, const char *cfg, size_t cfglen,
    u_int8_t peer, int mode, const char *cathedral)
{
	struct voice_session	session;
	struct vigil_config	*config;
	char			reason[128];

	PRECOND(id > 0);
	PRECOND(cfg != NULL);
	PRECOND(peer > 0 && peer < KYRKA_PEERS_PER_FLOCK);
	PRECOND(mode == VIGIL_VOICE_MODE_CALL ||
	    mode == VIGIL_VOICE_MODE_RING ||
	    mode == VIGIL_VOICE_MODE_ANSWER ||
	    mode == VIGIL_VOICE_MODE_DECLINE);

	if (!vigil_atomic_cas_simple(&active, 0, 1)) {
		vigil_log(LOG_NOTICE, "[voice] a call is already running");
		return (VIGIL_VOICE_END_ERROR);
	}

	vigil_atomic_write(&current, id);

	if (id <= vigil_atomic_read(&stopped)) {
		vigil_atomic_write(&active, 0);
		return (VIGIL_VOICE_END_LOCAL);
	}

	if (wake[0] == -1)
		voice_wake_init();

	voice_wake_drain();

	config = vigil_config_parse(cfg, cfglen, reason, sizeof(reason));
	if (config == NULL) {
		vigil_log(LOG_ERR, "[voice] invalid configuration: %s", reason);
		vigil_atomic_write(&active, 0);
		return (VIGIL_VOICE_END_ERROR);
	}

	vigil_kyrka_enter();

	if (voice_session_open(&session, config, peer, mode) != -1) {
		session.owned = 1;

		if (voice_session_resolve(&session, cathedral) == -1)
			session.end = VIGIL_VOICE_END_ERROR;
		else if (voice_session_setup(&session) == -1)
			session.end = VIGIL_VOICE_END_ERROR;
		else
			voice_session_loop(&session);
	} else {
		vigil_config_free(config);
	}

	voice_session_close(&session);
	vigil_kyrka_leave();

	vigil_atomic_write(&active, 0);

	return (vigil_atomic_read(&ended_why));
}

/*
 * Request a voice session with the given peer from the vigil core, any
 * session it is running is hung up first. May be called from any thread.
 */
void
vigil_voice_call(u_int8_t peer, int mode)
{
	PRECOND(peer > 0 && peer < KYRKA_PEERS_PER_FLOCK);
	PRECOND(mode == VIGIL_VOICE_MODE_CALL ||
	    mode == VIGIL_VOICE_MODE_RING ||
	    mode == VIGIL_VOICE_MODE_ANSWER ||
	    mode == VIGIL_VOICE_MODE_DECLINE);

	vigil_atomic_write(&request_peer, peer);
	vigil_atomic_write(&request_mode, mode);
	vigil_atomic_add(&request_seq, 1);
}

/*
 * Hang up the voice session the vigil core is running, if any.
 * May be called from any thread.
 */
void
vigil_voice_hangup(void)
{
	vigil_atomic_write(&request_hangup, vigil_atomic_read(&request_seq));
}

/*
 * Answer the ringing voice session, may be called from any thread.
 */
void
vigil_voice_answer(void)
{
	vigil_atomic_write(&answered, 1);
}

/*
 * Returns 1 if the vigil core is running a voice session.
 */
int
vigil_voice_busy(void)
{
	return (embedded_open);
}

/*
 * Called from the vigil core event loop, we pick up new requests and
 * run the session if we have one.
 */
void
vigil_voice_update(void)
{
	u_int64_t		seq;
	struct sockaddr_in	*sin;
	int			signal;

	PRECOND(vigil != NULL);
	PRECOND(vigil->gospel != NULL);

	seq = vigil_atomic_read(&request_seq);

	if (seq != request_handled) {
		request_handled = seq;

		if (embedded_open) {
			voice_hangup_send(&embedded);
			embedded.end = VIGIL_VOICE_END_LOCAL;
			vigil_voice_cleanup();
		}

		if (!vigil_atomic_cas_simple(&active, 0, 1)) {
			vigil_log(LOG_NOTICE, "[voice] can't start a call");
			vigil_atomic_write(&ended_why, VIGIL_VOICE_END_ERROR);
			vigil_atomic_add(&ended, 1);
			return;
		}

		if (voice_session_open(&embedded, vigil->gospel,
		    vigil_atomic_read(&request_peer),
		    vigil_atomic_read(&request_mode)) == -1) {
			voice_session_close(&embedded);
			vigil_atomic_write(&active, 0);
			return;
		}

		sin = vigil_call_cathedral();
		if (sin == NULL) {
			vigil_log(LOG_NOTICE, "[voice] no cathedral for calls");
			embedded.end = VIGIL_VOICE_END_ERROR;
		} else {
			memcpy(&embedded.cathedral, sin, sizeof(*sin));

			if (voice_session_setup(&embedded) == -1) {
				embedded.end = VIGIL_VOICE_END_ERROR;
			} else {
				embedded_io.fd = embedded.fd;
				embedded_io.handle = voice_io_event;
				vigil_io_schedule(&embedded_io);
			}
		}

		embedded_open = 1;
	}

	if (!embedded_open)
		return;

	embedded.now = vigil_time_ms();

	if (embedded.end == -1 &&
	    vigil_atomic_read(&request_hangup) == request_handled)
		voice_session_hangup(&embedded);

	if (vigil_call_peer(embedded.peer) & VIGIL_CALL_PEER_SIGNALING)
		signal = 1;
	else
		signal = 0;

	if (embedded.end == -1 && embedded.online == 0 && signal == 0 &&
	    (embedded.now - embedded.started) >= VOICE_SIGNAL_GRACE) {
		vigil_log(LOG_INFO, "[voice] %02x gave up", embedded.peer);
		embedded.end = VIGIL_VOICE_END_REMOTE;
	}

	if (embedded.end == -1) {
		voice_session_manage(&embedded);
		voice_audio_send(&embedded);
		voice_video_update(&embedded);
	}

	if (embedded.end != -1)
		vigil_voice_cleanup();
}

/*
 * Give the voice session of the vigil core a new socket, if it runs one.
 */
void
vigil_voice_rebind(void)
{
	if (!embedded_open)
		return;

	if (embedded.fd == -1)
		return;

	if (embedded.ctx == NULL)
		return;

	vigil_io_unschedule(&embedded_io);
	voice_session_rebind(&embedded);

	embedded_io.fd = embedded.fd;
	vigil_io_schedule(&embedded_io);
}

/*
 * Stop the voice session of the vigil core, if it is running one.
 */
void
vigil_voice_cleanup(void)
{
	if (!embedded_open)
		return;

	if (embedded.fd != -1)
		vigil_io_unschedule(&embedded_io);

	vigil_call_done(embedded.peer);
	voice_session_close(&embedded);

	embedded_open = 0;
	vigil_atomic_write(&active, 0);
}

/*
 * Returns a new id for vigil_voice_run(), higher than all those before it.
 * May be called from any thread.
 */
u_int64_t
vigil_voice_run_id(void)
{
	return (vigil_atomic_add(&last_id, 1) + 1);
}

/*
 * Request the session with the given id, and those before it, to stop.
 * May be called from any thread.
 */
void
vigil_voice_stop(u_int64_t id)
{
	int		fd;
	u_int8_t	byte;
	u_int64_t	last;

	PRECOND(id > 0);

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
 * Mute or unmute our side of the call, may be called from any thread.
 */
void
vigil_voice_mute(int on)
{
	PRECOND(on == 0 || on == 1);

	vigil_atomic_write(&muted, on);
}

/*
 * Copy the state of the voice session into buf as a json object.
 * May be called from any thread.
 *
 * Returns the length of the json in buf, or 0 if buf was too small.
 */
size_t
vigil_voice_status(char *buf, size_t len)
{
	u_int64_t		since;
	const char		*name;
	int			ret, st;
	char			video[256];
	struct in_addr		path, relay;
	char			via[INET_ADDRSTRLEN];
	char			cathedral[INET_ADDRSTRLEN];

	PRECOND(buf != NULL);
	PRECOND(len > 0);

	st = vigil_atomic_read(&state);

	switch (st) {
	case VIGIL_VOICE_STATE_CONNECTING:
		name = "connecting";
		break;
	case VIGIL_VOICE_STATE_ONLINE:
		name = "online";
		break;
	case VIGIL_VOICE_STATE_TALKING:
		name = "talking";
		break;
	default:
		ret = snprintf(buf, len,
		    "{\"running\":false,\"calls\":%" PRIu64
		    ",\"end\":\"%s\",\"rx_bytes\":%" PRIu64
		    ",\"tx_bytes\":%" PRIu64 "}", vigil_atomic_read(&ended),
		    voice_end_name(vigil_atomic_read(&ended_why)),
		    vigil_atomic_read(&rx_bytes), vigil_atomic_read(&tx_bytes));
		if (ret < 0 || (size_t)ret >= len)
			return (0);
		return ((size_t)ret);
	}

	since = vigil_atomic_read(&talking);
	if (since != 0)
		since = (vigil_time_ms() - since) / 1000;

	path.s_addr = vigil_atomic_read(&path_ip);
	relay.s_addr = vigil_atomic_read(&relay_ip);

	if (inet_ntop(AF_INET, &path, via, sizeof(via)) == NULL)
		return (0);

	if (inet_ntop(AF_INET, &relay, cathedral, sizeof(cathedral)) == NULL)
		return (0);

	if (vigil_video_status(video, sizeof(video)) == 0)
		return (0);

	ret = snprintf(buf, len,
	    "{\"running\":true,\"calls\":%" PRIu64 ",\"peer\":\"%02x\","
	    "\"state\":\"%s\",\"muted\":%s,\"uptime\":%" PRIu64
	    ",\"rx\":%" PRIu64 ",\"tx\":%" PRIu64 ",\"p2p\":%s,"
	    "\"path\":\"%s:%u\",\"cathedral\":\"%s:%u\",\"rx_bytes\":%" PRIu64
	    ",\"tx_bytes\":%" PRIu64 ",\"video\":%s}",
	    vigil_atomic_read(&ended), vigil_atomic_read(&peer_id), name,
	    vigil_atomic_read(&muted) ? "true" : "false", since,
	    vigil_atomic_read(&rx_frames), vigil_atomic_read(&tx_frames),
	    (path.s_addr == relay.s_addr && vigil_atomic_read(&path_port) ==
	    vigil_atomic_read(&relay_port)) ? "false" : "true",
	    via, ntohs(vigil_atomic_read(&path_port)),
	    cathedral, ntohs(vigil_atomic_read(&relay_port)),
	    vigil_atomic_read(&rx_bytes), vigil_atomic_read(&tx_bytes), video);

	if (ret < 0 || (size_t)ret >= len)
		return (0);

	return ((size_t)ret);
}

/*
 * Called by the platform audio with captured samples, we queue them up
 * in frames for the voice thread to encode and send.
 */
void
vigil_voice_capture(const int16_t *pcm, size_t samples)
{
	size_t		count;

	PRECOND(pcm != NULL);
	PRECOND(capture_frame == NULL ||
	    capture_offset < VIGIL_VOICE_SAMPLE_COUNT);

	while (samples > 0) {
		if (capture_frame == NULL) {
			capture_offset = 0;
			capture_frame = voice_ring_produce(&capture);
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
			voice_ring_produced(&capture);
		}
	}
}

/*
 * Called by the platform audio when it wants samples to play, we hand
 * it the frames the voice thread decoded or silence if we have none.
 */
void
vigil_voice_playback(int16_t *pcm, size_t samples)
{
	size_t		count, pending;

	PRECOND(pcm != NULL);
	PRECOND(playback_frame == NULL ||
	    playback_offset < VIGIL_VOICE_SAMPLE_COUNT);

	if (vigil_atomic_read(&state) != VIGIL_VOICE_STATE_TALKING &&
	    vigil_atomic_read(&tone) != VOICE_TONE_NONE) {
		voice_tone_fill(pcm, samples);
		return;
	}

	while (samples > 0) {
		if (playback_frame == NULL) {
			pending = voice_ring_pending(&playback);
			if (!playback_primed && pending < VOICE_PLAYBACK_PRIME)
				break;

			playback_offset = 0;
			playback_frame = voice_ring_consume(&playback);
			if (playback_frame == NULL) {
				playback_primed = 0;
				break;
			}

			playback_primed = 1;
		}

		count = VIGIL_VOICE_SAMPLE_COUNT - playback_offset;
		if (samples < count)
			count = samples;

		memcpy(pcm, &playback_frame[playback_offset],
		    count * sizeof(*pcm));

		pcm += count;
		samples -= count;
		playback_offset += count;

		if (playback_offset == VIGIL_VOICE_SAMPLE_COUNT) {
			playback_frame = NULL;
			voice_ring_consumed(&playback);
		}
	}

	if (samples > 0)
		memset(pcm, 0, samples * sizeof(*pcm));
}

/*
 * Prepare a new session with the given peer using the given configuration,
 * it is set up via voice_session_setup() once we know the cathedral.
 */
static int
voice_session_open(struct voice_session *s, struct vigil_config *config,
    u_int8_t peer, int mode)
{
	PRECOND(s != NULL);
	PRECOND(config != NULL);
	PRECOND(mode == VIGIL_VOICE_MODE_CALL ||
	    mode == VIGIL_VOICE_MODE_RING ||
	    mode == VIGIL_VOICE_MODE_ANSWER ||
	    mode == VIGIL_VOICE_MODE_DECLINE);

	memset(s, 0, sizeof(*s));

	s->fd = -1;
	s->end = -1;
	s->peer = peer;
	s->mode = mode;
	s->config = config;

	s->now = vigil_time_ms();
	s->started = s->now;
	s->declined = s->now;

	voice_rings_reset();
	vigil_video_open();

	vigil_atomic_write(&muted, 0);
	vigil_atomic_write(&answered, 0);
	vigil_atomic_write(&talking, 0);
	vigil_atomic_write(&rx_frames, 0);
	vigil_atomic_write(&tx_frames, 0);
	vigil_atomic_write(&peer_id, peer);
	vigil_atomic_write(&state, VIGIL_VOICE_STATE_CONNECTING);

	switch (mode) {
	case VIGIL_VOICE_MODE_CALL:
		vigil_atomic_write(&tone, VOICE_TONE_RINGBACK);
		break;
	case VIGIL_VOICE_MODE_ANSWER:
		vigil_atomic_write(&tone, VOICE_TONE_PIPS);
		break;
	default:
		vigil_atomic_write(&tone, VOICE_TONE_NONE);
		break;
	}

	if (config->mode != VIGIL_MODE_GOSPEL) {
		vigil_log(LOG_ERR, "[voice] calls need a gospel configuration");
		s->end = VIGIL_VOICE_END_ERROR;
		return (-1);
	}

	if (peer == 0 || peer == (config->spi & 0xff) ||
	    peer >= KYRKA_PEERS_PER_FLOCK) {
		vigil_log(LOG_ERR, "[voice] can't call %02x", peer);
		s->end = VIGIL_VOICE_END_ERROR;
		return (-1);
	}

	return (0);
}

/*
 * Set up the tunnel and codec for the call.
 */
static int
voice_session_setup(struct voice_session *s)
{
	int				err;
	struct kyrka_cathedral_cfg	kcfg;
	u_int8_t			local;

	PRECOND(s != NULL);
	PRECOND(s->fd == -1);
	PRECOND(s->ctx == NULL);
	PRECOND(s->config != NULL);

	local = s->config->spi & 0xff;

	s->fd = vigil_socket_udp();
	if (s->fd == -1)
		return (-1);

	voice_socket_buffers(s->fd);

	s->ctx = kyrka_ctx_alloc(voice_kyrka_event, s);
	if (s->ctx == NULL)
		fatal("failed to create voice context");

	memset(&kcfg, 0, sizeof(kcfg));

	kcfg.udata = s;
	kcfg.send = voice_cathedral_send;
	kcfg.tunnel = (local << 8) | s->peer;
	kcfg.identity = s->config->cathedral_id;
	kcfg.flock_src = s->config->cathedral_flock | VIGIL_CALL_FLOCK_DOMAIN;
	kcfg.flock_dst = kcfg.flock_src;

	if (s->config->flags & VIGIL_CONFIG_FLAG_COMMIXTION)
		kcfg.hops = VIGIL_COMMIXTION_HOPS;

	if (s->config->flags & VIGIL_CONFIG_FLAG_SHROUD) {
		if (kyrka_shroud_enable(s->ctx) == -1) {
			fatal("failed to enable shroud: %d",
			    kyrka_last_error(s->ctx));
		}
	}

	if (kyrka_cathedral_config(s->ctx, &kcfg) == -1) {
		fatal("failed to configure cathedral: %d",
		    kyrka_last_error(s->ctx));
	}

	if (kyrka_device_kek_load(s->ctx, s->config->kek,
	    sizeof(s->config->kek)) == -1) {
		fatal("kyrka_device_kek_load: %d", kyrka_last_error(s->ctx));
	}

	if (kyrka_cathedral_secret_load(s->ctx, s->config->cathedral_secret,
	    sizeof(s->config->cathedral_secret)) == -1) {
		fatal("kyrka_cathedral_secret_load: %d",
		    kyrka_last_error(s->ctx));
	}

	if (kyrka_cathedral_cosk_load(s->ctx, s->config->cathedral_cosk,
	    sizeof(s->config->cathedral_cosk)) == -1) {
		fatal("kyrka_cathedral_cosk_load: %d",
		    kyrka_last_error(s->ctx));
	}

	if (s->config->mtu < VOICE_MTU_MAX)
		s->mtu = s->config->mtu;
	else
		s->mtu = VOICE_MTU_MAX;

	if (kyrka_mtu_size(s->ctx, s->mtu) == -1) {
		fatal("failed to set libkyrka mtu size: %d",
		    kyrka_last_error(s->ctx));
	}

	if (kyrka_heaven_ifc(s->ctx, voice_heaven_input, s) == -1)
		fatal("failed to set heaven interface");

	if (kyrka_purgatory_ifc(s->ctx, voice_purgatory_input, s) == -1)
		fatal("failed to set purgatory interface");

	if (s->mode == VIGIL_VOICE_MODE_CALL ||
	    s->mode == VIGIL_VOICE_MODE_ANSWER) {
		if (vigil_platform_audio_start() == -1) {
			vigil_log(LOG_ERR, "[voice] no audio");
			return (-1);
		}

		s->audio = 1;
	}

	s->decoder = opus_decoder_create(VIGIL_VOICE_SAMPLE_RATE, 1, &err);
	if (err != OPUS_OK)
		fatal("failed to create opus decoder: %d", err);

	s->encoder = opus_encoder_create(VIGIL_VOICE_SAMPLE_RATE, 1,
	    OPUS_APPLICATION_VOIP, &err);
	if (err != OPUS_OK)
		fatal("failed to create opus encoder: %d", err);

	err = opus_encoder_ctl(s->encoder, OPUS_SET_BITRATE(OPUS_AUTO));
	if (err != OPUS_OK)
		fatal("failed to set bitrate: %d", err);

	err = opus_encoder_ctl(s->encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
	if (err != OPUS_OK)
		fatal("failed to set signal type: %d", err);

	err = opus_encoder_ctl(s->encoder, OPUS_SET_INBAND_FEC(1));
	if (err != OPUS_OK)
		fatal("failed to enable FEC: %d", err);

	err = opus_encoder_ctl(s->encoder, OPUS_SET_PACKET_LOSS_PERC(5));
	if (err != OPUS_OK)
		fatal("failed to set expected packet loss: %d", err);

	memcpy(&s->addr, &s->cathedral, sizeof(s->cathedral));
	voice_path_publish(s);

	vigil_log(LOG_INFO, "[voice] %s %02x via %s",
	    s->mode == VIGIL_VOICE_MODE_RING ? "ringing for" :
	    s->mode == VIGIL_VOICE_MODE_DECLINE ? "declining" : "calling",
	    s->peer, vigil_address(&s->cathedral));

	return (0);
}

/*
 * Our ringing session was answered, start the audio and talk to our peer.
 */
static void
voice_session_answer(struct voice_session *s)
{
	PRECOND(s != NULL);
	PRECOND(s->audio == 0);
	PRECOND(s->mode == VIGIL_VOICE_MODE_RING);

	vigil_log(LOG_INFO, "[voice] answered %02x", s->peer);

	s->rang = 1;
	s->mode = VIGIL_VOICE_MODE_ANSWER;
	vigil_atomic_write(&tone, VOICE_TONE_PIPS);

	if (vigil_platform_audio_start() == -1) {
		vigil_log(LOG_ERR, "[voice] no audio");
		s->end = VIGIL_VOICE_END_ERROR;
		return;
	}

	s->audio = 1;
}

/*
 * We hang up, a ringing session declines the call instead.
 */
static void
voice_session_hangup(struct voice_session *s)
{
	PRECOND(s != NULL);

	switch (s->mode) {
	case VIGIL_VOICE_MODE_DECLINE:
		break;
	case VIGIL_VOICE_MODE_RING:
		s->declined = s->now;
		s->mode = VIGIL_VOICE_MODE_DECLINE;
		break;
	default:
		/* Linger like a decline so our hangup gets there. */
		voice_hangup_send(s);
		s->declined = s->now;
		s->next_hangup = s->now + VOICE_DECLINE_INTERVAL;
		s->mode = VIGIL_VOICE_MODE_DECLINE;
		break;
	}
}

/*
 * Ask for larger socket buffers, the system may give us less.
 */
static void
voice_socket_buffers(int fd)
{
	int		size;

	PRECOND(fd != -1);

	size = VOICE_SOCKET_BUFFER;

	if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) == -1)
		vigil_log(LOG_NOTICE, "[voice] SO_SNDBUF: %s", errno_s);

	if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)) == -1)
		vigil_log(LOG_NOTICE, "[voice] SO_RCVBUF: %s", errno_s);
}

/*
 * Resolve the given cathedral, or the one in our Gospel configuration,
 * we keep trying for a bit as we may just have woken up.
 */
static int
voice_session_resolve(struct voice_session *s, const char *cathedral)
{
	PRECOND(s != NULL);
	PRECOND(s->config != NULL);

	if (cathedral != NULL) {
		if (voice_cathedral_resolve(s, cathedral) != -1)
			return (0);

		vigil_log(LOG_NOTICE, "[voice] bad cathedral %s", cathedral);
	}

	while (vigil_resolve(s->config->cathedral_host,
	    s->config->cathedral_port, &s->cathedral) == -1) {
		if (voice_stop_check())
			return (-1);

		if ((vigil_time_ms() - s->started) > VOICE_RESOLVE_TIMEOUT)
			return (-1);

		vigil_kyrka_leave();
		(void)poll(NULL, 0, VOICE_MANAGE_INTERVAL);
		vigil_kyrka_enter();
	}

	return (0);
}

/*
 * Resolve the given cathedral (ip:port), returns -1 if it is invalid.
 */
static int
voice_cathedral_resolve(struct voice_session *s, const char *cathedral)
{
	unsigned long	port;
	char		*p, *ep;
	char		host[VIGIL_HOST_MAX];

	PRECOND(s != NULL);
	PRECOND(cathedral != NULL);

	(void)snprintf(host, sizeof(host), "%s", cathedral);

	p = strrchr(host, ':');
	if (p == NULL)
		return (-1);

	*(p)++ = '\0';

	errno = 0;
	port = strtoul(p, &ep, 10);

	if (errno != 0)
		return (-1);

	if (*ep != '\0')
		return (-1);

	if (port == 0 || port > USHRT_MAX)
		return (-1);

	return (vigil_resolve(host, port, &s->cathedral));
}

/*
 * Run the session until it ended or we are asked to stop.
 */
static void
voice_session_loop(struct voice_session *s)
{
	int			ret;
	struct pollfd		pfd[2];
	u_int64_t		seen, next;

	PRECOND(s != NULL);
	PRECOND(s->fd != -1);
	PRECOND(wake[0] != -1);

	next = 0;
	seen = vigil_rebind_generation();

	pfd[0].fd = s->fd;
	pfd[0].events = POLLIN;

	pfd[1].fd = wake[0];
	pfd[1].events = POLLIN;

	while (s->end == -1) {
		vigil_kyrka_leave();
		ret = poll(pfd, 2, VOICE_IO_WAIT);
		vigil_kyrka_enter();

		if (ret == -1 && errno != EINTR)
			fatal("poll: %s", errno_s);

		s->now = vigil_time_ms();

		if (pfd[1].revents & POLLIN)
			voice_wake_drain();

		if (voice_stop_check()) {
			voice_session_hangup(s);
			if (s->end != -1)
				break;
		}

		if (pfd[0].revents & (POLLIN | POLLERR | POLLHUP))
			voice_io_read(s);

		if (vigil_rebind_due(&seen, &next, s->now)) {
			voice_session_rebind(s);
			pfd[0].fd = s->fd;
		}

		voice_session_manage(s);
		voice_audio_send(s);
		voice_video_update(s);
	}
}

/*
 * Give the session a new socket and go back to the cathedral, the
 * network we were on changed so a p2p path we had is gone.
 */
static void
voice_session_rebind(struct voice_session *s)
{
	int		fd;

	PRECOND(s != NULL);
	PRECOND(s->ctx != NULL);

	fd = vigil_socket_udp();
	if (fd == -1)
		return;

	voice_socket_buffers(fd);
	vigil_log(LOG_NOTICE, "[voice] network changed, new socket");

	vigil_socket_close(s->fd);
	s->fd = fd;

	s->p2p_retry = 0;
	s->p2p_verify = 0;
	s->p2p_backoff = 0;
	s->next_notify = 0;

	if (!vigil_inet_match(&s->addr, &s->cathedral)) {
		memcpy(&s->addr, &s->cathedral, sizeof(s->cathedral));
		(void)kyrka_p2p_active(s->ctx, 0);
		voice_path_publish(s);
	}
}

/*
 * End the session, releasing all it holds.
 */
static void
voice_session_close(struct voice_session *s)
{
	PRECOND(s != NULL);

	if (s->end == -1)
		s->end = VIGIL_VOICE_END_LOCAL;

	vigil_log(LOG_INFO, "[voice] call with %02x ended (%s)",
	    s->peer, voice_end_name(s->end));

	vigil_atomic_write(&ended_why, s->end);
	vigil_atomic_write(&state, VIGIL_VOICE_STATE_IDLE);
	vigil_atomic_add(&ended, 1);

	voice_session_cleanup(s);
}

/*
 * Returns a name for the given VIGIL_VOICE_END_* reason.
 */
static const char *
voice_end_name(int why)
{
	switch (why) {
	case VIGIL_VOICE_END_LOCAL:
		return ("local");
	case VIGIL_VOICE_END_REMOTE:
		return ("remote");
	case VIGIL_VOICE_END_TIMEOUT:
		return ("timeout");
	default:
		return ("error");
	}
}

/*
 * Callback for when an event occurred on the socket of the session
 * run by the vigil core.
 */
static void
voice_io_event(void *udata)
{
	PRECOND(udata == &embedded_io);

	if (embedded_io.flags & VIGIL_IO_READABLE) {
		embedded_io.flags &= ~VIGIL_IO_READABLE;
		voice_io_read(&embedded);
	}
}

/*
 * Release everything the session holds.
 */
static void
voice_session_cleanup(struct voice_session *s)
{
	PRECOND(s != NULL);

	if (s->audio) {
		vigil_platform_audio_stop();
		s->audio = 0;
	}

	vigil_video_close();

	if (s->encoder != NULL)
		opus_encoder_destroy(s->encoder);

	if (s->decoder != NULL)
		opus_decoder_destroy(s->decoder);

	if (s->ctx != NULL)
		kyrka_ctx_free(s->ctx);

	if (s->fd != -1)
		vigil_socket_close(s->fd);

	if (s->owned && s->config != NULL)
		vigil_config_free(s->config);

	nyfe_mem_zero(s, sizeof(*s));
}

/*
 * Manage keys, keep the cathedral informed and check if the call
 * should be over by now.
 */
static void
voice_session_manage(struct voice_session *s)
{
	u_int64_t	since;

	PRECOND(s != NULL);
	PRECOND(s->ctx != NULL);

	if (s->mode == VIGIL_VOICE_MODE_RING && vigil_atomic_read(&answered))
		voice_session_answer(s);

	if (s->mode == VIGIL_VOICE_MODE_DECLINE) {
		if (s->online != 0 && s->now >= s->next_hangup) {
			s->next_hangup = s->now + VOICE_DECLINE_INTERVAL;
			voice_hangup_send(s);
		}

		if (s->online > s->declined)
			since = s->online;
		else
			since = s->declined;

		if (s->online != 0 &&
		    (s->now - since) >= VOICE_DECLINE_LINGER) {
			s->end = VIGIL_VOICE_END_LOCAL;
			return;
		}

		if (s->online == 0 &&
		    (s->now - s->declined) >= VOICE_DECLINE_WAIT) {
			s->end = VIGIL_VOICE_END_LOCAL;
			return;
		}
	}

	if ((s->mode == VIGIL_VOICE_MODE_RING || s->rang) && s->online == 0 &&
	    (s->now - s->started) >= VOICE_RING_VERIFY) {
		vigil_log(LOG_INFO, "[voice] %02x never showed up", s->peer);
		s->end = VIGIL_VOICE_END_TIMEOUT;
		return;
	}

	if (s->mode != VIGIL_VOICE_MODE_DECLINE &&
	    vigil_atomic_read(&state) != VIGIL_VOICE_STATE_TALKING &&
	    (s->now - s->started) >= VOICE_CONNECT_TIMEOUT) {
		vigil_log(LOG_INFO, "[voice] %02x did not answer", s->peer);
		s->end = VIGIL_VOICE_END_TIMEOUT;
		return;
	}

	if (vigil_atomic_read(&state) == VIGIL_VOICE_STATE_TALKING &&
	    (s->now - s->last_rx) >= VOICE_RX_TIMEOUT) {
		vigil_log(LOG_INFO, "[voice] %02x went quiet", s->peer);
		s->end = VIGIL_VOICE_END_TIMEOUT;
		return;
	}

	if (s->p2p_verify != 0 && s->now >= s->p2p_verify) {
		vigil_log(LOG_INFO, "[voice] %02x p2p via %s did not work",
		    s->peer, vigil_address(&s->addr));
		voice_p2p_fail(s);
	}

	if (s->now >= s->next_notify) {
		s->next_notify = s->now + VOICE_NOTIFY_INTERVAL;

		if (kyrka_cathedral_notify(s->ctx) == -1) {
			vigil_log(LOG_NOTICE, "[voice] cathedral notify: %d",
			    kyrka_last_error(s->ctx));
		}

		if (kyrka_cathedral_nat_detection(s->ctx) == -1) {
			vigil_log(LOG_NOTICE, "[voice] nat detection: %d",
			    kyrka_last_error(s->ctx));
		}
	}

	if (s->now >= s->next_manage) {
		s->next_manage = s->now + VOICE_MANAGE_INTERVAL;

		if (kyrka_key_manage(s->ctx) == -1 &&
		    kyrka_last_error(s->ctx) != KYRKA_ERROR_NO_SECRET) {
			vigil_log(LOG_NOTICE, "[voice] key manage: %d",
			    kyrka_last_error(s->ctx));
		}
	}
}

/*
 * Read packets from our socket and push them into libkyrka.
 */
static void
voice_io_read(struct voice_session *s)
{
	int			idx;
	size_t			len;
	ssize_t			ret;
	struct kyrka_packet	pkt;
	struct sockaddr_in	from;
	u_int8_t		*ptr;
	socklen_t		fromlen;

	PRECOND(s != NULL);
	PRECOND(s->ctx != NULL);

	for (idx = 0; idx < VIGIL_IO_BATCH; idx++) {
		ptr = kyrka_packet_recvbuf(s->ctx, &pkt, &len);
		if (ptr == NULL)
			fatal("failed to get voice receive buffer");

		fromlen = sizeof(from);

		ret = recvfrom(s->fd, ptr, len, 0,
		    (struct sockaddr *)&from, &fromlen);
		if (ret == -1) {
			if (errno == EINTR)
				continue;
			if (errno != EWOULDBLOCK && errno != EAGAIN)
				vigil_log(LOG_NOTICE, "[voice] read: %s",
				    errno_s);
			break;
		}

		if (ret == 0)
			continue;

		vigil_atomic_add(&rx_bytes, ret);

		pkt.length = ret;
		memcpy(&s->from, &from, sizeof(from));

		if (vigil_inet_match(&from, &s->cathedral))
			pkt.shroud = KYRKA_PACKET_SHROUD_CATHEDRAL;
		else
			pkt.shroud = KYRKA_PACKET_SHROUD_PEER;

		if (kyrka_purgatory_input(s->ctx, &pkt) == -1 &&
		    kyrka_last_error(s->ctx) != KYRKA_ERROR_NO_RX_KEY) {
			vigil_log(LOG_NOTICE, "[voice] purgatory input: %d",
			    kyrka_last_error(s->ctx));
		}
	}
}

/*
 * Encode and send all frames the platform audio captured.
 */
static void
voice_audio_send(struct voice_session *s)
{
	int		len;
	int16_t		*pcm;
	u_int8_t	opus[VIGIL_VOICE_OPUS_MAX];

	PRECOND(s != NULL);
	PRECOND(s->encoder != NULL);

	while ((pcm = voice_ring_consume(&capture)) != NULL) {
		if (vigil_atomic_read(&muted))
			memset(pcm, 0, VIGIL_VOICE_SAMPLE_COUNT * sizeof(*pcm));

		if (s->online == 0 || s->mode == VIGIL_VOICE_MODE_DECLINE) {
			voice_ring_consumed(&capture);
			continue;
		}

		if (s->mode != VIGIL_VOICE_MODE_ANSWER &&
		    vigil_atomic_read(&state) != VIGIL_VOICE_STATE_TALKING) {
			voice_ring_consumed(&capture);
			continue;
		}

		len = opus_encode(s->encoder, pcm, VIGIL_VOICE_SAMPLE_COUNT,
		    opus, sizeof(opus));

		voice_ring_consumed(&capture);

		if (len < 0) {
			vigil_log(LOG_NOTICE, "[voice] opus_encode: %d", len);
			continue;
		}

		voice_frame_send(s, VIGIL_VOICE_TYPE_VOICE, opus, len);
		vigil_atomic_add(&tx_frames, 1);
	}
}

/*
 * Let the video send what it can and tell our peer how it is doing.
 */
static void
voice_video_update(struct voice_session *s)
{
	struct vigil_video_link		link;

	PRECOND(s != NULL);

	if (s->ctx == NULL)
		return;

	voice_video_link(s, &link);
	vigil_video_update(&link);
}

/*
 * Fill in what the video needs to know about the given session.
 */
static void
voice_video_link(struct voice_session *s, struct vigil_video_link *link)
{
	PRECOND(s != NULL);
	PRECOND(s->ctx != NULL);
	PRECOND(link != NULL);

	memset(link, 0, sizeof(*link));

	link->ctx = s->ctx;
	link->now = s->now;
	link->mtu = s->mtu;

	if (s->online != 0)
		link->online = 1;

	if (s->mode == VIGIL_VOICE_MODE_CALL ||
	    s->mode == VIGIL_VOICE_MODE_ANSWER)
		link->live = 1;

	if (vigil_atomic_read(&state) == VIGIL_VOICE_STATE_TALKING)
		link->talking = 1;

	if (vigil_inet_match(&s->addr, &s->cathedral))
		link->shroud = KYRKA_PACKET_SHROUD_CATHEDRAL;
	else
		link->shroud = KYRKA_PACKET_SHROUD_PEER;
}

/*
 * Tell our peer we hung up, if we have a tunnel with it.
 */
static void
voice_hangup_send(struct voice_session *s)
{
	int		idx;

	PRECOND(s != NULL);

	if (s->online == 0)
		return;

	for (idx = 0; idx < VOICE_HANGUP_COUNT; idx++)
		voice_frame_send(s, VIGIL_VOICE_TYPE_HANGUP, NULL, 0);
}

/*
 * Send a single frame of the given type with the given opus data
 * to our peer, padded to VIGIL_VOICE_PAYLOAD.
 */
static void
voice_frame_send(struct voice_session *s, u_int8_t type,
    const u_int8_t *data, size_t len)
{
	struct kyrka_packet	pkt;
	size_t			max;
	struct vigil_voice_hdr	*hdr;
	u_int8_t		*ptr;

	PRECOND(s != NULL);
	PRECOND(s->ctx != NULL);
	PRECOND(data != NULL || len == 0);
	PRECOND(len <= VIGIL_VOICE_OPUS_MAX);
	PRECOND(type == VIGIL_VOICE_TYPE_VOICE ||
	    type == VIGIL_VOICE_TYPE_HANGUP);

	if (s->online == 0)
		return;

	ptr = kyrka_packet_databuf(s->ctx, &pkt, &max);
	if (ptr == NULL)
		fatal("failed to get voice data buffer");

	if (max < VIGIL_VOICE_PAYLOAD)
		fatal("voice payload does not fit");

	memset(ptr, 0, VIGIL_VOICE_PAYLOAD);

	hdr = (struct vigil_voice_hdr *)ptr;
	hdr->type = type;
	hdr->length = htons(len);

	if (type == VIGIL_VOICE_TYPE_VOICE) {
		s->tx_seq++;
		if (s->tx_seq == 0)
			s->tx_seq = 1;
		hdr->seq = htons(s->tx_seq);
	}

	if (data != NULL)
		memcpy(ptr + sizeof(*hdr), data, len);

	pkt.length = VIGIL_VOICE_PAYLOAD;

	if (vigil_inet_match(&s->addr, &s->cathedral))
		pkt.shroud = KYRKA_PACKET_SHROUD_CATHEDRAL;
	else
		pkt.shroud = KYRKA_PACKET_SHROUD_PEER;

	if (kyrka_heaven_input(s->ctx, &pkt) == -1 &&
	    kyrka_last_error(s->ctx) != KYRKA_ERROR_NO_TX_KEY) {
		vigil_log(LOG_NOTICE, "[voice] kyrka_heaven_input: %d",
		    kyrka_last_error(s->ctx));
	}
}

/*
 * Callback from libkyrka when an event occurred on our voice context.
 */
static void
voice_kyrka_event(KYRKA *ctx, union kyrka_event *evt, void *udata)
{
	struct voice_session	*s;
	struct sockaddr_in	sin;

	PRECOND(ctx != NULL);
	PRECOND(evt != NULL);
	PRECOND(udata != NULL);

	s = udata;

	switch (evt->type) {
	case KYRKA_EVENT_KEYS_INFO:
		vigil_log(LOG_INFO, "[voice] %02x online tx=%08x rx=%08x",
		    s->peer, evt->keys.tx_spi, evt->keys.rx_spi);

		s->last_rx = s->now;
		s->next_notify = 0;

		if (evt->keys.tx_spi == 0)
			break;

		if (s->online == 0)
			s->online = s->now;

		if (vigil_atomic_read(&state) == VIGIL_VOICE_STATE_CONNECTING)
			vigil_atomic_write(&state, VIGIL_VOICE_STATE_ONLINE);
		break;
	case KYRKA_EVENT_KEYS_ERASED:
		vigil_log(LOG_INFO, "[voice] %02x keys erased", s->peer);
		s->p2p_verify = 0;
		memcpy(&s->addr, &s->cathedral, sizeof(s->cathedral));
		(void)kyrka_p2p_active(s->ctx, 0);
		voice_path_publish(s);
		break;
	case KYRKA_EVENT_PEER_DISCOVERY:
		if (!(s->config->flags & VIGIL_CONFIG_FLAG_P2P))
			break;

		memset(&sin, 0, sizeof(sin));
		sin.sin_family = AF_INET;
		sin.sin_port = evt->peer.port;
		sin.sin_addr.s_addr = evt->peer.ip;

		/* No hairpinning and we will never reach a peer on loopback. */
		if ((evt->peer.flags & KYRKA_INFO_FLAG_SAME_EXTERNAL_IPV4) ||
		    (ntohl(sin.sin_addr.s_addr) >> 24) == 127) {
			memset(&s->p2p_peer, 0, sizeof(s->p2p_peer));
			memcpy(&sin, &s->cathedral, sizeof(sin));
		} else {
			memcpy(&s->p2p_peer, &sin, sizeof(sin));
		}

		if (s->now < s->p2p_retry &&
		    vigil_inet_match(&sin, &s->p2p_failed))
			memcpy(&sin, &s->cathedral, sizeof(sin));

		if (vigil_inet_match(&sin, &s->addr))
			break;

		memcpy(&s->addr, &sin, sizeof(sin));
		voice_path_publish(s);

		if (vigil_inet_match(&sin, &s->cathedral)) {
			s->p2p_verify = 0;
			(void)kyrka_p2p_active(s->ctx, 0);
		} else {
			vigil_log(LOG_INFO, "[voice] %02x p2p via %s",
			    s->peer, vigil_address(&sin));
			s->p2p_rx = s->now;
			s->p2p_verify = s->now + VOICE_P2P_VERIFY;
			(void)kyrka_p2p_active(s->ctx, 1);
		}
		break;
	case KYRKA_EVENT_EXCHANGE_INFO:
		vigil_log(LOG_INFO, "[voice] exchange: %s",
		    evt->exchange.reason);
		break;
	case KYRKA_EVENT_LOGMSG:
		vigil_log(LOG_INFO, "[voice] libkyrka: %s", evt->logmsg.log);
		break;
	default:
		break;
	}
}

/*
 * Callback from libkyrka with a decrypted frame from our peer, we
 * decode the voice in it and queue it up for playback.
 */
static void
voice_heaven_input(struct kyrka_packet *pkt, u_int64_t seq, void *udata)
{
	struct voice_session		*s;
	struct vigil_video_ctl		ctl;
	u_int16_t			len, gap, frame;
	int16_t				*pcm;
	struct vigil_video_nack		nack;
	struct vigil_video_link		link;
	const struct vigil_voice_hdr	*hdr;
	int				count;
	const u_int8_t			*data;

	PRECOND(pkt != NULL);
	PRECOND(udata != NULL);

	s = udata;
	PRECOND(s->decoder != NULL);

	s->last_rx = s->now;

	if (vigil_inet_match(&s->from, &s->cathedral))
		voice_p2p_abandon(s);
	else
		voice_p2p_follow(s);

	if (s->end != -1)
		return;

	if (pkt->type != KYRKA_PACKET_TYPE_IP)
		return;

	data = kyrka_packet_data(pkt);

	if (pkt->length > VIGIL_VOICE_PAYLOAD &&
	    data[0] == VIGIL_VOICE_TYPE_VIDEO) {
		if (s->online != 0 && (s->mode == VIGIL_VOICE_MODE_CALL ||
		    s->mode == VIGIL_VOICE_MODE_ANSWER)) {
			voice_video_link(s, &link);
			vigil_video_recv(&link, data, pkt->length);
		}
		return;
	}

	if (pkt->length != VIGIL_VOICE_PAYLOAD)
		return;

	hdr = (const struct vigil_voice_hdr *)data;

	if (hdr->type == VIGIL_VOICE_TYPE_HANGUP) {
		vigil_log(LOG_INFO, "[voice] %02x hung up", s->peer);
		s->end = VIGIL_VOICE_END_REMOTE;
		return;
	}

	if (hdr->type == VIGIL_VOICE_TYPE_VIDEO_CTL) {
		if (ntohs(hdr->length) != sizeof(ctl))
			return;

		memcpy(&ctl, data + sizeof(*hdr), sizeof(ctl));
		voice_video_link(s, &link);
		vigil_video_ctl(&link, &ctl);
		return;
	}

	if (hdr->type == VIGIL_VOICE_TYPE_VIDEO_NACK) {
		if (ntohs(hdr->length) != sizeof(nack))
			return;

		memcpy(&nack, data + sizeof(*hdr), sizeof(nack));
		voice_video_link(s, &link);
		vigil_video_nack(&link, &nack);
		return;
	}

	if (hdr->type != VIGIL_VOICE_TYPE_VOICE)
		return;

	if (s->mode != VIGIL_VOICE_MODE_CALL &&
	    s->mode != VIGIL_VOICE_MODE_ANSWER)
		return;

	len = ntohs(hdr->length);
	if (len > VIGIL_VOICE_OPUS_MAX)
		return;

	/* Only once we can talk back, so both sides go live together. */
	if (s->online == 0)
		return;

	vigil_atomic_add(&rx_frames, 1);

	if (vigil_atomic_read(&state) != VIGIL_VOICE_STATE_TALKING) {
		vigil_atomic_write(&talking, s->now);
		vigil_atomic_write(&state, VIGIL_VOICE_STATE_TALKING);
	}

	gap = 0;
	frame = ntohs(hdr->seq);

	if (frame != 0) {
		if (s->rx_seq != 0) {
			gap = (u_int16_t)(frame - s->rx_seq);

			/* Overtaken by those after it, too late to play. */
			if (gap == 0 || gap >= 0x8000)
				return;
		}

		s->rx_seq = frame;
	}

	/* More than one went missing, conceal the oldest of them. */
	if (gap > 2) {
		pcm = voice_ring_produce(&playback);
		if (pcm != NULL) {
			if (opus_decode(s->decoder, NULL, 0, pcm,
			    VIGIL_VOICE_SAMPLE_COUNT, 0) > 0)
				voice_ring_produced(&playback);
		}
	}

	/* The one right before this frame we get back from its FEC. */
	if (gap > 1) {
		pcm = voice_ring_produce(&playback);
		if (pcm != NULL) {
			if (opus_decode(s->decoder, data + sizeof(*hdr), len,
			    pcm, VIGIL_VOICE_SAMPLE_COUNT, 1) > 0)
				voice_ring_produced(&playback);
		}
	}

	pcm = voice_ring_produce(&playback);
	if (pcm == NULL)
		return;

	count = opus_decode(s->decoder, data + sizeof(*hdr), len, pcm,
	    VIGIL_VOICE_SAMPLE_COUNT, 0);

	if (count != VIGIL_VOICE_SAMPLE_COUNT) {
		if (count < 0)
			vigil_log(LOG_NOTICE, "[voice] opus_decode: %d", count);
		return;
	}

	voice_ring_produced(&playback);
}

/*
 * Callback from libkyrka with ciphertext to send to our peer.
 */
static void
voice_purgatory_input(struct kyrka_packet *pkt, u_int64_t seq, void *udata)
{
	struct voice_session	*s;
	size_t			len;
	u_int8_t		*ptr;

	PRECOND(pkt != NULL);
	PRECOND(udata != NULL);

	s = udata;

	ptr = kyrka_packet_sendbuf(s->ctx, pkt, &len);
	if (ptr == NULL)
		fatal("failed to get voice send buffer");

	if (sendto(s->fd, ptr, len, 0,
	    (const struct sockaddr *)&s->addr, sizeof(s->addr)) == -1) {
		if (errno != EAGAIN && errno != EWOULDBLOCK &&
		    !vigil_socket_unreachable(errno,
		    vigil_inet_match(&s->addr, &s->cathedral)))
			vigil_log(LOG_NOTICE, "[voice] sendto: %s", errno_s);
		return;
	}

	vigil_atomic_add(&tx_bytes, len);
}

/*
 * Callback from libkyrka with data for the cathedral.
 */
static void
voice_cathedral_send(struct kyrka_packet *pkt, u_int64_t magic, void *udata)
{
	struct voice_session	*s;
	size_t			len;
	struct sockaddr_in	sin;
	u_int8_t		*ptr;

	PRECOND(pkt != NULL);
	PRECOND(udata != NULL);

	s = udata;

	memcpy(&sin, &s->cathedral, sizeof(sin));

	if (magic == KYRKA_CATHEDRAL_NAT_MAGIC)
		sin.sin_port = htons(s->config->cathedral_nat_port);

	ptr = kyrka_packet_sendbuf(s->ctx, pkt, &len);
	if (ptr == NULL)
		fatal("failed to get voice cathedral buffer");

	if (sendto(s->fd, ptr, len, 0,
	    (const struct sockaddr *)&sin, sizeof(sin)) == -1) {
		if (errno != EAGAIN && errno != EWOULDBLOCK &&
		    !vigil_socket_unreachable(errno, 1))
			vigil_log(LOG_NOTICE, "[voice] sendto: %s", errno_s);
		return;
	}

	vigil_atomic_add(&tx_bytes, len);
}

/*
 * Empty both rings, only safe while the platform audio is stopped.
 */
static void
voice_rings_reset(void)
{
	vigil_atomic_write(&capture.head, 0);
	vigil_atomic_write(&capture.tail, 0);
	vigil_atomic_write(&playback.head, 0);
	vigil_atomic_write(&playback.tail, 0);

	capture_frame = NULL;
	capture_offset = 0;

	tone_sample = 0;
	playback_primed = 0;
	playback_frame = NULL;
	playback_offset = 0;

	vigil_atomic_fence();
}

/*
 * We got a packet from our peer that did not come via the cathedral.
 *
 * If it came in on our p2p path that path works. If we are relaying and it
 * came from where the cathedral said our peer is, our peer already uses
 * that path and we follow it.
 */
static void
voice_p2p_follow(struct voice_session *s)
{
	PRECOND(s != NULL);
	PRECOND(s->ctx != NULL);

	if (vigil_inet_match(&s->from, &s->addr)) {
		if (s->p2p_verify != 0)
			s->p2p_backoff = 0;

		s->p2p_rx = s->now;
		s->p2p_retry = 0;
		s->p2p_verify = 0;
		return;
	}

	if (!(s->config->flags & VIGIL_CONFIG_FLAG_P2P))
		return;

	if (!vigil_inet_match(&s->addr, &s->cathedral))
		return;

	if (!vigil_inet_match(&s->from, &s->p2p_peer))
		return;

	vigil_log(LOG_INFO, "[voice] %02x p2p via %s, following our peer",
	    s->peer, vigil_address(&s->from));

	memcpy(&s->addr, &s->from, sizeof(s->from));
	voice_path_publish(s);

	s->p2p_rx = s->now;
	s->p2p_retry = 0;
	s->p2p_verify = s->now + VOICE_P2P_VERIFY;
	(void)kyrka_p2p_active(s->ctx, 1);
}

/*
 * Our peer reaches us via the cathedral. If we did not hear from it over
 * p2p for a while it can no longer be reached there, its network changed
 * or it never got our p2p path to work, so we go back to the cathedral.
 */
static void
voice_p2p_abandon(struct voice_session *s)
{
	PRECOND(s != NULL);
	PRECOND(s->ctx != NULL);

	if (vigil_inet_match(&s->addr, &s->cathedral))
		return;

	/* Our peer may not know about this path yet. */
	if (s->p2p_verify != 0)
		return;

	if ((s->now - s->p2p_rx) < VOICE_P2P_SILENT)
		return;

	vigil_log(LOG_INFO, "[voice] %02x p2p via %s went silent",
	    s->peer, vigil_address(&s->addr));

	voice_p2p_fail(s);
}

/*
 * The p2p path we are on does not work, go back to the cathedral and
 * do not try it again for a while.
 */
static void
voice_p2p_fail(struct voice_session *s)
{
	PRECOND(s != NULL);
	PRECOND(s->ctx != NULL);

	if (s->p2p_backoff == 0)
		s->p2p_backoff = VOICE_P2P_RETRY;
	else
		s->p2p_backoff *= 2;

	if (s->p2p_backoff > VOICE_P2P_RETRY_MAX)
		s->p2p_backoff = VOICE_P2P_RETRY_MAX;

	s->p2p_verify = 0;
	s->p2p_retry = s->now + s->p2p_backoff;

	memcpy(&s->p2p_failed, &s->addr, sizeof(s->addr));
	memcpy(&s->addr, &s->cathedral, sizeof(s->cathedral));
	voice_path_publish(s);

	(void)kyrka_p2p_active(s->ctx, 0);
}

/*
 * Publish where the session sends to and which cathedral it relays via.
 */
static void
voice_path_publish(struct voice_session *s)
{
	PRECOND(s != NULL);

	vigil_atomic_write(&path_ip, s->addr.sin_addr.s_addr);
	vigil_atomic_write(&path_port, s->addr.sin_port);
	vigil_atomic_write(&relay_ip, s->cathedral.sin_addr.s_addr);
	vigil_atomic_write(&relay_port, s->cathedral.sin_port);
}

/*
 * Fill pcm with the tone we play while we wait for our peer.
 */
static void
voice_tone_fill(int16_t *pcm, size_t samples)
{
	size_t		idx;
	u_int64_t	on, period, at;

	PRECOND(pcm != NULL);

	if (vigil_atomic_read(&tone) == VOICE_TONE_RINGBACK) {
		on = VOICE_RINGBACK_ON;
		period = VOICE_RINGBACK_PERIOD;
	} else {
		on = VOICE_PIPS_ON;
		period = VOICE_PIPS_PERIOD;
	}

	on = on * VIGIL_VOICE_SAMPLE_RATE / 1000;
	period = period * VIGIL_VOICE_SAMPLE_RATE / 1000;

	for (idx = 0; idx < samples; idx++) {
		at = tone_sample++ % period;

		if (at < on) {
			pcm[idx] = VOICE_TONE_LEVEL * sin(2 * M_PI *
			    VOICE_TONE_HZ * at / VIGIL_VOICE_SAMPLE_RATE);
		} else {
			pcm[idx] = 0;
		}
	}
}

/*
 * Create our wakeup pipe.
 */
static void
voice_wake_init(void)
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
voice_wake_drain(void)
{
	u_int8_t	buf[32];

	PRECOND(wake[0] != -1);

	while (read(wake[0], buf, sizeof(buf)) > 0)
		;
}

/*
 * Returns 1 if the running session was asked to stop.
 */
static int
voice_stop_check(void)
{
	return (vigil_atomic_read(&current) <= vigil_atomic_read(&stopped));
}

/*
 * Returns the number of frames waiting in the given ring.
 */
static size_t
voice_ring_pending(struct voice_ring *ring)
{
	PRECOND(ring != NULL);

	return (vigil_atomic_read(&ring->head) -
	    vigil_atomic_read(&ring->tail));
}

/*
 * Returns the next free frame in the ring, or NULL if it is full.
 * The frame is queued once voice_ring_produced() is called.
 */
static int16_t *
voice_ring_produce(struct voice_ring *ring)
{
	u_int32_t	head;

	PRECOND(ring != NULL);

	head = vigil_atomic_read(&ring->head);

	if (head - vigil_atomic_read(&ring->tail) == VOICE_RING_FRAMES)
		return (NULL);

	return (ring->frames[head & (VOICE_RING_FRAMES - 1)]);
}

/*
 * Queue the frame we got from voice_ring_produce().
 */
static void
voice_ring_produced(struct voice_ring *ring)
{
	PRECOND(ring != NULL);

	vigil_atomic_add(&ring->head, 1);
}

/*
 * Returns the oldest frame in the ring, or NULL if it is empty.
 * The frame is released once voice_ring_consumed() is called.
 */
static int16_t *
voice_ring_consume(struct voice_ring *ring)
{
	u_int32_t	tail;

	PRECOND(ring != NULL);

	tail = vigil_atomic_read(&ring->tail);

	if (tail == vigil_atomic_read(&ring->head))
		return (NULL);

	return (ring->frames[tail & (VOICE_RING_FRAMES - 1)]);
}

/*
 * Release the frame we got from voice_ring_consume().
 */
static void
voice_ring_consumed(struct voice_ring *ring)
{
	PRECOND(ring != NULL);

	vigil_atomic_add(&ring->tail, 1);
}
