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

/*
 * A video call between two forked children over a path we simulate, with
 * a delay, jitter, loss and a bottleneck that queues and drops like the
 * cathedrals do under commixtion. Time is simulated too, we step it for
 * both children and libkyrka sees it through our own clock_gettime().
 *
 * Each child encodes frames at the bitrate the video asks for and counts
 * how long its picture froze, we check that a short path and a long one
 * both give a usable call.
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include <inttypes.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include "vigil.h"

/* The milliseconds we step time with, and how long a call lasts. */
#define TEST_TICK		5
#define TEST_CALL		40000

/* The frames per second we encode, every how many a keyframe, its size. */
#define TEST_FRAME_INTERVAL	33
#define TEST_KEY_FRAMES		60
#define TEST_KEY_SCALE		6

/* A picture that does not change for longer than this froze. */
#define TEST_FREEZE		200

/* The mtu of the tunnel and how often we send a voice packet. */
#define TEST_MTU		1300
#define TEST_VOICE_INTERVAL	20

/* The packets we hold on the path in each direction. */
#define TEST_QUEUE		16384

/* The most bytes one tick of packets takes. */
#define TEST_BATCH		(4 * 1024 * 1024)

/* Tells a child the call is over. */
#define TEST_QUIT		UINT64_MAX

/*
 * One direction of the path, and how long the picture may freeze over it
 * in total and at once.
 */
struct test_path {
	const char	*name;
	u_int32_t	delay;
	u_int32_t	jitter;
	u_int32_t	loss;
	u_int32_t	bps;
	u_int32_t	queue;
	u_int32_t	freeze;
	u_int32_t	longest;
};

/*
 * A packet on its way.
 */
struct test_packet {
	u_int64_t	due;
	u_int16_t	len;
	u_int8_t	data[KYRKA_PACKET_MAX_LEN];
};

/*
 * A direction of the path with the packets on it.
 */
struct test_wire {
	const struct test_path	*path;
	u_int64_t		busy;
	u_int64_t		last;
	u_int32_t		head;
	u_int32_t		tail;
	u_int64_t		dropped;
	u_int64_t		queued;
	struct test_packet	packets[TEST_QUEUE];
};

/*
 * What a child tells us when the call is over.
 */
struct test_stats {
	u_int64_t	decoded;
	u_int64_t	encoded;
	u_int64_t	keys;
	u_int64_t	freeze;
	u_int64_t	longest;
	u_int64_t	first;
	u_int32_t	rate;
	u_int32_t	rate_min;
	u_int64_t	dropped;
	u_int64_t	queued;
	char		video[256];
};

static int	test_call(const struct test_path *, struct test_stats *);
static void	test_child(int);
static void	test_wire_put(struct test_wire *, u_int64_t,
		    const u_int8_t *, u_int16_t);
static size_t	test_wire_take(struct test_wire *, u_int64_t,
		    u_int8_t *, size_t, u_int32_t *);
static void	test_send(int, const u_int8_t *, size_t);
static size_t	test_recv(int, u_int8_t *, size_t);
static u_int32_t	test_random(void);

static void	child_tick(u_int64_t);
static void	child_frame(void);
static void	child_decode(void);
static void	child_event(KYRKA *, union kyrka_event *, void *);
static void	child_plaintext(struct kyrka_packet *, u_int64_t, void *);
static void	child_ciphertext(struct kyrka_packet *, u_int64_t, void *);

extern int	host_quiet;

static const struct test_path	paths[] = {
	{ "direct", 20, 5, 2, 8 * 1000 * 1000, 200, 1000, 500 },
	{ "commixtion", 300, 40, 10, 3 * 1000 * 1000, 300, 4000, 2500 },
	{ "commixtion-far", 450, 60, 20, 2 * 1000 * 1000, 400, 8000, 4000 },
};

static u_int32_t		seed = 0x5a5a1234;
static u_int8_t			batch[TEST_BATCH];
static u_int8_t			reply[TEST_BATCH];

/* The child its state. */
static KYRKA			*ctx = NULL;
static volatile u_int64_t	sim = 0;
static struct vigil_video_link	vlink;
static struct test_stats	stats;
static u_int8_t			*out = NULL;
static size_t			outlen = 0;
static size_t			outcount = 0;
static int			encoding = 0;
static int			keyframe = 0;
static u_int32_t		bitrate = 0;
static u_int32_t		since = TEST_KEY_FRAMES;
static u_int32_t		seq = 0;
static u_int64_t		next_frame = 0;
static u_int64_t		next_voice = 0;
static u_int64_t		last_decoded = 0;
static u_int8_t			*frame = NULL;

int
main(void)
{
	size_t			idx;
	int			fail;
	struct test_stats	st[2];

	host_quiet = getenv("VIGIL_TEST_VERBOSE") == NULL;
	fail = 0;

	for (idx = 0; idx < sizeof(paths) / sizeof(paths[0]); idx++) {
		if (test_call(&paths[idx], st) == -1)
			fatal("call over %s failed", paths[idx].name);

		printf("video_path_test: %-15s decoded %4" PRIu64 "/%4" PRIu64
		    " keys %3" PRIu64 " froze %5" PRIu64 "ms longest %5"
		    PRIu64 "ms rate %4u..%4uk\n", paths[idx].name,
		    st[0].decoded, st[1].encoded, st[1].keys, st[0].freeze,
		    st[0].longest, st[1].rate_min / 1000, st[1].rate / 1000);

		if (getenv("VIGIL_TEST_VERBOSE") != NULL) {
			printf("    a %s\n    b %s\n", st[0].video,
			    st[1].video);
			printf("    to a: %" PRIu64 " lost, %" PRIu64
			    " dropped at the bottleneck\n", st[0].dropped,
			    st[0].queued);
		}

		if (st[0].freeze > paths[idx].freeze) {
			printf("FAIL the picture froze too long over %s\n",
			    paths[idx].name);
			fail = 1;
		}

		if (st[0].longest > paths[idx].longest) {
			printf("FAIL the picture froze too long at once "
			    "over %s\n", paths[idx].name);
			fail = 1;
		}

		if (st[0].decoded < st[1].encoded * 8 / 10) {
			printf("FAIL too few frames decoded over %s\n",
			    paths[idx].name);
			fail = 1;
		}

		if (st[1].keys > TEST_CALL / 1000) {
			printf("FAIL too many keyframes over %s\n",
			    paths[idx].name);
			fail = 1;
		}
	}

	if (fail)
		return (1);

	printf("video_path_test: ok\n");

	return (0);
}

/*
 * Run a call between two children over the given path, stats for the
 * one that receives (a) and the one that sends to it (b) end up in st.
 */
static int
test_call(const struct test_path *path, struct test_stats *st)
{
	u_int64_t		now;
	int			fds[2][2];
	pid_t			pid[2];
	struct test_wire	*wire[2];
	u_int8_t		*ptr;
	size_t			idx, len, off;
	u_int32_t		count, pkt;
	u_int16_t		plen;

	for (idx = 0; idx < 2; idx++) {
		if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds[idx]) == -1)
			fatal("socketpair: %s", errno_s);

		if ((wire[idx] = calloc(1, sizeof(*wire[idx]))) == NULL)
			fatal("calloc");

		wire[idx]->path = path;

		(void)fflush(stdout);

		if ((pid[idx] = fork()) == -1)
			fatal("fork: %s", errno_s);

		if (pid[idx] == 0) {
			(void)close(fds[idx][0]);
			test_child(fds[idx][1]);
			exit(0);
		}

		(void)close(fds[idx][1]);
	}

	for (now = 0; now <= TEST_CALL; now += TEST_TICK) {
		for (idx = 0; idx < 2; idx++) {
			memcpy(batch, &now, sizeof(now));
			off = sizeof(now) + sizeof(count);
			off += test_wire_take(wire[idx], now, &batch[off],
			    sizeof(batch) - off, &count);
			memcpy(&batch[sizeof(now)], &count, sizeof(count));

			test_send(fds[idx][0], batch, off);
			len = test_recv(fds[idx][0], reply, sizeof(reply));

			if (len < sizeof(count))
				fatal("short reply");

			memcpy(&count, reply, sizeof(count));
			ptr = reply + sizeof(count);

			for (pkt = 0; pkt < count; pkt++) {
				memcpy(&plen, ptr, sizeof(plen));
				ptr += sizeof(plen);
				test_wire_put(wire[idx ^ 1], now, ptr, plen);
				ptr += plen;
			}
		}
	}

	now = TEST_QUIT;

	for (idx = 0; idx < 2; idx++) {
		memcpy(batch, &now, sizeof(now));
		test_send(fds[idx][0], batch, sizeof(now));

		len = test_recv(fds[idx][0], reply, sizeof(reply));
		if (len != sizeof(st[idx]))
			fatal("short stats");

		memcpy(&st[idx], reply, sizeof(st[idx]));
		st[idx].dropped = wire[idx]->dropped;
		st[idx].queued = wire[idx]->queued;

		if (waitpid(pid[idx], NULL, 0) == -1)
			fatal("waitpid: %s", errno_s);

		(void)close(fds[idx][0]);
		free(wire[idx]);
	}

	return (0);
}

/*
 * Put a packet on the wire, it waits its turn at the bottleneck and is
 * dropped if that queue is too long or at random for the loss we have.
 * Packets never overtake each other, a path under commixtion is stable.
 */
static void
test_wire_put(struct test_wire *wire, u_int64_t now, const u_int8_t *data,
    u_int16_t len)
{
	u_int64_t		start, due;
	struct test_packet	*pkt;

	if (wire->tail - wire->head == TEST_QUEUE)
		fatal("wire overflow");

	start = wire->busy > now ? wire->busy : now;

	if (start - now > wire->path->queue) {
		wire->queued++;
		return;
	}

	wire->busy = start + ((u_int64_t)len * 8 * 1000) / wire->path->bps;

	if (test_random() % 1000 < wire->path->loss) {
		wire->dropped++;
		return;
	}

	due = wire->busy + wire->path->delay;
	if (wire->path->jitter > 0)
		due += test_random() % wire->path->jitter;

	if (due < wire->last)
		due = wire->last;

	wire->last = due;

	pkt = &wire->packets[wire->tail++ % TEST_QUEUE];
	pkt->due = due;
	pkt->len = len;
	memcpy(pkt->data, data, len);
}

/*
 * Take the packets that arrived by now into buf, returns their length.
 */
static size_t
test_wire_take(struct test_wire *wire, u_int64_t now, u_int8_t *buf,
    size_t len, u_int32_t *count)
{
	size_t			off;
	struct test_packet	*pkt;

	off = 0;
	*count = 0;

	while (wire->head != wire->tail) {
		pkt = &wire->packets[wire->head % TEST_QUEUE];

		if (pkt->due > now)
			break;

		if (len - off < sizeof(pkt->len) + pkt->len)
			fatal("batch overflow");

		memcpy(&buf[off], &pkt->len, sizeof(pkt->len));
		memcpy(&buf[off + sizeof(pkt->len)], pkt->data, pkt->len);
		off += sizeof(pkt->len) + pkt->len;

		wire->head++;
		(*count)++;
	}

	return (off);
}

/*
 * A child, one end of the call.
 */
static void
test_child(int fd)
{
	struct kyrka_packet	kp;
	u_int64_t		now;
	u_int16_t		plen;
	u_int8_t		*ptr, *ct;
	u_int32_t		count, pkt;
	size_t			max;
	u_int8_t		secret[32];

	memset(secret, 0x42, sizeof(secret));

	if ((out = malloc(TEST_BATCH)) == NULL)
		fatal("malloc");

	if ((frame = calloc(1, VIGIL_VIDEO_FRAME_MAX)) == NULL)
		fatal("calloc");

	if ((ctx = kyrka_ctx_alloc(child_event, NULL)) == NULL)
		fatal("kyrka_ctx_alloc");

	if (kyrka_heaven_ifc(ctx, child_plaintext, NULL) == -1)
		fatal("kyrka_heaven_ifc");

	if (kyrka_purgatory_ifc(ctx, child_ciphertext, NULL) == -1)
		fatal("kyrka_purgatory_ifc");

	if (kyrka_secret_load(ctx, secret, sizeof(secret)) == -1)
		fatal("kyrka_secret_load");

	memset(&stats, 0, sizeof(stats));
	stats.rate_min = UINT32_MAX;

	vigil_video_open();
	vigil_video_camera(1);

	memset(&vlink, 0, sizeof(vlink));
	vlink.ctx = ctx;
	vlink.mtu = TEST_MTU;
	vlink.live = 1;

	for (;;) {
		(void)test_recv(fd, batch, sizeof(batch));
		memcpy(&now, batch, sizeof(now));

		if (now == TEST_QUIT)
			break;

		sim = now;
		vlink.now = now;
		outlen = sizeof(count);
		outcount = 0;

		memcpy(&count, batch + sizeof(now), sizeof(count));
		ptr = batch + sizeof(now) + sizeof(count);

		for (pkt = 0; pkt < count; pkt++) {
			memcpy(&plen, ptr, sizeof(plen));
			ptr += sizeof(plen);

			if ((ct = kyrka_packet_recvbuf(ctx, &kp, &max)) == NULL)
				fatal("kyrka_packet_recvbuf");

			if (plen > max)
				fatal("ciphertext too large");

			memcpy(ct, ptr, plen);
			kp.length = plen;
			kp.shroud = KYRKA_PACKET_SHROUD_PEER;
			(void)kyrka_purgatory_input(ctx, &kp);

			ptr += plen;
		}

		child_tick(now);

		count = outcount;
		memcpy(out, &count, sizeof(count));
		test_send(fd, out, outlen);
	}

	(void)vigil_video_status(stats.video, sizeof(stats.video));
	test_send(fd, (const u_int8_t *)&stats, sizeof(stats));

	vigil_video_close();
	kyrka_ctx_free(ctx);
}

/*
 * One step of time for a child, encode, send, decode.
 */
static void
child_tick(u_int64_t now)
{
	struct kyrka_packet	pkt;
	size_t			max;
	u_int8_t		*ptr;

	(void)kyrka_key_manage(ctx);

	if (vlink.talking && now >= next_voice) {
		next_voice = now + TEST_VOICE_INTERVAL;

		if ((ptr = kyrka_packet_databuf(ctx, &pkt, &max)) == NULL)
			fatal("kyrka_packet_databuf");

		memset(ptr, 0, VIGIL_VOICE_PAYLOAD);
		ptr[0] = VIGIL_VOICE_TYPE_VOICE;
		pkt.length = VIGIL_VOICE_PAYLOAD;
		pkt.shroud = 0;
		(void)kyrka_heaven_input(ctx, &pkt);
	}

	if (encoding && now >= next_frame) {
		next_frame = now + TEST_FRAME_INTERVAL;
		child_frame();
	}

	vigil_video_update(&vlink);
	child_decode();

	if (vlink.talking && bitrate != 0) {
		stats.rate = bitrate;
		if (bitrate < stats.rate_min)
			stats.rate_min = bitrate;
	}
}

/*
 * Encode a frame at the bitrate we were given, a keyframe when asked or
 * every TEST_KEY_FRAMES.
 */
static void
child_frame(void)
{
	int		flags;
	u_int32_t	len;

	len = bitrate / 8 / (1000 / TEST_FRAME_INTERVAL);

	if (since >= TEST_KEY_FRAMES || keyframe) {
		keyframe = 0;
		since = 0;
		flags = VIGIL_VIDEO_FLAG_KEY;
		len *= TEST_KEY_SCALE;
	} else {
		flags = 0;
	}

	if (len < sizeof(seq))
		len = sizeof(seq);

	if (len > VIGIL_VIDEO_FRAME_MAX)
		len = VIGIL_VIDEO_FRAME_MAX;

	seq++;
	memcpy(frame, &seq, sizeof(seq));

	if (vigil_video_input(frame, len, flags) == -1) {
		since = TEST_KEY_FRAMES;
		return;
	}

	since++;
	stats.encoded++;

	if (flags & VIGIL_VIDEO_FLAG_KEY)
		stats.keys++;
}

/*
 * Take the frames the video has for us and see how long the picture
 * stood still in between.
 */
static void
child_decode(void)
{
	int		flags;
	u_int64_t	gap;

	while (vigil_video_take(frame, VIGIL_VIDEO_FRAME_MAX, &flags) > 0) {
		if (last_decoded == 0) {
			stats.first = sim;
		} else {
			gap = sim - last_decoded;

			if (gap > TEST_FREEZE)
				stats.freeze += gap;

			if (gap > 1000 && !host_quiet) {
				printf("    froze %" PRIu64 "ms until %" PRIu64
				    "\n", gap, sim);
			}

			if (gap > stats.longest)
				stats.longest = gap;
		}

		last_decoded = sim;
		stats.decoded++;
	}
}

/*
 * The call starts once we have keys in both directions.
 */
static void
child_event(KYRKA *kctx, union kyrka_event *evt, void *udata)
{
	if (evt->type != KYRKA_EVENT_KEYS_INFO)
		return;

	if (evt->keys.tx_spi != 0 && evt->keys.rx_spi != 0) {
		vlink.online = 1;
		vlink.talking = 1;
	}
}

/*
 * A packet from our peer, handed to the video like the voice session does.
 */
static void
child_plaintext(struct kyrka_packet *pkt, u_int64_t pn, void *udata)
{
	struct vigil_video_ctl		ctl;
	struct vigil_video_nack		nack;
	const struct vigil_voice_hdr	*hdr;
	u_int8_t			*data;

	if (pkt->type != KYRKA_PACKET_TYPE_IP)
		return;

	data = kyrka_packet_data(pkt);

	if (pkt->length > VIGIL_VOICE_PAYLOAD &&
	    data[0] == VIGIL_VOICE_TYPE_VIDEO) {
		vigil_video_recv(&vlink, data, pkt->length);
		return;
	}

	if (pkt->length != VIGIL_VOICE_PAYLOAD)
		return;

	hdr = (const struct vigil_voice_hdr *)data;

	if (hdr->type == VIGIL_VOICE_TYPE_VIDEO_CTL &&
	    ntohs(hdr->length) == sizeof(ctl)) {
		memcpy(&ctl, data + sizeof(*hdr), sizeof(ctl));
		vigil_video_ctl(&vlink, &ctl);
		return;
	}

	if (hdr->type == VIGIL_VOICE_TYPE_VIDEO_NACK &&
	    ntohs(hdr->length) == sizeof(nack)) {
		memcpy(&nack, data + sizeof(*hdr), sizeof(nack));
		vigil_video_nack(&vlink, &nack);
	}
}

/*
 * Ciphertext for our peer, it goes to the parent with this tick.
 */
static void
child_ciphertext(struct kyrka_packet *pkt, u_int64_t seqnr, void *udata)
{
	size_t		len;
	u_int16_t	plen;
	u_int8_t	*ptr;

	if ((ptr = kyrka_packet_sendbuf(ctx, pkt, &len)) == NULL)
		fatal("kyrka_packet_sendbuf");

	if (TEST_BATCH - outlen < sizeof(plen) + len)
		fatal("child batch overflow");

	plen = len;
	memcpy(&out[outlen], &plen, sizeof(plen));
	memcpy(&out[outlen + sizeof(plen)], ptr, len);

	outlen += sizeof(plen) + len;
	outcount++;
}

/*
 * The platform video, driven by child_tick() instead of threads.
 */
int
vigil_platform_video_start(void)
{
	return (0);
}

void
vigil_platform_video_stop(void)
{
	encoding = 0;
}

void
vigil_platform_video_camera(int on)
{
	encoding = on;
}

void
vigil_platform_video_keyframe(void)
{
	keyframe = 1;
}

void
vigil_platform_video_bitrate(u_int32_t bps)
{
	bitrate = bps;
}

/*
 * Our clock, the simulated time, for libkyrka and the core alike.
 */
int
clock_gettime(clockid_t clk, struct timespec *ts)
{
	ts->tv_sec = 1000000 + sim / 1000;
	ts->tv_nsec = (sim % 1000) * 1000000;

	return (0);
}

/*
 * A deterministic random number, so each run sees the same path.
 */
static u_int32_t
test_random(void)
{
	seed ^= seed << 13;
	seed ^= seed >> 17;
	seed ^= seed << 5;

	return (seed);
}

/*
 * Send a frame, preceded by its length.
 */
static void
test_send(int fd, const u_int8_t *data, size_t len)
{
	ssize_t		ret;
	u_int32_t	flen;
	size_t		off;

	flen = len;
	if (write(fd, &flen, sizeof(flen)) != sizeof(flen))
		fatal("write: %s", errno_s);

	for (off = 0; off < len; off += ret) {
		if ((ret = write(fd, &data[off], len - off)) <= 0)
			fatal("write: %s", errno_s);
	}
}

/*
 * Receive a frame, returns its length.
 */
static size_t
test_recv(int fd, u_int8_t *data, size_t max)
{
	ssize_t		ret;
	u_int32_t	flen;
	size_t		off, len;

	for (off = 0; off < sizeof(flen); off += ret) {
		if ((ret = read(fd, (u_int8_t *)&flen + off,
		    sizeof(flen) - off)) <= 0)
			fatal("read: %s", errno_s);
	}

	len = flen;
	if (len > max)
		fatal("bad frame length %zu", len);

	for (off = 0; off < len; off += ret) {
		if ((ret = read(fd, &data[off], len - off)) <= 0)
			fatal("read: %s", errno_s);
	}

	return (len);
}
