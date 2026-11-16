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

#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <pulse/error.h>
#include <pulse/simple.h>

#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sodium.h>

#include "vigil.h"
#include "runner.h"

/*
 * Voice messages. All platforms send each other AAC in ADTS frames, so
 * that is what we record and what we play.
 */

/* What we record, the same as the other platforms. */
#define MEMO_RATE		44100
#define MEMO_BITRATE		32000
#define MEMO_ADTS_LEN		7
#define MEMO_ADTS_RATE_44100	4

/* The largest clip we are willing to keep decoded, a peer picks its size. */
#define MEMO_SAMPLES_MAX	(48000 * 60 * 30)

/* The grain and overlap we use to change speed, in milliseconds. */
#define MEMO_GRAIN_MS		40
#define MEMO_SEARCH_MS		10

/* How far ahead of what the user hears we are, seeking feels this. */
#define MEMO_LATENCY_MS		100

/* How much we want to get from the microphone at a time. */
#define MEMO_FRAGMENT_MS	50

static void	*memo_record_thread(void *);
static void	*memo_play_thread(void *);
static gboolean	memo_play_finished(gpointer);
static gboolean	memo_play_respond(gpointer);
static void	memo_record_encode(AVCodecContext *, AVFrame *);
static int	memo_decode(const u_int8_t *, size_t);
static void	memo_decode_frames(AVCodecContext *, AVPacket *, AVFrame *);
static size_t	memo_stretch(int16_t *, double, double *, int16_t *, size_t);
static void	memo_clip_free(void);
static void	memo_clip_grow(size_t);
static void	*memo_calloc(size_t, size_t);

/* Protects everything below. */
static pthread_mutex_t		lock = PTHREAD_MUTEX_INITIALIZER;

/* What we are recording and the thread doing it. */
static pthread_t	recorder;
static int		record_on = 0;
static volatile int	recording = 0;
static u_int8_t		*recorded = NULL;
static size_t		recorded_len = 0;

/* What we are playing and the thread doing it. */
static pthread_t	player;
static int		play_on = 0;
static volatile int	playing = 0;
static FlMethodCall	*pending = NULL;
static int16_t		*clip = NULL;
static size_t		clip_samples = 0;
static size_t		clip_cap = 0;
static u_int32_t	clip_rate = 0;
static double		clip_at = 0;
static double		clip_speed = 1;

/* Which clip is playing, the end of an older one does not stop this one. */
static guint		generation = 0;

/*
 * Start recording a voice message.
 */
int
memo_record(void)
{
	if (record_on)
		return (-1);

	if (runner_audio_busy())
		return (-1);

	memo_stop();

	recorded = memo_calloc(1, VIGIL_CHAT_DATA_MAX);
	recorded_len = 0;

	vigil_atomic_write(&recording, 1);

	if (pthread_create(&recorder, NULL, memo_record_thread, NULL) != 0)
		fatal("pthread_create");

	record_on = 1;

	return (0);
}

/*
 * Stop recording, returns what we have or NULL if there is nothing.
 * The caller wipes and frees it.
 */
u_int8_t *
memo_finish(size_t *len)
{
	u_int8_t	*out;

	PRECOND(len != NULL);

	if (!record_on)
		return (NULL);

	vigil_atomic_write(&recording, 0);

	if (pthread_join(recorder, NULL) != 0)
		fatal("failed to join the recorder");

	record_on = 0;

	out = NULL;

	if (recorded_len > 0) {
		out = memo_calloc(1, recorded_len);
		memcpy(out, recorded, recorded_len);
		*len = recorded_len;
	}

	sodium_memzero(recorded, VIGIL_CHAT_DATA_MAX);
	free(recorded);

	recorded = NULL;
	recorded_len = 0;

	return (out);
}

/*
 * Play the given voice message from the given millisecond on and answer
 * the given call once playing it ends.
 */
int
memo_play(const u_int8_t *data, size_t len, int at, double rate,
    FlMethodCall *call)
{
	PRECOND(data != NULL);
	PRECOND(call != NULL);

	memo_stop();

	if (runner_audio_busy())
		return (-1);

	if (memo_decode(data, len) == -1)
		return (-1);

	if (at < 0)
		at = 0;

	pthread_mutex_lock(&lock);

	clip_at = (double)at * clip_rate / 1000;
	clip_speed = rate;
	pending = g_object_ref(call);

	if (clip_at >= clip_samples)
		clip_at = 0;

	vigil_atomic_write(&playing, 1);

	if (pthread_create(&player, NULL, memo_play_thread, NULL) != 0)
		fatal("pthread_create");

	play_on = 1;
	generation++;

	pthread_mutex_unlock(&lock);

	return (0);
}

/*
 * Stop playing and let the user interface know.
 */
void
memo_stop(void)
{
	int		was;
	FlMethodCall	*call;
	pthread_t	running;

	pthread_mutex_lock(&lock);

	was = play_on;
	running = player;
	play_on = 0;
	call = pending;
	pending = NULL;

	vigil_atomic_write(&playing, 0);

	pthread_mutex_unlock(&lock);

	if (!was)
		return;

	if (pthread_join(running, NULL) != 0)
		fatal("failed to join the player");

	pthread_mutex_lock(&lock);
	memo_clip_free();
	pthread_mutex_unlock(&lock);

	if (call != NULL)
		g_idle_add(memo_play_respond, call);
}

/*
 * Jump to the given millisecond in what we are playing.
 */
void
memo_seek(int at)
{
	if (at < 0)
		at = 0;

	pthread_mutex_lock(&lock);

	if (clip != NULL) {
		clip_at = (double)at * clip_rate / 1000;
		if (clip_at >= clip_samples)
			clip_at = clip_samples;
	}

	pthread_mutex_unlock(&lock);
}

/*
 * Change how fast we are playing.
 */
void
memo_speed(double rate)
{
	pthread_mutex_lock(&lock);
	clip_speed = rate;
	pthread_mutex_unlock(&lock);
}

/*
 * Take where we are and how long the clip is in milliseconds, returns -1
 * if we are not playing.
 */
int
memo_position(u_int64_t *at, u_int64_t *len)
{
	int		ret;

	PRECOND(at != NULL);
	PRECOND(len != NULL);

	ret = -1;
	pthread_mutex_lock(&lock);

	if (clip != NULL && clip_rate != 0) {
		*at = clip_at * 1000 / clip_rate;
		*len = (u_int64_t)clip_samples * 1000 / clip_rate;
		ret = 0;
	}

	pthread_mutex_unlock(&lock);

	return (ret);
}

/*
 * Capture the microphone and encode it until told otherwise.
 */
static void *
memo_record_thread(void *arg)
{
	pa_buffer_attr		attr;
	int			err;
	size_t			idx;
	AVCodecContext		*ctx;
	pa_sample_spec		spec;
	AVFrame			*frame;
	const AVCodec		*codec;
	float			*plane;
	pa_simple		*stream;
	int16_t			pcm[1024];

	if ((codec = avcodec_find_encoder(AV_CODEC_ID_AAC)) == NULL) {
		vigil_log(LOG_ERR, "[memo] no aac encoder");
		return (NULL);
	}

	ctx = avcodec_alloc_context3(codec);
	frame = av_frame_alloc();

	if (ctx == NULL || frame == NULL)
		fatal("failed to allocate encoder");

	ctx->bit_rate = MEMO_BITRATE;
	ctx->sample_rate = MEMO_RATE;
	ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
	av_channel_layout_default(&ctx->ch_layout, 1);

	if (avcodec_open2(ctx, codec, NULL) < 0) {
		vigil_log(LOG_ERR, "[memo] failed to open aac encoder");
		goto cleanup;
	}

	frame->nb_samples = ctx->frame_size;
	frame->format = ctx->sample_fmt;
	frame->sample_rate = MEMO_RATE;

	if (av_channel_layout_copy(&frame->ch_layout, &ctx->ch_layout) < 0 ||
	    av_frame_get_buffer(frame, 0) < 0)
		fatal("failed to allocate frame");

	PRECOND((size_t)ctx->frame_size <= sizeof(pcm) / sizeof(pcm[0]));

	spec.format = PA_SAMPLE_S16NE;
	spec.rate = MEMO_RATE;
	spec.channels = 1;

	attr.maxlength = (u_int32_t)-1;
	attr.tlength = (u_int32_t)-1;
	attr.prebuf = (u_int32_t)-1;
	attr.minreq = (u_int32_t)-1;
	attr.fragsize = MEMO_RATE * sizeof(int16_t) * MEMO_FRAGMENT_MS / 1000;

	stream = pa_simple_new(NULL, "Vigil", PA_STREAM_RECORD, NULL,
	    "Voice message", &spec, NULL, &attr, &err);

	if (stream == NULL) {
		vigil_log(LOG_ERR, "[memo] microphone: %s", pa_strerror(err));
		goto cleanup;
	}

	frame->pts = 0;

	while (vigil_atomic_read(&recording)) {
		if (pa_simple_read(stream, pcm,
		    ctx->frame_size * sizeof(pcm[0]), &err) == -1) {
			vigil_log(LOG_ERR, "[memo] read: %s", pa_strerror(err));
			break;
		}

		if (av_frame_make_writable(frame) < 0)
			fatal("failed to make frame writable");

		plane = (float *)frame->data[0];

		for (idx = 0; idx < (size_t)ctx->frame_size; idx++)
			plane[idx] = pcm[idx] / 32768.0f;

		if (avcodec_send_frame(ctx, frame) < 0)
			break;

		frame->pts += ctx->frame_size;

		memo_record_encode(ctx, frame);
	}

	pa_simple_free(stream);
	sodium_memzero(pcm, sizeof(pcm));

	if (avcodec_send_frame(ctx, NULL) == 0)
		memo_record_encode(ctx, frame);

cleanup:
	av_frame_free(&frame);
	avcodec_free_context(&ctx);

	return (NULL);
}

/*
 * Take the packets the encoder has and append them in ADTS frames.
 */
static void
memo_record_encode(AVCodecContext *ctx, AVFrame *frame)
{
	AVPacket	*pkt;
	u_int8_t	*out;
	size_t		total;

	if ((pkt = av_packet_alloc()) == NULL)
		fatal("failed to allocate packet");

	while (avcodec_receive_packet(ctx, pkt) == 0) {
		total = MEMO_ADTS_LEN + pkt->size;

		if (recorded_len + total > VIGIL_CHAT_DATA_MAX) {
			av_packet_unref(pkt);
			break;
		}

		out = recorded + recorded_len;

		out[0] = 0xff;
		out[1] = 0xf1;
		out[2] = (1 << 6) | (MEMO_ADTS_RATE_44100 << 2);
		out[3] = (1 << 6) | ((total >> 11) & 0x03);
		out[4] = (total >> 3) & 0xff;
		out[5] = ((total & 0x07) << 5) | 0x1f;
		out[6] = 0xfc;

		memcpy(out + MEMO_ADTS_LEN, pkt->data, pkt->size);
		recorded_len += total;

		av_packet_unref(pkt);
	}

	av_packet_free(&pkt);
}

/*
 * Decode the given AAC into memory so we can seek and change speed.
 */
static int
memo_decode(const u_int8_t *data, size_t len)
{
	const u_int8_t			*at;
	int				used;
	AVPacket			*pkt;
	AVCodecContext			*ctx;
	size_t				left;
	u_int8_t			*input;
	AVFrame				*frame;
	const AVCodec			*codec;
	AVCodecParserContext		*parser;

	if ((codec = avcodec_find_decoder(AV_CODEC_ID_AAC)) == NULL) {
		fprintf(stderr, "vigil: there is no aac decoder\n");
		return (-1);
	}

	parser = av_parser_init(AV_CODEC_ID_AAC);
	ctx = avcodec_alloc_context3(codec);
	frame = av_frame_alloc();
	pkt = av_packet_alloc();

	if (parser == NULL || ctx == NULL || frame == NULL || pkt == NULL)
		fatal("failed to allocate decoder");

	input = NULL;

	if (avcodec_open2(ctx, codec, NULL) < 0)
		goto failed;

	input = memo_calloc(1, len + AV_INPUT_BUFFER_PADDING_SIZE);
	memcpy(input, data, len);

	at = input;
	left = len;

	while (left > 0) {
		used = av_parser_parse2(parser, ctx, &pkt->data, &pkt->size,
		    at, left, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);

		if (used < 0 || (used == 0 && pkt->size == 0))
			break;

		at += used;
		left -= used;

		if (pkt->size > 0)
			memo_decode_frames(ctx, pkt, frame);
	}

	memo_decode_frames(ctx, NULL, frame);

	sodium_memzero(input, len + AV_INPUT_BUFFER_PADDING_SIZE);
	free(input);
	input = NULL;

	if (clip_samples == 0 || clip_rate == 0)
		goto failed;

	av_parser_close(parser);
	avcodec_free_context(&ctx);
	av_frame_free(&frame);
	av_packet_free(&pkt);

	return (0);

failed:
	if (input != NULL) {
		sodium_memzero(input, len + AV_INPUT_BUFFER_PADDING_SIZE);
		free(input);
	}

	pthread_mutex_lock(&lock);
	memo_clip_free();
	pthread_mutex_unlock(&lock);

	av_parser_close(parser);
	avcodec_free_context(&ctx);
	av_frame_free(&frame);
	av_packet_free(&pkt);

	return (-1);
}

/*
 * Hand a packet to the decoder and keep all it gives us as mono.
 */
static void
memo_decode_frames(AVCodecContext *ctx, AVPacket *pkt, AVFrame *frame)
{
	int		ch;
	float		mix;
	const float	*plane;
	size_t		idx, need;

	if (avcodec_send_packet(ctx, pkt) < 0)
		return;

	while (avcodec_receive_frame(ctx, frame) == 0) {
		if (frame->format != AV_SAMPLE_FMT_FLTP)
			continue;

		if (frame->sample_rate <= 0)
			continue;

		if (frame->ch_layout.nb_channels <= 0)
			continue;

		if (clip_rate == 0)
			clip_rate = frame->sample_rate;

		if ((u_int32_t)frame->sample_rate != clip_rate)
			continue;

		need = clip_samples + frame->nb_samples;
		if (need > MEMO_SAMPLES_MAX)
			continue;

		pthread_mutex_lock(&lock);

		if (need > clip_cap) {
			if (need > clip_cap * 2)
				memo_clip_grow(need);
			else
				memo_clip_grow(clip_cap * 2);
		}

		for (idx = 0; idx < (size_t)frame->nb_samples; idx++) {
			mix = 0;

			for (ch = 0; ch < frame->ch_layout.nb_channels; ch++) {
				plane = (const float *)frame->extended_data[ch];
				mix += plane[idx];
			}

			mix /= frame->ch_layout.nb_channels;

			if (mix > 1.0f)
				mix = 1.0f;
			if (mix < -1.0f)
				mix = -1.0f;

			clip[clip_samples + idx] = (int16_t)(mix * 32767.0f);
		}

		clip_samples = need;

		pthread_mutex_unlock(&lock);
	}
}

/*
 * Play the decoded clip until it ends or until we are told to stop.
 */
static void *
memo_play_thread(void *arg)
{
	int		err;
	pa_buffer_attr	attr;
	pa_sample_spec	spec;
	guint		current;
	pa_simple	*stream;
	double		speed, at;
	int16_t		*tail, *out;
	size_t		grain, produced;

	pthread_mutex_lock(&lock);
	current = generation;
	pthread_mutex_unlock(&lock);

	spec.format = PA_SAMPLE_S16NE;
	spec.rate = clip_rate;
	spec.channels = 1;

	attr.maxlength = (u_int32_t)-1;
	attr.tlength = clip_rate * sizeof(int16_t) * MEMO_LATENCY_MS / 1000;
	attr.prebuf = (u_int32_t)-1;
	attr.minreq = (u_int32_t)-1;
	attr.fragsize = (u_int32_t)-1;

	stream = pa_simple_new(NULL, "Vigil", PA_STREAM_PLAYBACK, NULL,
	    "Voice message", &spec, NULL, &attr, &err);

	if (stream == NULL) {
		vigil_log(LOG_ERR, "[memo] speaker: %s", pa_strerror(err));
		g_idle_add(memo_play_finished, GUINT_TO_POINTER(current));
		return (NULL);
	}

	grain = clip_rate * MEMO_GRAIN_MS / 1000 / 2;
	out = memo_calloc(grain, sizeof(int16_t));
	tail = memo_calloc(grain, sizeof(int16_t));

	while (vigil_atomic_read(&playing)) {
		pthread_mutex_lock(&lock);

		speed = clip_speed;
		at = clip_at;

		produced = memo_stretch(tail, speed, &at, out, grain);
		clip_at = at;

		pthread_mutex_unlock(&lock);

		if (produced == 0)
			break;

		if (pa_simple_write(stream, out, produced * sizeof(int16_t),
		    &err) == -1) {
			vigil_log(LOG_ERR, "[memo] write: %s",
			    pa_strerror(err));
			break;
		}
	}

	if (vigil_atomic_read(&playing))
		(void)pa_simple_drain(stream, &err);

	pa_simple_free(stream);

	free(tail);
	free(out);

	g_idle_add(memo_play_finished, GUINT_TO_POINTER(current));

	return (NULL);
}

/*
 * Make len samples of output, at the given speed. Above normal speed this
 * is overlap-add with a search for the best fit so the pitch stays as it
 * was. Called with the lock held.
 */
static size_t
memo_stretch(int16_t *tail, double speed, double *pos, int16_t *out, size_t len)
{
	float		fade;
	double		best, sum;
	ssize_t		delta, found;
	size_t		idx, start, at, search;

	at = (size_t)*pos;

	if (at + len >= clip_samples)
		return (0);

	if (speed == 1) {
		memcpy(out, clip + at, len * sizeof(int16_t));
		memcpy(tail, clip + at, len * sizeof(int16_t));
		*pos += len;
		return (len);
	}

	search = clip_rate * MEMO_SEARCH_MS / 1000;
	found = 0;
	best = -1e300;

	for (delta = -(ssize_t)search; delta <= (ssize_t)search; delta += 4) {
		if ((ssize_t)at + delta < 0)
			continue;

		start = at + delta;

		if (start + 2 * len >= clip_samples)
			break;

		sum = 0;

		for (idx = 0; idx < len; idx += 4)
			sum += (double)tail[idx] * clip[start + idx];

		if (sum > best) {
			best = sum;
			found = delta;
		}
	}

	start = at + found;

	if (start + 2 * len >= clip_samples)
		return (0);

	for (idx = 0; idx < len; idx++) {
		fade = (float)idx / len;
		out[idx] = tail[idx] * (1 - fade) + clip[start + idx] * fade;
	}

	memcpy(tail, clip + start + len, len * sizeof(int16_t));

	*pos += len * speed;

	return (len);
}

/*
 * The clip ended, clean up from the main thread.
 */
static gboolean
memo_play_finished(gpointer udata)
{
	int		same;

	pthread_mutex_lock(&lock);
	same = GPOINTER_TO_UINT(udata) == generation;
	pthread_mutex_unlock(&lock);

	if (same)
		memo_stop();

	return (G_SOURCE_REMOVE);
}

/*
 * Answer the call that started playing.
 */
static gboolean
memo_play_respond(gpointer udata)
{
	FlMethodCall	*call;
	FlValue		*empty;

	call = udata;
	empty = fl_value_new_string("");

	(void)fl_method_call_respond_success(call, empty, NULL);

	fl_value_unref(empty);
	g_object_unref(call);

	return (G_SOURCE_REMOVE);
}

/*
 * Wipe and release the decoded clip, called with the lock held.
 */
static void
memo_clip_free(void)
{
	if (clip != NULL) {
		sodium_memzero(clip, clip_cap * sizeof(int16_t));
		free(clip);
	}

	clip = NULL;
	clip_cap = 0;
	clip_rate = 0;
	clip_samples = 0;
	clip_at = 0;
}

/*
 * Make room for more samples, called with the lock held. The old allocation
 * is wiped as this is what someone said to us.
 */
static void
memo_clip_grow(size_t cap)
{
	int16_t		*next;

	next = memo_calloc(cap, sizeof(int16_t));

	if (clip != NULL) {
		memcpy(next, clip, clip_samples * sizeof(int16_t));
		sodium_memzero(clip, clip_cap * sizeof(int16_t));
		free(clip);
	}

	clip = next;
	clip_cap = cap;
}

/*
 * Allocate zeroed memory or die trying.
 */
static void *
memo_calloc(size_t count, size_t size)
{
	void		*ptr;

	if ((ptr = calloc(count, size)) == NULL)
		fatal("calloc");

	return (ptr);
}
