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

#include <inttypes.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>

#include "vigil.h"

/*
 * Hosts have no camera for us, we encode frames of a known pattern at the
 * bitrate we are given and check every frame we get to decode against it.
 *
 * To test against a phone VIGIL_VIDEO_SOURCE names an h264 annex b file
 * with an access unit delimiter in front of each frame, we send its frames
 * instead. VIGIL_VIDEO_DUMP names a file we write the frames we get to.
 */

/* The milliseconds between frames, about 30 per second. */
#define VIDEO_FRAME_INTERVAL	33

/* The frames between keyframes. */
#define VIDEO_KEY_FRAMES	60

/* How much larger a keyframe is than other frames. */
#define VIDEO_KEY_SCALE		6

/* The milliseconds between decode reports. */
#define VIDEO_REPORT_INTERVAL	2000

/* The front of each frame, its number and length. */
#define VIDEO_HEAD_LEN		8

static void	*video_encode(void *);
static void	*video_decode(void *);
static void	*video_source(void *);
static size_t	video_unit(const u_int8_t *, size_t, size_t, int *);
static void	video_fill(u_int8_t *, u_int32_t, u_int32_t);
static int	video_check(const u_int8_t *, size_t, u_int32_t *);

/* The threads and if they should keep running. */
static pthread_t		encoder;
static pthread_t		decoder;
static volatile int		decoding = 0;
static volatile int		encoding = 0;

/* Set when the core wants a keyframe, and the bitrate it wants. */
static volatile int		keyframe = 0;
static volatile u_int32_t	bitrate = 0;

/* What we decoded, for the tests. */
volatile u_int64_t		host_video_ok = 0;
volatile u_int64_t		host_video_bad = 0;
volatile u_int64_t		host_video_keys = 0;

/*
 * Start our fake decoder.
 */
int
vigil_platform_video_start(void)
{
	PRECOND(decoding == 0);

	vigil_atomic_write(&decoding, 1);

	if (pthread_create(&decoder, NULL, video_decode, NULL) != 0) {
		vigil_atomic_write(&decoding, 0);
		return (-1);
	}

	return (0);
}

/*
 * Stop our fake decoder and encoder.
 */
void
vigil_platform_video_stop(void)
{
	vigil_platform_video_camera(0);

	if (!vigil_atomic_read(&decoding))
		return;

	vigil_atomic_write(&decoding, 0);
	(void)pthread_join(decoder, NULL);
}

/*
 * Start or stop our fake encoder.
 */
void
vigil_platform_video_camera(int on)
{
	PRECOND(on == 0 || on == 1);

	if (on == vigil_atomic_read(&encoding))
		return;

	vigil_atomic_write(&encoding, on);

	if (on == 0) {
		(void)pthread_join(encoder, NULL);
		return;
	}

	if (getenv("VIGIL_VIDEO_SOURCE") != NULL) {
		if (pthread_create(&encoder, NULL, video_source, NULL) != 0) {
			vigil_log(LOG_ERR, "[video] no source thread");
			vigil_atomic_write(&encoding, 0);
		}
		return;
	}

	if (pthread_create(&encoder, NULL, video_encode, NULL) != 0) {
		vigil_log(LOG_ERR, "[video] no encoder thread");
		vigil_atomic_write(&encoding, 0);
	}
}

/*
 * The core wants a keyframe on the next frame.
 */
void
vigil_platform_video_keyframe(void)
{
	vigil_atomic_write(&keyframe, 1);
}

/*
 * The core wants frames at the given bitrate.
 */
void
vigil_platform_video_bitrate(u_int32_t bps)
{
	vigil_atomic_write(&bitrate, bps);
}

/*
 * Every frame interval we encode a frame sized for the bitrate, the
 * first one and every VIDEO_KEY_FRAMES after it or when asked a keyframe.
 */
static void *
video_encode(void *arg)
{
	int		flags;
	u_int8_t	*frame;
	u_int32_t	seq, len, since;

	PRECOND(arg == NULL);

	frame = calloc(1, VIGIL_VIDEO_FRAME_MAX);
	if (frame == NULL)
		fatal("calloc: failed to allocate video frame");

	seq = 1;
	since = VIDEO_KEY_FRAMES;

	while (vigil_atomic_read(&encoding)) {
		len = vigil_atomic_read(&bitrate) / 8 /
		    (1000 / VIDEO_FRAME_INTERVAL);

		if (since >= VIDEO_KEY_FRAMES || vigil_atomic_read(&keyframe)) {
			vigil_atomic_write(&keyframe, 0);
			flags = VIGIL_VIDEO_FLAG_KEY;
			len *= VIDEO_KEY_SCALE;
			since = 0;
		} else {
			flags = 0;
		}

		if (len < VIDEO_HEAD_LEN)
			len = VIDEO_HEAD_LEN;

		if (len > VIGIL_VIDEO_FRAME_MAX)
			len = VIGIL_VIDEO_FRAME_MAX;

		video_fill(frame, seq, len);

		if (vigil_video_input(frame, len, flags) == -1)
			since = VIDEO_KEY_FRAMES;
		else
			since++;

		seq++;
		(void)poll(NULL, 0, VIDEO_FRAME_INTERVAL);
	}

	free(frame);

	return (NULL);
}

/*
 * Take every frame the core has for us and check it.
 */
static void *
video_decode(void *arg)
{
	size_t		len;
	u_int32_t	last;
	int		flags;
	FILE		*dump;
	const char	*path;
	u_int64_t	report;
	u_int8_t	*frame;

	PRECOND(arg == NULL);

	last = 0;
	dump = NULL;

	path = getenv("VIGIL_VIDEO_DUMP");
	if (path != NULL) {
		dump = fopen(path, "w");
		if (dump == NULL)
			fatal("fopen(%s): %s", path, errno_s);
	}

	frame = calloc(1, VIGIL_VIDEO_FRAME_MAX);
	if (frame == NULL)
		fatal("calloc: failed to allocate video frame");

	report = vigil_time_ms() + VIDEO_REPORT_INTERVAL;

	while (vigil_atomic_read(&decoding)) {
		len = vigil_video_take(frame, VIGIL_VIDEO_FRAME_MAX, &flags);

		if (len == 0) {
			(void)poll(NULL, 0, 5);
		} else if (dump != NULL) {
			if (fwrite(frame, len, 1, dump) != 1)
				fatal("fwrite: %s", errno_s);
			vigil_atomic_add(&host_video_ok, 1);
			if (flags & VIGIL_VIDEO_FLAG_KEY)
				vigil_atomic_add(&host_video_keys, 1);
		} else if (video_check(frame, len, &last) == -1) {
			vigil_atomic_add(&host_video_bad, 1);
		} else {
			vigil_atomic_add(&host_video_ok, 1);

			if (flags & VIGIL_VIDEO_FLAG_KEY)
				vigil_atomic_add(&host_video_keys, 1);
		}

		if (vigil_time_ms() >= report) {
			vigil_log(LOG_INFO, "[video] decoded %" PRIu64
			    " (%" PRIu64 " keys), %" PRIu64 " bad",
			    vigil_atomic_read(&host_video_ok),
			    vigil_atomic_read(&host_video_keys),
			    vigil_atomic_read(&host_video_bad));
			report = vigil_time_ms() + VIDEO_REPORT_INTERVAL;
		}
	}

	if (dump != NULL)
		(void)fclose(dump);

	free(frame);

	return (NULL);
}

/*
 * Send the frames from the file VIGIL_VIDEO_SOURCE names, over and over.
 */
static void *
video_source(void *arg)
{
	FILE		*fp;
	long		size;
	int		flags;
	u_int8_t	*data;
	size_t		offset, len;

	PRECOND(arg == NULL);

	fp = fopen(getenv("VIGIL_VIDEO_SOURCE"), "r");
	if (fp == NULL)
		fatal("fopen: %s", errno_s);

	if (fseek(fp, 0, SEEK_END) == -1)
		fatal("fseek: %s", errno_s);

	size = ftell(fp);
	if (size <= 0)
		fatal("video source is empty");

	rewind(fp);

	data = malloc(size);
	if (data == NULL)
		fatal("malloc: failed to allocate video source");

	if (fread(data, size, 1, fp) != 1)
		fatal("fread: %s", errno_s);

	(void)fclose(fp);

	offset = 0;

	while (vigil_atomic_read(&encoding)) {
		len = video_unit(data, size, offset, &flags);

		if (len > 0 && len <= VIGIL_VIDEO_FRAME_MAX)
			(void)vigil_video_input(data + offset, len, flags);

		offset += len;
		if (offset >= (size_t)size)
			offset = 0;

		(void)poll(NULL, 0, VIDEO_FRAME_INTERVAL);
	}

	free(data);

	return (NULL);
}

/*
 * Returns the length of the access unit at offset, it runs up to the next
 * access unit delimiter. Sets the key flag if it holds an IDR slice.
 */
static size_t
video_unit(const u_int8_t *data, size_t size, size_t offset, int *flags)
{
	size_t		idx;
	u_int8_t	type;

	PRECOND(data != NULL);
	PRECOND(flags != NULL);
	PRECOND(offset < size);

	*flags = 0;

	for (idx = offset; idx + 4 < size; idx++) {
		if (data[idx] != 0 || data[idx + 1] != 0 || data[idx + 2] != 1)
			continue;

		type = data[idx + 3] & 0x1f;

		if (type == 5)
			*flags = VIGIL_VIDEO_FLAG_KEY;

		if (type == 9 && idx > offset + 1) {
			if (data[idx - 1] == 0)
				return (idx - 1 - offset);
			return (idx - offset);
		}
	}

	return (size - offset);
}

/*
 * Fill a frame with its number, its length and a pattern derived from both.
 */
static void
video_fill(u_int8_t *frame, u_int32_t seq, u_int32_t len)
{
	u_int32_t	idx;

	PRECOND(frame != NULL);
	PRECOND(len >= VIDEO_HEAD_LEN);

	memcpy(frame, &seq, sizeof(seq));
	memcpy(frame + sizeof(seq), &len, sizeof(len));

	for (idx = VIDEO_HEAD_LEN; idx < len; idx++)
		frame[idx] = (seq * 31 + idx) & 0xff;
}

/*
 * Check that a frame we got holds what video_fill() put in it, and that
 * it comes after the last one we got.
 */
static int
video_check(const u_int8_t *frame, size_t len, u_int32_t *last)
{
	size_t		idx;
	u_int32_t	seq, size;

	PRECOND(frame != NULL);
	PRECOND(last != NULL);

	if (len < VIDEO_HEAD_LEN)
		return (-1);

	memcpy(&seq, frame, sizeof(seq));
	memcpy(&size, frame + sizeof(seq), sizeof(size));

	if (size != len)
		return (-1);

	if (*last != 0 && seq <= *last)
		return (-1);

	for (idx = VIDEO_HEAD_LEN; idx < len; idx++) {
		if (frame[idx] != ((seq * 31 + idx) & 0xff))
			return (-1);
	}

	*last = seq;

	return (0);
}
