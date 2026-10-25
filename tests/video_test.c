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
 * Tests for putting video frames back together. We first throw garbage
 * at it, then frames of the pattern src/video_host.c checks with their
 * fragments shuffled, duplicated, dropped, resent late and mixed with
 * fragments that lie about the frame. The decoder may never see a frame
 * that is not what was sent, or one that is out of order.
 */

#include <sys/types.h>

#include <arpa/inet.h>

#include <inttypes.h>
#include <poll.h>
#include <stdio.h>

#include "vigil.h"

/* The frames we send in the second part, and how often a keyframe. */
#define TEST_FRAMES		900
#define TEST_KEY_FRAMES		30

/* The garbage we send in the first part. */
#define TEST_GARBAGE		200000

/* The front of each frame, as src/video_host.c fills it. */
#define TEST_HEAD_LEN		8

static void	test_link(struct vigil_video_link *, KYRKA *);
static void	test_garbage(struct vigil_video_link *);
static void	test_frames(struct vigil_video_link *);
static void	test_fill(u_int8_t *, u_int32_t, u_int32_t);
static void	test_event(KYRKA *, union kyrka_event *, void *);
static void	test_fragment(struct vigil_video_link *, u_int32_t,
		    const u_int8_t *, u_int32_t, u_int16_t, u_int16_t, int);

/* Set by src/video_host.c. */
extern int			host_quiet;
extern volatile u_int64_t	host_video_ok;
extern volatile u_int64_t	host_video_bad;

int
main(void)
{
	KYRKA				*ctx;
	struct vigil_video_link		link;

	host_quiet = getenv("VIGIL_TEST_VERBOSE") == NULL;

	ctx = kyrka_ctx_alloc(test_event, NULL);
	if (ctx == NULL)
		fatal("kyrka_ctx_alloc");

	test_link(&link, ctx);
	test_garbage(&link);

	vigil_atomic_write(&host_video_ok, 0);
	vigil_atomic_write(&host_video_bad, 0);

	test_link(&link, ctx);
	test_frames(&link);

	kyrka_ctx_free(ctx);

	printf("video_test: %" PRIu64 " frames decoded, %" PRIu64 " bad\n",
	    vigil_atomic_read(&host_video_ok),
	    vigil_atomic_read(&host_video_bad));

	if (vigil_atomic_read(&host_video_bad) != 0) {
		printf("FAIL the decoder got a frame that was not sent\n");
		return (1);
	}

	if (vigil_atomic_read(&host_video_ok) < TEST_FRAMES / 2) {
		printf("FAIL too few frames decoded\n");
		return (1);
	}

	printf("video_test: ok\n");

	return (0);
}

/*
 * Start a fresh video session where our peer has its camera on and we
 * do not, so only the receiving side runs.
 */
static void
test_link(struct vigil_video_link *link, KYRKA *ctx)
{
	struct vigil_video_ctl		ctl;

	PRECOND(link != NULL);
	PRECOND(ctx != NULL);

	vigil_video_close();
	vigil_video_open();

	memset(link, 0, sizeof(*link));

	link->ctx = ctx;
	link->online = 1;
	link->talking = 1;
	link->mtu = VIGIL_MTU_DEFAULT;
	link->now = vigil_time_ms();

	memset(&ctl, 0, sizeof(ctl));
	ctl.camera = 1;

	vigil_video_ctl(link, &ctl);
	vigil_video_update(link);
}

/*
 * Throw fragments, nacks and reports with random contents at the video,
 * only to see that nothing breaks.
 */
static void
test_garbage(struct vigil_video_link *link)
{
	int				idx;
	size_t				len;
	struct vigil_video_ctl		ctl;
	struct vigil_video_nack		nack;
	struct vigil_video_hdr		*hdr;
	u_int8_t			pkt[KYRKA_PACKET_DATA_LEN];

	PRECOND(link != NULL);

	for (idx = 0; idx < TEST_GARBAGE; idx++) {
		len = arc4random_uniform(sizeof(pkt) + 1);
		arc4random_buf(pkt, sizeof(pkt));

		hdr = (struct vigil_video_hdr *)pkt;

		/* Make some of it look close enough to get far. */
		if (idx % 4 == 1) {
			hdr->frame = htonl(arc4random_uniform(64));
			hdr->size = htonl(arc4random_uniform(64 * 1024));
			hdr->fragsize = htons(arc4random_uniform(1500));
			hdr->fragment = htons(arc4random_uniform(128));
			hdr->length = htons(arc4random_uniform(1500));
		}

		/* And some of it a whole frame of garbage in one fragment. */
		if (idx % 4 == 3) {
			len = sizeof(*hdr) + 1 + arc4random_uniform(1000);
			hdr->frame = htonl(idx);
			hdr->size = htonl(len - sizeof(*hdr));
			hdr->length = htons(len - sizeof(*hdr));
			hdr->fragsize = htons(len - sizeof(*hdr) +
			    arc4random_uniform(200));
			hdr->fragment = 0;
		}

		vigil_video_recv(link, pkt, len);

		if (idx % 64 == 0) {
			arc4random_buf(&nack, sizeof(nack));
			vigil_video_nack(link, &nack);

			arc4random_buf(&ctl, sizeof(ctl));
			ctl.camera = 1;
			vigil_video_ctl(link, &ctl);

			link->now += arc4random_uniform(100);
			vigil_video_update(link);
		}
	}
}

/*
 * Send frames as fragments that arrive shuffled, twice, late or never,
 * next to fragments that lie about the size of their frame.
 */
static void
test_frames(struct vigil_video_link *link)
{
	int		key, late_key;
	u_int16_t	late[64], tmp;
	u_int8_t	*frame, *late_frame;
	u_int16_t	*order, late_fragsize;
	u_int32_t	late_seq, late_count, late_len;
	u_int32_t	seq, len, fragsize, count, idx, pick;

	PRECOND(link != NULL);

	frame = calloc(1, VIGIL_VIDEO_FRAME_MAX);
	late_frame = calloc(1, VIGIL_VIDEO_FRAME_MAX);
	order = calloc(VIGIL_VIDEO_FRAME_MAX / 64, sizeof(*order));

	if (frame == NULL || late_frame == NULL || order == NULL)
		fatal("calloc");

	late_key = 0;
	late_len = 0;
	late_seq = 0;
	late_count = 0;
	late_fragsize = 0;

	for (seq = 1; seq <= TEST_FRAMES; seq++) {
		key = (seq % TEST_KEY_FRAMES) == 1;

		if (key)
			len = 20000 + arc4random_uniform(60000);
		else
			len = TEST_HEAD_LEN + arc4random_uniform(8000);

		fragsize = 600 + arc4random_uniform(VIGIL_MTU_DEFAULT -
		    sizeof(struct vigil_video_hdr) - 600 + 1);
		count = (len + fragsize - 1) / fragsize;

		test_fill(frame, seq, len);

		for (idx = 0; idx < count; idx++)
			order[idx] = idx;

		for (idx = count - 1; idx > 0; idx--) {
			pick = arc4random_uniform(idx + 1);
			tmp = order[idx];
			order[idx] = order[pick];
			order[pick] = tmp;
		}

		if (late_seq == 0 && arc4random_uniform(100) < 20) {
			late_key = key;
			late_len = len;
			late_seq = seq;
			late_fragsize = fragsize;
			memcpy(late_frame, frame, len);
		}

		for (idx = 0; idx < count; idx++) {
			/* Some go missing now and come in late as a resend. */
			if (late_seq == seq && late_count < 64 &&
			    arc4random_uniform(100) < 10) {
				late[late_count++] = order[idx];
				continue;
			}

			test_fragment(link, seq, frame, len,
			    fragsize, order[idx], key);

			if (arc4random_uniform(100) < 10) {
				test_fragment(link, seq, frame, len,
				    fragsize, order[idx], key);
			}

			/* A fragment that lies about its frame. */
			if (arc4random_uniform(100) < 2) {
				test_fragment(link, seq, frame,
				    len + fragsize, fragsize, order[idx], key);
			}
		}

		link->now += 33;
		vigil_video_update(link);

		if (late_seq != 0 && seq == late_seq + 2) {
			for (idx = 0; idx < late_count; idx++) {
				test_fragment(link, late_seq, late_frame,
				    late_len, late_fragsize, late[idx],
				    late_key);
			}

			late_seq = 0;
			late_count = 0;
		}

		(void)poll(NULL, 0, 2);
	}

	for (idx = 0; idx < 50; idx++) {
		link->now += 33;
		vigil_video_update(link);
		(void)poll(NULL, 0, 10);
	}

	vigil_video_close();

	free(frame);
	free(order);
	free(late_frame);
}

/*
 * Send a single fragment of the given frame of the given length. The frame
 * buffer may be shorter than the length if the fragment lies about it.
 */
static void
test_fragment(struct vigil_video_link *link, u_int32_t seq,
    const u_int8_t *frame, u_int32_t len, u_int16_t fragsize,
    u_int16_t fragment, int key)
{
	struct vigil_video_hdr	*hdr;
	u_int32_t		offset, length;
	u_int8_t		pkt[KYRKA_PACKET_DATA_LEN];

	PRECOND(link != NULL);
	PRECOND(frame != NULL);
	PRECOND(fragsize > 0);

	memset(pkt, 0, sizeof(pkt));
	hdr = (struct vigil_video_hdr *)pkt;

	offset = fragment * fragsize;
	if (offset >= len)
		return;

	length = len - offset;
	if (length > fragsize)
		length = fragsize;

	hdr->type = VIGIL_VOICE_TYPE_VIDEO;
	hdr->frame = htonl(seq);
	hdr->size = htonl(len);
	hdr->length = htons(length);
	hdr->fragment = htons(fragment);
	hdr->fragsize = htons(fragsize);

	if (key)
		hdr->flags = VIGIL_VIDEO_FLAG_KEY;

	if (offset + length <= VIGIL_VIDEO_FRAME_MAX)
		memcpy(pkt + sizeof(*hdr), frame + offset, length);

	vigil_video_recv(link, pkt, sizeof(*hdr) + fragsize);
}

/*
 * Fill a frame the way src/video_host.c does.
 */
static void
test_fill(u_int8_t *frame, u_int32_t seq, u_int32_t len)
{
	u_int32_t	idx;

	PRECOND(frame != NULL);

	if (len < TEST_HEAD_LEN)
		return;

	memcpy(frame, &seq, sizeof(seq));
	memcpy(frame + sizeof(seq), &len, sizeof(len));

	for (idx = TEST_HEAD_LEN; idx < len; idx++)
		frame[idx] = (seq * 31 + idx) & 0xff;
}

/*
 * We do not care about libkyrka events.
 */
static void
test_event(KYRKA *ctx, union kyrka_event *evt, void *udata)
{
	(void)ctx;
	(void)evt;
	(void)udata;
}
