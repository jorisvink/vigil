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

#include "vigil.h"

/* The frames we hold in each direction, must be a power of 2. */
#define VIDEO_FRAMES			128

/*
 * The most frames we keep around after sending them for resends, and
 * the milliseconds we keep them at least, longer on a slow path.
 */
#define VIDEO_HISTORY			96
#define VIDEO_HISTORY_TIME		500

/* The frames we put together at once, and the most we wait on unseen. */
#define VIDEO_ASSEMBLY			96
#define VIDEO_GAP_MAX			3

/* The frames we remember sending for round trips, must be a power of 2. */
#define VIDEO_SENT			128

/*
 * What we add to the round trip for the time it takes our peer to see a
 * fragment is missing and for the resend to make its way through.
 */
#define VIDEO_RTT_MARGIN		50

/*
 * A round trip this much longer than the shortest we saw, or a quarter
 * of it if that is more, means the queues on the path are filling up
 * and we should slow down before they drop.
 */
#define VIDEO_RTT_QUEUE			100

/*
 * From this round trip on a resend takes too long to wait for, we send
 * a parity fragment after every VIDEO_FEC_GROUP fragments so our peer can
 * rebuild one it lost on its own. The most a peer may group by.
 */
#define VIDEO_FEC_RTT			150
#define VIDEO_FEC_GROUP			4
#define VIDEO_FEC_GROUP_MAX		16

/* The most fragments a frame can have, enough for the smallest mtu. */
#define VIDEO_FRAGMENTS_MAX		2048

/* The fragments we queue up for a resend, must be a power of 2. */
#define VIDEO_RESENDS			256

/* The milliseconds between reports to our peer. */
#define VIDEO_REPORT_INTERVAL		500

/* The milliseconds between keyframe requests, and between those we honor. */
#define VIDEO_KEY_INTERVAL		250

/*
 * The milliseconds after sending a keyframe we ignore requests for another,
 * those our peer sent before it got the keyframe are still coming in.
 */
#define VIDEO_KEY_HOLD			500

/*
 * The milliseconds we skip frames waiting on a keyframe before we ask our
 * encoder again, it may have dropped the frame it was asked for.
 */
#define VIDEO_KEY_RETRY			500

/*
 * The milliseconds a frame may go without a new fragment, and how long
 * it gets to arrive in full.
 */
#define VIDEO_FRAME_TIMEOUT		400
#define VIDEO_FRAME_TIMEOUT_MAX		1500

/* The frames waiting to be sent before we skip ahead to a keyframe. */
#define VIDEO_BACKLOG			8

/*
 * The milliseconds between asking for the fragments of a frame we miss,
 * and how long we wait on the last fragments before we ask for those too.
 *
 * All of these timers are the least we wait, on a slow path such as one
 * under commixtion we wait as long as the round trip needs.
 */
#define VIDEO_NACK_INTERVAL		100
#define VIDEO_TAIL_WAIT			30

/*
 * The bitrate we start at and its bounds, in bits per second. We only
 * back off on loss, by then the queues on the path are full and the call
 * lags, so we stay well below what links can take. More does not look
 * any better at 960x540.
 */
#define VIDEO_RATE_START		(1000 * 1000)
#define VIDEO_RATE_MIN			(150 * 1000)
#define VIDEO_RATE_MAX			(2500 * 1000)

/* The most packets we send in one go, below the libkyrka replay window. */
#define VIDEO_BURST			48

/*
 * The most bytes we let our pacing build up, we pace at twice our bitrate
 * so a keyframe does not hold up the frames after it for long. Kept small
 * so a burst does not queue up the voice behind it.
 */
#define VIDEO_TOKENS_MAX		(16 * 1500)

/*
 * An encoded frame, either one we send or one we put together.
 */
struct video_frame {
	u_int32_t		id;
	u_int8_t		flags;
	u_int32_t		size;
	u_int16_t		count;
	u_int16_t		fragsize;
	u_int16_t		got;
	u_int16_t		highest;
	u_int16_t		fec;
	u_int64_t		last;
	u_int64_t		sent;
	u_int8_t		*data;
	u_int8_t		*parity;
	u_int8_t		seen[VIDEO_FRAGMENTS_MAX / 8];
	u_int8_t		asked[VIDEO_FRAGMENTS_MAX / 8];
	u_int8_t		pseen[VIDEO_FRAGMENTS_MAX / 8];
};

/*
 * The frames for a direction, those that are free and those queued
 * for the voice thread or the platform video.
 */
struct video_pool {
	struct vigil_ring	free;
	struct vigil_ring	queue;
	struct video_frame	frames[VIDEO_FRAMES];
};

/*
 * A frame we are putting together, or one we know of but have not seen
 * a fragment of yet (slot is -1).
 */
struct video_entry {
	int			used;
	int			done;
	int			lost;
	int			slot;
	int			asked;

	u_int32_t		id;
	u_int64_t		first;
	u_int64_t		next_nack;
};

/*
 * A frame we sent and when, the first time our peer asks for it again
 * tells us about the round trip.
 */
struct video_sent {
	u_int32_t		id;
	u_int64_t		when;
	int			nacked;
};

/*
 * A fragment of a frame we sent that our peer asked for again.
 */
struct video_resend {
	u_int32_t		id;
	u_int32_t		slot;
	u_int16_t		fragment;
};

static void	video_start(void);
static void	video_flush(void);
static void	video_catch_up(void);
static void	video_key_need(const char *);
static void	video_send(struct vigil_video_link *);
static void	video_drain(struct vigil_video_link *);
static void	video_expire(struct vigil_video_link *);
static void	video_ctl_send(struct vigil_video_link *);
static void	video_key_honor(struct vigil_video_link *);
static void	video_rate_adjust(const struct vigil_video_ctl *);

static u_int64_t	video_rtt(void);
static u_int64_t	video_rtt_wait(u_int64_t, u_int64_t);
static void		video_rtt_sample(u_int32_t, u_int64_t);

static void	video_tx_skip(u_int64_t);
static void	video_tx_finish(u_int64_t);
static void	video_tx_release(u_int32_t);
static void	video_tx_history(u_int64_t);
static void	video_tx_begin(struct vigil_video_link *, u_int32_t);
static void	video_tx_parity(struct vigil_video_link *, u_int32_t,
		    u_int16_t);
static void	video_tx_fragment(struct vigil_video_link *, u_int32_t,
		    u_int16_t);

static void	video_rx_release(struct video_entry *);
static void	video_rx_recover(struct video_entry *, u_int16_t);
static void	video_rx_nack(struct vigil_video_link *, struct video_entry *);
static int	video_rx_slot(struct video_entry *,
		    const struct vigil_video_hdr *);
static int	video_rx_parity(struct video_entry *,
		    const struct vigil_video_hdr *, const u_int8_t *,
		    u_int64_t);
static int	video_rx_fragment(struct video_entry *,
		    const struct vigil_video_hdr *, const u_int8_t *,
		    u_int64_t);

static struct video_pool	*video_pool_alloc(void);
static void			video_pool_free(struct video_pool *);
static void			video_resend_queue(u_int32_t, u_int16_t);
static struct video_entry	*video_entry_get(u_int32_t, u_int64_t);
static struct video_entry	*video_entry_new(u_int32_t, u_int64_t);

/* The frames we send and get, read by the platform video. */
static struct video_pool	*tx = NULL;
static struct video_pool	*rx = NULL;

/* If the platform wants our camera on, may be set from any thread. */
static volatile int		camera = 0;

/* Set when the platform dropped a frame and has to send a keyframe. */
static volatile int		force_key = 0;

/* Set when the platform decoder starts over and needs a keyframe. */
static volatile int		refresh = 0;

/* What we show in vigil_video_status(). */
static volatile int		remote = 0;
static volatile int		sending = 0;
static volatile u_int64_t	resent = 0;
static volatile u_int64_t	rx_frames = 0;
static volatile u_int64_t	tx_frames = 0;
static volatile u_int64_t	lost_frames = 0;
static volatile u_int32_t	rate = VIDEO_RATE_START;

/* If we started the platform video, only touched by the voice thread. */
static int			started = 0;

/* If we were talking the last time we ran. */
static int			talked = 0;

/* The frame we are sending and its next fragment. */
static u_int32_t		tx_id = 0;
static u_int32_t		tx_slot = 0;
static u_int16_t		tx_next = 0;
static int			tx_busy = 0;

/* Set while our peer waits for a keyframe, we send it nothing else. */
static int			tx_skip = 0;
static u_int64_t		skip_since = 0;

/* When we last sent a keyframe in full. */
static u_int64_t		key_sent = 0;

/* The frames we sent, oldest first, for resends. */
static u_int32_t		history[VIDEO_HISTORY];
static int			history_count = 0;

/* The round trip we measured, the one our peer did and what we sent. */
static u_int64_t		rtt = 0;
static u_int64_t		rtt_min = 0;
static u_int64_t		rtt_last = 0;
static u_int64_t		peer_rtt = 0;
static struct video_sent	stamps[VIDEO_SENT];

/* The fragments we resend first. */
static struct video_resend	resends[VIDEO_RESENDS];
static u_int32_t		resend_head = 0;
static u_int32_t		resend_tail = 0;

/* Our pacing, in bytes we may send. */
static u_int64_t		tokens = 0;
static u_int64_t		tokens_last = 0;

/* The frames we are putting together. */
static struct video_entry	entries[VIDEO_ASSEMBLY];

/* Where we rebuild a fragment from its parity. */
static u_int8_t			rebuild[KYRKA_PACKET_DATA_LEN];

/* The last frame we handed to the platform or gave up on. */
static u_int32_t		rx_last = 0;

/* If we need a keyframe before we can hand frames over again. */
static int			need_key = 1;

/*
 * If a frame is stuck for so long that a keyframe gets here sooner than
 * its resends, we ask for one but keep waiting on the frame too.
 */
static int			want_key = 0;

/* When we last asked for and honored a keyframe, and next report. */
static u_int64_t		key_asked = 0;
static u_int64_t		key_honored = 0;
static u_int64_t		next_report = 0;

/* What we report to our peer next. */
static u_int32_t		report_lost = 0;
static u_int32_t		report_nacks = 0;
static u_int32_t		report_frames = 0;
static u_int32_t		report_fragments = 0;

/* The bitrate we last gave the platform. */
static u_int32_t		rate_told = 0;

/*
 * Turn our camera on or off, may be called from any thread.
 */
void
vigil_video_camera(int on)
{
	PRECOND(on == 0 || on == 1);

	vigil_atomic_write(&camera, on);
}

/*
 * The platform decoder starts over, ask our peer for a keyframe. May be
 * called from any thread.
 */
void
vigil_video_refresh(void)
{
	vigil_atomic_write(&refresh, 1);
}

/*
 * Called by the platform video with an encoded frame from our camera,
 * returns -1 if we cannot take it right now. May be called from any
 * thread while the platform video runs.
 */
int
vigil_video_input(const u_int8_t *data, size_t len, int flags)
{
	u_int32_t		idx;
	struct video_pool	*pool;
	struct video_frame	*frame;

	PRECOND(data != NULL);

	pool = vigil_atomic_read(&tx);
	if (pool == NULL)
		return (-1);

	if (len == 0 || len > VIGIL_VIDEO_FRAME_MAX) {
		vigil_atomic_write(&force_key, 1);
		return (-1);
	}

	if (vigil_ring_dequeue(&pool->free, &idx) == -1) {
		vigil_atomic_write(&force_key, 1);
		return (-1);
	}

	VERIFY(idx < VIDEO_FRAMES);

	frame = &pool->frames[idx];
	VERIFY(frame->data == NULL);

	frame->data = malloc(len);
	if (frame->data == NULL)
		fatal("malloc: failed to allocate video frame");

	frame->size = len;
	frame->flags = flags & 0xff;
	memcpy(frame->data, data, len);

	if (vigil_ring_queue(&pool->queue, idx) == -1)
		fatal("video tx queue overflow");

	return (0);
}

/*
 * Called by the platform video for the next frame to decode, buf must
 * hold VIGIL_VIDEO_FRAME_MAX bytes. Returns the length of the frame or 0
 * if there is none. May be called from any thread while the platform
 * video runs.
 */
size_t
vigil_video_take(u_int8_t *buf, size_t len, int *flags)
{
	u_int32_t		idx;
	size_t			size;
	struct video_pool	*pool;
	struct video_frame	*frame;

	PRECOND(buf != NULL);
	PRECOND(len >= VIGIL_VIDEO_FRAME_MAX);
	PRECOND(flags != NULL);

	pool = vigil_atomic_read(&rx);
	if (pool == NULL)
		return (0);

	if (vigil_ring_dequeue(&pool->queue, &idx) == -1)
		return (0);

	VERIFY(idx < VIDEO_FRAMES);

	frame = &pool->frames[idx];
	size = frame->size;
	*flags = frame->flags;

	VERIFY(size <= len);
	VERIFY(frame->data != NULL);

	memcpy(buf, frame->data, size);

	free(frame->data);
	frame->data = NULL;

	free(frame->parity);
	frame->parity = NULL;

	if (vigil_ring_queue(&pool->free, idx) == -1)
		fatal("video rx free overflow");

	return (size);
}

/*
 * Reset the video for a new voice session.
 */
void
vigil_video_open(void)
{
	PRECOND(started == 0);

	tx_id = 0;
	talked = 0;
	tx_busy = 0;
	tx_skip = 0;
	key_sent = 0;
	skip_since = 0;
	history_count = 0;

	rtt = 0;
	rtt_min = 0;
	rtt_last = 0;
	peer_rtt = 0;
	memset(stamps, 0, sizeof(stamps));

	resend_head = 0;
	resend_tail = 0;

	tokens = 0;
	tokens_last = 0;

	rx_last = 0;
	need_key = 1;
	want_key = 0;
	key_asked = 0;
	rate_told = 0;
	key_honored = 0;
	next_report = 0;
	report_lost = 0;
	report_nacks = 0;
	report_frames = 0;
	report_fragments = 0;

	memset(entries, 0, sizeof(entries));

	vigil_atomic_write(&remote, 0);
	vigil_atomic_write(&sending, 0);
	vigil_atomic_write(&refresh, 0);
	vigil_atomic_write(&force_key, 0);
	vigil_atomic_write(&resent, 0);
	vigil_atomic_write(&rx_frames, 0);
	vigil_atomic_write(&tx_frames, 0);
	vigil_atomic_write(&lost_frames, 0);
	vigil_atomic_write(&rate, VIDEO_RATE_START);
}

/*
 * Stop the platform video and release our frames, the voice session ended.
 */
void
vigil_video_close(void)
{
	struct video_pool	*in, *out;

	in = vigil_atomic_read(&rx);
	out = vigil_atomic_read(&tx);

	vigil_atomic_write(&rx, NULL);
	vigil_atomic_write(&tx, NULL);

	if (started) {
		vigil_platform_video_stop();
		started = 0;
	}

	video_pool_free(in);
	video_pool_free(out);

	memset(entries, 0, sizeof(entries));

	vigil_atomic_write(&remote, 0);
	vigil_atomic_write(&sending, 0);
}

/*
 * Called by the voice session every time it runs. We start the platform
 * video once either of us has a camera on, tell the platform if our camera
 * should be on, send what we can and tell our peer how we are doing.
 */
void
vigil_video_update(struct vigil_video_link *link)
{
	int		want;
	u_int32_t	encode;

	PRECOND(link != NULL);
	PRECOND(link->ctx != NULL);

	if (link->live && vigil_atomic_read(&camera))
		want = 1;
	else
		want = 0;

	if (started == 0) {
		if (want)
			video_start();
		else if (link->talking && vigil_atomic_read(&remote))
			video_start();
	}

	if (started == 0)
		return;

	if (want != vigil_atomic_read(&sending)) {
		vigil_atomic_write(&sending, want);
		vigil_platform_video_camera(want);

		if (want)
			vigil_atomic_write(&force_key, 1);
	}

	/* What we encoded before was never sent, start with a keyframe. */
	if (link->talking && talked == 0)
		vigil_atomic_write(&force_key, 1);

	talked = link->talking;

	if (vigil_atomic_read(&force_key)) {
		vigil_atomic_write(&force_key, 0);
		vigil_platform_video_keyframe();
	}

	/* Our rate is what goes on the wire, parity included. */
	encode = vigil_atomic_read(&rate);
	if (video_rtt() >= VIDEO_FEC_RTT)
		encode = encode * VIDEO_FEC_GROUP / (VIDEO_FEC_GROUP + 1);

	if (rate_told != encode) {
		rate_told = encode;
		vigil_platform_video_bitrate(rate_told);
	}

	if (link->online == 0)
		return;

	/* This also tells a peer that rings for us that our camera is on. */
	if (link->now >= next_report || (link->talking &&
	    (need_key || want_key) &&
	    (link->now - key_asked) >= video_rtt_wait(VIDEO_KEY_INTERVAL,
	    video_rtt() + VIDEO_RTT_MARGIN)))
		video_ctl_send(link);

	if (link->talking == 0) {
		video_drain(link);
		return;
	}

	if (vigil_atomic_read(&refresh)) {
		vigil_atomic_write(&refresh, 0);
		video_key_need("the decoder started over");
	}

	video_expire(link);
	video_flush();
	video_send(link);
}

/*
 * Our peer told us about its video, if its camera is on, if it needs
 * a keyframe and how much of our video it got and lost.
 */
void
vigil_video_ctl(struct vigil_video_link *link,
    const struct vigil_video_ctl *ctl)
{
	PRECOND(link != NULL);
	PRECOND(ctl != NULL);

	if (ctl->camera != vigil_atomic_read(&remote)) {
		vigil_log(LOG_INFO, "[video] peer camera %s",
		    ctl->camera ? "on" : "off");
		vigil_atomic_write(&remote, ctl->camera ? 1 : 0);
	}

	peer_rtt = ntohs(ctl->rtt);

	if (vigil_atomic_read(&sending) == 0)
		return;

	if (ctl->keyframe)
		video_key_honor(link);

	video_rate_adjust(ctl);
}

/*
 * Our peer misses fragments of one of our frames, queue them for a resend
 * if we still have the frame.
 */
void
vigil_video_nack(struct vigil_video_link *link,
    const struct vigil_video_nack *nack)
{
	int			idx;
	struct video_pool	*pool;
	struct video_frame	*frame;
	u_int32_t		id, slot;
	u_int16_t		count, fragment, sent;

	PRECOND(link != NULL);
	PRECOND(nack != NULL);

	pool = vigil_atomic_read(&tx);
	if (pool == NULL)
		return;

	id = ntohl(nack->frame);
	count = ntohs(nack->count);

	if (count > VIGIL_VIDEO_NACK_MAX)
		return;

	video_rtt_sample(id, link->now);

	slot = 0;
	sent = 0;
	frame = NULL;

	for (idx = 0; idx < history_count; idx++) {
		if (pool->frames[history[idx]].id == id) {
			slot = history[idx];
			frame = &pool->frames[slot];
			sent = frame->count;
			break;
		}
	}

	if (frame == NULL && tx_busy && pool->frames[tx_slot].id == id) {
		slot = tx_slot;
		frame = &pool->frames[slot];
		sent = tx_next;
	}

	if (frame == NULL)
		return;

	if (count == 0) {
		for (fragment = 0; fragment < sent; fragment++)
			video_resend_queue(slot, fragment);
		return;
	}

	for (idx = 0; idx < count; idx++) {
		fragment = ntohs(nack->fragments[idx]);
		if (fragment < sent)
			video_resend_queue(slot, fragment);
	}
}

/*
 * We got a fragment of a frame from our peer, put it where it belongs
 * and hand the frames to the platform once we have them, in order.
 */
void
vigil_video_recv(struct vigil_video_link *link, const u_int8_t *data,
    size_t len)
{
	struct vigil_video_hdr	hdr;
	struct video_entry	*entry;
	u_int32_t		id, size, count, expect;

	PRECOND(link != NULL);
	PRECOND(data != NULL);

	if (vigil_atomic_read(&rx) == NULL)
		return;

	if (len < sizeof(hdr))
		return;

	memcpy(&hdr, data, sizeof(hdr));

	id = ntohl(hdr.frame);
	size = ntohl(hdr.size);
	hdr.length = ntohs(hdr.length);
	hdr.fragment = ntohs(hdr.fragment);
	hdr.fragsize = ntohs(hdr.fragsize);

	if (size == 0 || size > VIGIL_VIDEO_FRAME_MAX)
		return;

	if (hdr.fragsize == 0 || hdr.fragsize > KYRKA_PACKET_DATA_LEN)
		return;

	count = (size + hdr.fragsize - 1) / hdr.fragsize;
	if (count > VIDEO_FRAGMENTS_MAX)
		return;

	if (hdr.fragment >= count) {
		if (hdr.length == 0 || hdr.length > VIDEO_FEC_GROUP_MAX)
			return;

		if (hdr.fragment - count >= (count + hdr.length - 1) /
		    hdr.length)
			return;

		if (hdr.fragsize > len - sizeof(hdr))
			return;
	} else {
		if (hdr.fragment == count - 1)
			expect = size - hdr.fragment * hdr.fragsize;
		else
			expect = hdr.fragsize;

		if (hdr.length != expect || hdr.length > len - sizeof(hdr))
			return;
	}

	if (rx_last != 0 && (int32_t)(id - rx_last) <= 0)
		return;

	report_fragments++;

	entry = video_entry_get(id, link->now);
	if (entry == NULL || entry->done)
		return;

	hdr.frame = id;
	hdr.size = size;

	if (hdr.fragment >= count) {
		if (video_rx_parity(entry, &hdr,
		    data + sizeof(hdr), link->now) == -1)
			return;
	} else {
		if (video_rx_fragment(entry, &hdr,
		    data + sizeof(hdr), link->now) == -1)
			return;
	}

	video_flush();
}

/*
 * Write the state of our video as a JSON object into buf, returns its
 * length or 0 if buf was too small.
 */
size_t
vigil_video_status(char *buf, size_t len)
{
	int		ret;

	PRECOND(buf != NULL);
	PRECOND(len > 0);

	ret = snprintf(buf, len,
	    "{\"camera\":%s,\"remote\":%s,\"bitrate\":%u,"
	    "\"tx\":%" PRIu64 ",\"rx\":%" PRIu64 ",\"lost\":%" PRIu64
	    ",\"resent\":%" PRIu64 "}",
	    vigil_atomic_read(&sending) ? "true" : "false",
	    vigil_atomic_read(&remote) ? "true" : "false",
	    vigil_atomic_read(&rate), vigil_atomic_read(&tx_frames),
	    vigil_atomic_read(&rx_frames), vigil_atomic_read(&lost_frames),
	    vigil_atomic_read(&resent));

	if (ret < 0)
		return (0);

	if ((size_t)ret >= len)
		return (0);

	return ((size_t)ret);
}

/*
 * Set up our frames and start the platform video.
 */
static void
video_start(void)
{
	struct video_pool	*in, *out;

	PRECOND(started == 0);

	in = video_pool_alloc();
	out = video_pool_alloc();

	vigil_atomic_write(&rx, in);
	vigil_atomic_write(&tx, out);

	if (vigil_platform_video_start() == -1) {
		vigil_log(LOG_ERR, "[video] no video");
		vigil_atomic_write(&rx, NULL);
		vigil_atomic_write(&tx, NULL);
		video_pool_free(in);
		video_pool_free(out);
		return;
	}

	started = 1;
	vigil_log(LOG_INFO, "[video] started");
}

/*
 * We cannot send yet, drop what the platform gave us. It still shows it.
 */
static void
video_drain(struct vigil_video_link *link)
{
	u_int32_t		slot;
	struct video_pool	*pool;

	PRECOND(link != NULL);

	pool = vigil_atomic_read(&tx);
	if (pool == NULL)
		return;

	while (vigil_ring_dequeue(&pool->queue, &slot) != -1)
		video_tx_release(slot);

	if (tx_busy) {
		video_tx_release(tx_slot);
		tx_busy = 0;
	}

	while (history_count > 0)
		video_tx_release(history[--history_count]);

	resend_head = resend_tail;
	tokens_last = link->now;
}

/*
 * Send as many fragments as our pacing allows, those our peer asked for
 * again first.
 */
static void
video_send(struct vigil_video_link *link)
{
	int			idx;
	u_int32_t		slot;
	struct video_pool	*pool;
	struct video_resend	*resend;
	u_int64_t		elapsed;

	PRECOND(link != NULL);

	pool = vigil_atomic_read(&tx);
	if (pool == NULL)
		return;

	video_tx_history(link->now);

	elapsed = link->now - tokens_last;
	tokens_last = link->now;

	/*
	 * Twice our rate gets a keyframe out quickly, on a slow path that
	 * waits a round trip anyway we rather not burst past what it takes.
	 */
	if (video_rtt() >= VIDEO_FEC_RTT)
		tokens += elapsed * vigil_atomic_read(&rate) / 6400;
	else
		tokens += elapsed * vigil_atomic_read(&rate) / 4000;
	if (tokens > VIDEO_TOKENS_MAX)
		tokens = VIDEO_TOKENS_MAX;

	if (tx_skip && (link->now - skip_since) >= VIDEO_KEY_RETRY) {
		vigil_log(LOG_INFO, "[video] no keyframe yet, asking again");
		video_tx_skip(link->now);
	}

	for (idx = 0; idx < VIDEO_BURST && tokens > 0; idx++) {
		if (resend_head != resend_tail) {
			resend = &resends[resend_head++ & (VIDEO_RESENDS - 1)];

			if (pool->frames[resend->slot].id != resend->id)
				continue;

			if (pool->frames[resend->slot].data == NULL)
				continue;

			video_tx_fragment(link, resend->slot, resend->fragment);
			vigil_atomic_add(&resent, 1);
			continue;
		}

		if (tx_busy == 0) {
			if (vigil_ring_dequeue(&pool->queue, &slot) == -1)
				break;

			/* We fell behind, what is queued is too old to send. */
			if (tx_skip == 0 &&
			    vigil_ring_pending(&pool->queue) >= VIDEO_BACKLOG) {
				vigil_log(LOG_INFO, "[video] fell behind, "
				    "skipping to a keyframe");
				video_tx_skip(link->now);
			}

			if (tx_skip) {
				if (!(pool->frames[slot].flags &
				    VIGIL_VIDEO_FLAG_KEY)) {
					video_tx_release(slot);
					continue;
				}

				tx_skip = 0;
			}

			video_tx_begin(link, slot);

			if (tx_busy == 0)
				continue;
		}

		video_tx_fragment(link, tx_slot, tx_next);
		tx_next++;

		if (pool->frames[tx_slot].fec != 0 &&
		    (tx_next % pool->frames[tx_slot].fec == 0 ||
		    tx_next == pool->frames[tx_slot].count)) {
			video_tx_parity(link, tx_slot,
			    (tx_next - 1) / pool->frames[tx_slot].fec);
		}

		if (tx_next == pool->frames[tx_slot].count)
			video_tx_finish(link->now);
	}
}

/*
 * Start sending the frame in the given slot, its fragments are sized for
 * the current mtu of the tunnel.
 */
static void
video_tx_begin(struct vigil_video_link *link, u_int32_t slot)
{
	u_int32_t		count;
	struct video_pool	*pool;
	struct video_frame	*frame;

	PRECOND(link != NULL);
	PRECOND(slot < VIDEO_FRAMES);
	PRECOND(tx_busy == 0);

	pool = vigil_atomic_read(&tx);
	VERIFY(pool != NULL);

	frame = &pool->frames[slot];

	if (link->mtu <= sizeof(struct vigil_video_hdr))
		fatal("video fragment does not fit");

	frame->fragsize = link->mtu - sizeof(struct vigil_video_hdr);
	count = (frame->size + frame->fragsize - 1) / frame->fragsize;

	if (count > VIDEO_FRAGMENTS_MAX) {
		video_tx_release(slot);
		vigil_atomic_write(&force_key, 1);
		return;
	}

	frame->id = ++tx_id;
	frame->count = count;

	if (video_rtt() >= VIDEO_FEC_RTT)
		frame->fec = VIDEO_FEC_GROUP;
	else
		frame->fec = 0;

	tx_next = 0;
	tx_busy = 1;
	tx_slot = slot;
}

/*
 * Send our peer nothing but a keyframe, which we ask our encoder for.
 */
static void
video_tx_skip(u_int64_t now)
{
	tx_skip = 1;
	skip_since = now;
	vigil_atomic_write(&force_key, 1);
}

/*
 * The frame we were sending is out, keep it for resends.
 */
static void
video_tx_finish(u_int64_t now)
{
	int			idx;
	struct video_pool	*pool;

	PRECOND(tx_busy == 1);

	pool = vigil_atomic_read(&tx);
	VERIFY(pool != NULL);

	if (pool->frames[tx_slot].flags & VIGIL_VIDEO_FLAG_KEY)
		key_sent = now;

	pool->frames[tx_slot].sent = now;

	stamps[tx_id & (VIDEO_SENT - 1)].id = pool->frames[tx_slot].id;
	stamps[tx_id & (VIDEO_SENT - 1)].when = now;
	stamps[tx_id & (VIDEO_SENT - 1)].nacked = 0;

	if (history_count == VIDEO_HISTORY) {
		video_tx_release(history[0]);

		for (idx = 1; idx < VIDEO_HISTORY; idx++)
			history[idx - 1] = history[idx];

		history_count--;
	}

	history[history_count++] = tx_slot;
	tx_busy = 0;

	vigil_atomic_add(&tx_frames, 1);
}

/*
 * Give back the frames we sent long enough ago that our peer will not
 * ask for them anymore.
 */
static void
video_tx_history(u_int64_t now)
{
	int			idx;
	u_int64_t		keep;
	struct video_pool	*pool;

	pool = vigil_atomic_read(&tx);
	VERIFY(pool != NULL);

	keep = video_rtt_wait(VIDEO_HISTORY_TIME,
	    2 * (video_rtt() + VIDEO_RTT_MARGIN));

	while (history_count > 0 &&
	    now - pool->frames[history[0]].sent > keep) {
		video_tx_release(history[0]);

		for (idx = 1; idx < history_count; idx++)
			history[idx - 1] = history[idx];

		history_count--;
	}
}

/*
 * Send the given fragment of the frame in the given slot, at the full
 * mtu of the tunnel the frame was split for.
 */
static void
video_tx_fragment(struct vigil_video_link *link, u_int32_t slot,
    u_int16_t fragment)
{
	struct kyrka_packet		pkt;
	size_t				max;
	struct vigil_video_hdr		*hdr;
	u_int8_t			*ptr;
	struct video_pool		*pool;
	struct video_frame		*frame;
	u_int32_t			offset, length;

	PRECOND(link != NULL);
	PRECOND(slot < VIDEO_FRAMES);

	pool = vigil_atomic_read(&tx);
	VERIFY(pool != NULL);

	frame = &pool->frames[slot];
	VERIFY(fragment < frame->count);

	ptr = kyrka_packet_databuf(link->ctx, &pkt, &max);
	if (ptr == NULL)
		fatal("failed to get video data buffer");

	if (sizeof(*hdr) + frame->fragsize > max)
		fatal("video fragment does not fit");

	offset = fragment * frame->fragsize;
	length = frame->size - offset;

	if (length > frame->fragsize)
		length = frame->fragsize;

	memset(ptr, 0, sizeof(*hdr) + frame->fragsize);

	hdr = (struct vigil_video_hdr *)ptr;
	hdr->type = VIGIL_VOICE_TYPE_VIDEO;
	hdr->flags = frame->flags;
	hdr->frame = htonl(frame->id);
	hdr->size = htonl(frame->size);
	hdr->length = htons(length);
	hdr->fragment = htons(fragment);
	hdr->fragsize = htons(frame->fragsize);

	memcpy(ptr + sizeof(*hdr), frame->data + offset, length);

	pkt.shroud = link->shroud;
	pkt.length = sizeof(*hdr) + frame->fragsize;

	if (kyrka_heaven_input(link->ctx, &pkt) == -1 &&
	    kyrka_last_error(link->ctx) != KYRKA_ERROR_NO_TX_KEY) {
		vigil_log(LOG_NOTICE, "[video] kyrka_heaven_input: %d",
		    kyrka_last_error(link->ctx));
	}

	if (tokens > pkt.length)
		tokens -= pkt.length;
	else
		tokens = 0;
}

/*
 * Send the parity of the given group of fragments of the frame in the
 * given slot, the xor of all of them padded to the fragment size. Its
 * length says how many fragments a group has.
 */
static void
video_tx_parity(struct vigil_video_link *link, u_int32_t slot,
    u_int16_t group)
{
	struct kyrka_packet		pkt;
	size_t				max;
	struct vigil_video_hdr		*hdr;
	u_int8_t			*ptr;
	struct video_pool		*pool;
	struct video_frame		*frame;
	u_int32_t			idx, first, last, offset, length, byte;

	PRECOND(link != NULL);
	PRECOND(slot < VIDEO_FRAMES);

	pool = vigil_atomic_read(&tx);
	VERIFY(pool != NULL);

	frame = &pool->frames[slot];
	VERIFY(frame->fec != 0);

	ptr = kyrka_packet_databuf(link->ctx, &pkt, &max);
	if (ptr == NULL)
		fatal("failed to get video data buffer");

	if (sizeof(*hdr) + frame->fragsize > max)
		fatal("video parity does not fit");

	memset(ptr, 0, sizeof(*hdr) + frame->fragsize);

	first = group * frame->fec;
	last = first + frame->fec;

	if (last > frame->count)
		last = frame->count;

	for (idx = first; idx < last; idx++) {
		offset = idx * frame->fragsize;
		length = frame->size - offset;

		if (length > frame->fragsize)
			length = frame->fragsize;

		for (byte = 0; byte < length; byte++)
			ptr[sizeof(*hdr) + byte] ^= frame->data[offset + byte];
	}

	hdr = (struct vigil_video_hdr *)ptr;
	hdr->type = VIGIL_VOICE_TYPE_VIDEO;
	hdr->flags = frame->flags;
	hdr->frame = htonl(frame->id);
	hdr->size = htonl(frame->size);
	hdr->length = htons(frame->fec);
	hdr->fragment = htons(frame->count + group);
	hdr->fragsize = htons(frame->fragsize);

	pkt.shroud = link->shroud;
	pkt.length = sizeof(*hdr) + frame->fragsize;

	if (kyrka_heaven_input(link->ctx, &pkt) == -1 &&
	    kyrka_last_error(link->ctx) != KYRKA_ERROR_NO_TX_KEY) {
		vigil_log(LOG_NOTICE, "[video] kyrka_heaven_input: %d",
		    kyrka_last_error(link->ctx));
	}

	if (tokens > pkt.length)
		tokens -= pkt.length;
	else
		tokens = 0;
}

/*
 * Give the frame in the given slot back to the platform.
 */
static void
video_tx_release(u_int32_t slot)
{
	struct video_pool	*pool;

	PRECOND(slot < VIDEO_FRAMES);

	pool = vigil_atomic_read(&tx);
	VERIFY(pool != NULL);

	free(pool->frames[slot].data);
	pool->frames[slot].data = NULL;

	/* Resends still queued for it must not match what comes next. */
	pool->frames[slot].id = 0;
	pool->frames[slot].count = 0;

	if (vigil_ring_queue(&pool->free, slot) == -1)
		fatal("video tx free overflow");
}

/*
 * Queue the given fragment of the frame in the given slot for a resend,
 * we drop it if too many are queued already.
 */
static void
video_resend_queue(u_int32_t slot, u_int16_t fragment)
{
	struct video_pool	*pool;
	struct video_resend	*resend;

	PRECOND(slot < VIDEO_FRAMES);

	if (resend_tail - resend_head == VIDEO_RESENDS)
		return;

	pool = vigil_atomic_read(&tx);
	VERIFY(pool != NULL);

	resend = &resends[resend_tail++ & (VIDEO_RESENDS - 1)];
	resend->slot = slot;
	resend->fragment = fragment;
	resend->id = pool->frames[slot].id;
}

/*
 * Tell our peer about our camera, if we need a keyframe and how much of
 * its video we got and lost since we last told it.
 */
static void
video_ctl_send(struct vigil_video_link *link)
{
	struct kyrka_packet	pkt;
	size_t			max;
	struct vigil_video_ctl	ctl;
	struct vigil_voice_hdr	*hdr;
	u_int8_t		*ptr;

	PRECOND(link != NULL);

	next_report = link->now + VIDEO_REPORT_INTERVAL;

	memset(&ctl, 0, sizeof(ctl));
	ctl.rtt = htons(rtt > UINT16_MAX ? UINT16_MAX : rtt);
	ctl.camera = vigil_atomic_read(&sending);
	ctl.keyframe = need_key || want_key;
	ctl.lost = htonl(report_lost);
	ctl.nacks = htonl(report_nacks);
	ctl.frames = htonl(report_frames);
	ctl.fragments = htonl(report_fragments);

	if (need_key || want_key)
		key_asked = link->now;

	report_lost = 0;
	report_nacks = 0;
	report_frames = 0;
	report_fragments = 0;

	ptr = kyrka_packet_databuf(link->ctx, &pkt, &max);
	if (ptr == NULL)
		fatal("failed to get video ctl buffer");

	if (max < VIGIL_VOICE_PAYLOAD)
		fatal("voice payload does not fit");

	memset(ptr, 0, VIGIL_VOICE_PAYLOAD);

	hdr = (struct vigil_voice_hdr *)ptr;
	hdr->type = VIGIL_VOICE_TYPE_VIDEO_CTL;
	hdr->length = htons(sizeof(ctl));

	memcpy(ptr + sizeof(*hdr), &ctl, sizeof(ctl));
	pkt.length = VIGIL_VOICE_PAYLOAD;
	pkt.shroud = link->shroud;

	if (kyrka_heaven_input(link->ctx, &pkt) == -1 &&
	    kyrka_last_error(link->ctx) != KYRKA_ERROR_NO_TX_KEY) {
		vigil_log(LOG_NOTICE, "[video] kyrka_heaven_input: %d",
		    kyrka_last_error(link->ctx));
	}
}

/*
 * Our peer needs a keyframe. We send it nothing else until it has one,
 * unless one is on its way to it already.
 */
static void
video_key_honor(struct vigil_video_link *link)
{
	struct video_pool	*pool;

	PRECOND(link != NULL);

	pool = vigil_atomic_read(&tx);
	if (pool == NULL)
		return;

	if (tx_skip)
		return;

	if ((link->now - key_honored) < video_rtt_wait(VIDEO_KEY_INTERVAL,
	    video_rtt() + VIDEO_RTT_MARGIN))
		return;

	if (key_sent != 0 && (link->now - key_sent) <
	    video_rtt_wait(VIDEO_KEY_HOLD, video_rtt() + VIDEO_KEY_HOLD / 2))
		return;

	if (tx_busy) {
		if (pool->frames[tx_slot].flags & VIGIL_VIDEO_FLAG_KEY)
			return;

		video_tx_release(tx_slot);
		tx_busy = 0;
	}

	vigil_log(LOG_INFO, "[video] our peer needs a keyframe");
	key_honored = link->now;

	resend_head = resend_tail;
	video_tx_skip(link->now);
}

/*
 * Adjust our bitrate to how many of our frames our peer lost and how many
 * of our fragments it had to ask for again. Some loss we recover from by
 * resending is not a reason to slow down, a lot of it is.
 */
static void
video_rate_adjust(const struct vigil_video_ctl *ctl)
{
	u_int64_t	next;
	u_int32_t	frames, lost, fragments, nacks, ploss;

	PRECOND(ctl != NULL);

	lost = ntohl(ctl->lost);
	nacks = ntohl(ctl->nacks);
	frames = ntohl(ctl->frames);
	fragments = ntohl(ctl->fragments);

	if (frames == 0 && lost == 0)
		return;

	ploss = (u_int64_t)nacks * 100 / ((u_int64_t)fragments + nacks + 1);
	next = vigil_atomic_read(&rate);

	if (lost >= 3 || ploss >= 20)
		next = next * 7 / 10;
	else if (lost > 0 || ploss >= 10)
		next = next * 9 / 10;
	else if (rtt_min != 0 && rtt_last > rtt_min +
	    video_rtt_wait(VIDEO_RTT_QUEUE, rtt_min / 4))
		next = next * 85 / 100;
	else if (ploss < 5 && video_rtt() >= VIDEO_FEC_RTT)
		next = next * 102 / 100;
	else if (ploss < 5)
		next = next * 104 / 100;

	/* A round trip only counts until the next report. */
	rtt_last = 0;

	if (next < VIDEO_RATE_MIN)
		next = VIDEO_RATE_MIN;

	if (next > VIDEO_RATE_MAX)
		next = VIDEO_RATE_MAX;

	vigil_atomic_write(&rate, next);
}

/*
 * Ask for the fragments of frames we miss, and give up on frames that
 * stopped coming in or take too long.
 */
static void
video_expire(struct vigil_video_link *link)
{
	int			idx;
	struct video_pool	*pool;
	struct video_entry	*entry;
	u_int64_t		progress, wait, most, stuck;

	PRECOND(link != NULL);

	pool = vigil_atomic_read(&rx);
	VERIFY(pool != NULL);

	want_key = 0;
	stuck = video_rtt_wait(VIDEO_FRAME_TIMEOUT,
	    2 * (video_rtt() + VIDEO_RTT_MARGIN));

	wait = video_rtt_wait(VIDEO_FRAME_TIMEOUT,
	    3 * (video_rtt() + VIDEO_RTT_MARGIN));
	most = video_rtt_wait(VIDEO_FRAME_TIMEOUT_MAX,
	    4 * (video_rtt() + VIDEO_RTT_MARGIN));

	for (idx = 0; idx < VIDEO_ASSEMBLY; idx++) {
		entry = &entries[idx];

		if (entry->used == 0 || entry->done)
			continue;

		progress = entry->first;
		if (entry->slot != -1)
			progress = pool->frames[entry->slot].last;

		if ((link->now - progress) >= wait ||
		    (link->now - entry->first) >= most) {
			entry->done = 1;
			entry->lost = 1;
			continue;
		}

		if ((link->now - entry->first) >= stuck)
			want_key = 1;

		if (link->now >= entry->next_nack)
			video_rx_nack(link, entry);
	}
}

/*
 * Hand the frames that are done to the platform, oldest first. A frame
 * that is not done holds up those after it until it is.
 */
static void
video_flush(void)
{
	int			idx;
	struct video_pool	*pool;
	struct video_frame	*frame;
	struct video_entry	*entry;

	pool = vigil_atomic_read(&rx);
	VERIFY(pool != NULL);

	video_catch_up();

	for (;;) {
		entry = NULL;

		for (idx = 0; idx < VIDEO_ASSEMBLY; idx++) {
			if (entries[idx].used == 0)
				continue;

			if (entry == NULL ||
			    (int32_t)(entries[idx].id - entry->id) < 0)
				entry = &entries[idx];
		}

		if (entry == NULL || entry->done == 0)
			break;

		if (rx_last != 0 && (int32_t)(entry->id - rx_last) > 1) {
			report_lost += entry->id - rx_last - 1;
			vigil_atomic_add(&lost_frames,
			    entry->id - rx_last - 1);
			video_key_need("frames went missing");
		}

		rx_last = entry->id;

		if (entry->lost) {
			report_lost++;
			vigil_atomic_add(&lost_frames, 1);
			video_key_need("a frame did not arrive in time");
			video_rx_release(entry);
			continue;
		}

		frame = &pool->frames[entry->slot];

		if (frame->flags & VIGIL_VIDEO_FLAG_KEY) {
			if (need_key)
				vigil_log(LOG_INFO, "[video] got a keyframe");
			need_key = 0;
		}

		/* Not lost, we cannot use it until we have a keyframe. */
		if (need_key) {
			video_rx_release(entry);
			continue;
		}

		if (vigil_ring_queue(&pool->queue, entry->slot) == -1) {
			report_lost++;
			vigil_atomic_add(&lost_frames, 1);
			video_key_need("the decoder is not keeping up");
			video_rx_release(entry);
			continue;
		}

		entry->used = 0;
		report_frames++;
		vigil_atomic_add(&rx_frames, 1);
	}
}

/*
 * We cannot hand frames to the platform until we got a keyframe.
 */
static void
video_key_need(const char *why)
{
	PRECOND(why != NULL);

	if (need_key == 0)
		vigil_log(LOG_INFO, "[video] waiting for a keyframe, %s", why);

	need_key = 1;
}

/*
 * A keyframe we have in full does not need the frames before it, we give
 * up on those still coming in so it is not held up by them.
 */
static void
video_catch_up(void)
{
	int			idx;
	struct video_pool	*pool;
	struct video_entry	*key;

	pool = vigil_atomic_read(&rx);
	VERIFY(pool != NULL);

	key = NULL;

	for (idx = 0; idx < VIDEO_ASSEMBLY; idx++) {
		if (entries[idx].used == 0 || entries[idx].done == 0)
			continue;

		if (entries[idx].lost || entries[idx].slot == -1)
			continue;

		if (!(pool->frames[entries[idx].slot].flags &
		    VIGIL_VIDEO_FLAG_KEY))
			continue;

		if (key == NULL || (int32_t)(entries[idx].id - key->id) > 0)
			key = &entries[idx];
	}

	if (key == NULL)
		return;

	for (idx = 0; idx < VIDEO_ASSEMBLY; idx++) {
		if (entries[idx].used == 0 || entries[idx].done)
			continue;

		if ((int32_t)(entries[idx].id - key->id) >= 0)
			continue;

		entries[idx].done = 1;
		entries[idx].lost = 1;
	}
}

/*
 * Returns the entry for the frame with the given id, starting a new one
 * if we have not seen it yet. Frames between it and the newest we know of
 * get an entry too so we can ask for them, unless too many are missing.
 */
static struct video_entry *
video_entry_get(u_int32_t id, u_int64_t now)
{
	int			idx;
	u_int32_t		last, missing, gap;

	last = rx_last;

	for (idx = 0; idx < VIDEO_ASSEMBLY; idx++) {
		if (entries[idx].used == 0)
			continue;

		if (entries[idx].id == id)
			return (&entries[idx]);

		if (last == 0 || (int32_t)(entries[idx].id - last) > 0)
			last = entries[idx].id;
	}

	if (last != 0 && (int32_t)(id - last) > 1) {
		missing = id - last - 1;

		if (missing <= VIDEO_GAP_MAX) {
			for (gap = 1; gap <= missing; gap++) {
				if (video_entry_new(last + gap, now) == NULL)
					break;
			}
		}
	}

	return (video_entry_new(id, now));
}

/*
 * Start an entry for the frame with the given id. If we are out of entries
 * we give up on the oldest frame we are waiting on.
 */
static struct video_entry *
video_entry_new(u_int32_t id, u_int64_t now)
{
	int			idx;
	struct video_entry	*entry, *oldest;

	entry = NULL;
	oldest = NULL;

	for (idx = 0; idx < VIDEO_ASSEMBLY; idx++) {
		if (entries[idx].used == 0) {
			entry = &entries[idx];
			break;
		}

		if (entries[idx].done)
			continue;

		if (oldest == NULL ||
		    (int32_t)(entries[idx].id - oldest->id) < 0)
			oldest = &entries[idx];
	}

	if (entry == NULL) {
		if (oldest == NULL)
			return (NULL);

		oldest->done = 1;
		oldest->lost = 1;
		video_flush();

		/* The flush may have moved stuff past what we wanted. */
		if (rx_last != 0 && (int32_t)(id - rx_last) <= 0)
			return (NULL);

		for (idx = 0; idx < VIDEO_ASSEMBLY; idx++) {
			if (entries[idx].used == 0) {
				entry = &entries[idx];
				break;
			}
		}

		if (entry == NULL)
			return (NULL);
	}

	memset(entry, 0, sizeof(*entry));

	entry->id = id;
	entry->used = 1;
	entry->slot = -1;
	entry->first = now;
	entry->next_nack = now +
	    video_rtt_wait(VIDEO_TAIL_WAIT, video_rtt() / 8);

	return (entry);
}

/*
 * Put the given fragment into the frame of the given entry, returns -1 if
 * it does not belong there or we have it already.
 */
static int
video_rx_fragment(struct video_entry *entry, const struct vigil_video_hdr *hdr,
    const u_int8_t *data, u_int64_t now)
{
	struct video_pool	*pool;
	struct video_frame	*frame;

	PRECOND(entry != NULL);
	PRECOND(hdr != NULL);
	PRECOND(data != NULL);

	pool = vigil_atomic_read(&rx);
	VERIFY(pool != NULL);

	if (video_rx_slot(entry, hdr) == -1)
		return (-1);

	frame = &pool->frames[entry->slot];

	if (frame->seen[hdr->fragment / 8] & (1 << (hdr->fragment % 8)))
		return (-1);

	frame->seen[hdr->fragment / 8] |= 1 << (hdr->fragment % 8);
	memcpy(frame->data + hdr->fragment * frame->fragsize, data,
	    hdr->length);

	frame->got++;
	frame->last = now;

	if (hdr->fragment > frame->highest)
		frame->highest = hdr->fragment;

	if (frame->got == frame->count)
		entry->done = 1;
	else if (frame->fec != 0)
		video_rx_recover(entry, hdr->fragment / frame->fec);

	return (0);
}

/*
 * Give the entry a frame to put its fragments in if it has none yet,
 * returns -1 if we are out of frames or the fragment does not belong.
 */
static int
video_rx_slot(struct video_entry *entry, const struct vigil_video_hdr *hdr)
{
	u_int32_t		idx;
	struct video_pool	*pool;
	struct video_frame	*frame;

	PRECOND(entry != NULL);
	PRECOND(hdr != NULL);

	pool = vigil_atomic_read(&rx);
	VERIFY(pool != NULL);

	if (entry->slot == -1) {
		if (vigil_ring_dequeue(&pool->free, &idx) == -1) {
			entry->done = 1;
			entry->lost = 1;
			return (-1);
		}

		VERIFY(idx < VIDEO_FRAMES);

		frame = &pool->frames[idx];
		VERIFY(frame->data == NULL);
		VERIFY(frame->parity == NULL);

		frame->data = malloc(hdr->size);
		if (frame->data == NULL)
			fatal("malloc: failed to allocate video frame");

		frame->got = 0;
		frame->fec = 0;
		frame->highest = 0;
		frame->id = hdr->frame;
		frame->size = hdr->size;
		frame->flags = hdr->flags;
		frame->fragsize = hdr->fragsize;
		frame->count = (hdr->size + hdr->fragsize - 1) / hdr->fragsize;
		memset(frame->seen, 0, sizeof(frame->seen));
		memset(frame->asked, 0, sizeof(frame->asked));
		memset(frame->pseen, 0, sizeof(frame->pseen));

		entry->slot = idx;
	}

	frame = &pool->frames[entry->slot];

	if (frame->size != hdr->size || frame->fragsize != hdr->fragsize)
		return (-1);

	return (0);
}

/*
 * Keep the given parity fragment for the frame of the given entry and
 * see if it lets us rebuild a fragment we lost, returns -1 if it does
 * not belong there or we have it already.
 */
static int
video_rx_parity(struct video_entry *entry, const struct vigil_video_hdr *hdr,
    const u_int8_t *data, u_int64_t now)
{
	u_int16_t		group, groups;
	struct video_pool	*pool;
	struct video_frame	*frame;

	PRECOND(entry != NULL);
	PRECOND(hdr != NULL);
	PRECOND(data != NULL);

	pool = vigil_atomic_read(&rx);
	VERIFY(pool != NULL);

	if (video_rx_slot(entry, hdr) == -1)
		return (-1);

	frame = &pool->frames[entry->slot];

	if (frame->fec == 0)
		frame->fec = hdr->length;

	if (frame->fec != hdr->length)
		return (-1);

	groups = (frame->count + frame->fec - 1) / frame->fec;
	group = hdr->fragment - frame->count;

	if (frame->parity == NULL) {
		frame->parity = calloc(groups, frame->fragsize);
		if (frame->parity == NULL)
			fatal("calloc: failed to allocate video parity");
	}

	if (frame->pseen[group / 8] & (1 << (group % 8)))
		return (-1);

	frame->pseen[group / 8] |= 1 << (group % 8);
	memcpy(frame->parity + group * frame->fragsize, data, frame->fragsize);

	frame->last = now;
	video_rx_recover(entry, group);

	return (0);
}

/*
 * If the given group of fragments of the frame of the given entry has
 * its parity and misses one fragment, rebuild that fragment from the
 * parity and the others.
 */
static void
video_rx_recover(struct video_entry *entry, u_int16_t group)
{
	struct video_pool	*pool;
	struct video_frame	*frame;
	u_int32_t		idx, first, last, lost, missing;
	u_int32_t		offset, length, byte;

	PRECOND(entry != NULL);
	PRECOND(entry->slot != -1);

	pool = vigil_atomic_read(&rx);
	VERIFY(pool != NULL);

	frame = &pool->frames[entry->slot];
	VERIFY(frame->fec != 0);

	if (frame->parity == NULL)
		return;

	if (!(frame->pseen[group / 8] & (1 << (group % 8))))
		return;

	first = group * frame->fec;
	last = first + frame->fec;

	if (last > frame->count)
		last = frame->count;

	lost = 0;
	missing = 0;

	for (idx = first; idx < last; idx++) {
		if (frame->seen[idx / 8] & (1 << (idx % 8)))
			continue;

		missing = idx;
		lost++;
	}

	if (lost != 1)
		return;

	memcpy(rebuild, frame->parity + group * frame->fragsize,
	    frame->fragsize);

	for (idx = first; idx < last; idx++) {
		if (idx == missing)
			continue;

		offset = idx * frame->fragsize;
		length = frame->size - offset;

		if (length > frame->fragsize)
			length = frame->fragsize;

		for (byte = 0; byte < length; byte++)
			rebuild[byte] ^= frame->data[offset + byte];
	}

	offset = missing * frame->fragsize;
	length = frame->size - offset;

	if (length > frame->fragsize)
		length = frame->fragsize;

	memcpy(frame->data + offset, rebuild, length);

	frame->seen[missing / 8] |= 1 << (missing % 8);
	frame->got++;

	/* We still lost it, our peer should slow down all the same. */
	report_nacks++;

	if (frame->got == frame->count)
		entry->done = 1;
}

/*
 * Ask our peer for the fragments of the frame of the given entry we miss,
 * all of it if we have not seen any. Fragments after the last one we got
 * only once we waited a bit for them.
 */
static void
video_rx_nack(struct vigil_video_link *link, struct video_entry *entry)
{
	struct kyrka_packet		pkt;
	size_t				max;
	struct vigil_video_nack		nack;
	struct vigil_voice_hdr		*hdr;
	u_int8_t			*ptr;
	struct video_pool		*pool;
	struct video_frame		*frame;
	u_int16_t			idx, end, count, fresh;

	PRECOND(link != NULL);
	PRECOND(entry != NULL);

	pool = vigil_atomic_read(&rx);
	VERIFY(pool != NULL);

	count = 0;
	fresh = 0;
	memset(&nack, 0, sizeof(nack));

	if (entry->slot != -1) {
		frame = &pool->frames[entry->slot];

		if ((link->now - frame->last) >=
		    video_rtt_wait(VIDEO_TAIL_WAIT, video_rtt() / 8))
			end = frame->count;
		else
			end = frame->highest;

		for (idx = 0; idx < end && count < VIGIL_VIDEO_NACK_MAX;
		    idx++) {
			if (frame->seen[idx / 8] & (1 << (idx % 8)))
				continue;

			if (!(frame->asked[idx / 8] & (1 << (idx % 8)))) {
				frame->asked[idx / 8] |= 1 << (idx % 8);
				fresh++;
			}

			nack.fragments[count++] = htons(idx);
		}

		if (count == 0)
			return;
	} else if (entry->asked == 0) {
		fresh = 1;
	}

	/* We report what we missed, not how often we asked for it. */
	entry->asked = 1;
	entry->next_nack = link->now +
	    video_rtt_wait(VIDEO_NACK_INTERVAL, video_rtt() + VIDEO_RTT_MARGIN);
	report_nacks += fresh;

	nack.frame = htonl(entry->id);
	nack.count = htons(count);

	ptr = kyrka_packet_databuf(link->ctx, &pkt, &max);
	if (ptr == NULL)
		fatal("failed to get video nack buffer");

	if (max < VIGIL_VOICE_PAYLOAD)
		fatal("voice payload does not fit");

	memset(ptr, 0, VIGIL_VOICE_PAYLOAD);

	hdr = (struct vigil_voice_hdr *)ptr;
	hdr->type = VIGIL_VOICE_TYPE_VIDEO_NACK;
	hdr->length = htons(sizeof(nack));

	memcpy(ptr + sizeof(*hdr), &nack, sizeof(nack));
	pkt.length = VIGIL_VOICE_PAYLOAD;
	pkt.shroud = link->shroud;

	if (kyrka_heaven_input(link->ctx, &pkt) == -1 &&
	    kyrka_last_error(link->ctx) != KYRKA_ERROR_NO_TX_KEY) {
		vigil_log(LOG_NOTICE, "[video] kyrka_heaven_input: %d",
		    kyrka_last_error(link->ctx));
	}
}

/*
 * The round trip to our peer, the longest of what we and it measured,
 * 0 if neither of us did.
 */
static u_int64_t
video_rtt(void)
{
	if (peer_rtt > rtt)
		return (peer_rtt);

	return (rtt);
}

/*
 * Our peer asks for fragments of the given frame. The first time it does
 * the time since we sent the frame is about a round trip, its tail wait
 * aside.
 */
static void
video_rtt_sample(u_int32_t id, u_int64_t now)
{
	u_int64_t		tail, sample;
	struct video_sent	*s;

	s = &stamps[id & (VIDEO_SENT - 1)];

	if (s->id != id)
		return;

	if (s->nacked)
		return;

	s->nacked = 1;
	tail = video_rtt_wait(VIDEO_TAIL_WAIT, video_rtt() / 8);

	if (now - s->when <= tail)
		return;

	sample = now - s->when - tail;

	rtt_last = sample;

	if (rtt_min == 0 || sample < rtt_min)
		rtt_min = sample;

	if (rtt == 0)
		rtt = sample;
	else
		rtt = (rtt * 7 + sample) / 8;
}

/*
 * Returns the given milliseconds, or the scaled ones if those are longer.
 */
static u_int64_t
video_rtt_wait(u_int64_t least, u_int64_t scaled)
{
	if (scaled > least)
		return (scaled);

	return (least);
}

/*
 * Release the given entry and the frame it held, if any.
 */
static void
video_rx_release(struct video_entry *entry)
{
	struct video_pool	*pool;

	PRECOND(entry != NULL);
	PRECOND(entry->used);

	pool = vigil_atomic_read(&rx);
	VERIFY(pool != NULL);

	if (entry->slot != -1) {
		free(pool->frames[entry->slot].data);
		pool->frames[entry->slot].data = NULL;

		free(pool->frames[entry->slot].parity);
		pool->frames[entry->slot].parity = NULL;

		if (vigil_ring_queue(&pool->free, entry->slot) == -1)
			fatal("video rx free overflow");
	}

	entry->used = 0;
}

/*
 * Allocate the frames for a direction, all of them free.
 */
static struct video_pool *
video_pool_alloc(void)
{
	u_int32_t		idx;
	struct video_pool	*pool;

	pool = calloc(1, sizeof(*pool));
	if (pool == NULL)
		fatal("calloc: failed to allocate video frames");

	vigil_ring_init(&pool->free, VIDEO_FRAMES);
	vigil_ring_init(&pool->queue, VIDEO_FRAMES);

	for (idx = 0; idx < VIDEO_FRAMES; idx++) {
		if (vigil_ring_queue(&pool->free, idx) == -1)
			fatal("video free overflow");
	}

	return (pool);
}

/*
 * Release the frames of a direction, nothing may use them anymore.
 */
static void
video_pool_free(struct video_pool *pool)
{
	u_int32_t	idx;

	if (pool == NULL)
		return;

	for (idx = 0; idx < VIDEO_FRAMES; idx++) {
		free(pool->frames[idx].data);
		free(pool->frames[idx].parity);
	}

	free(pool);
}
