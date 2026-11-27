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

#include <pulse/error.h>
#include <pulse/simple.h>

#include <pthread.h>
#include <time.h>

#include "vigil.h"
#include "runner.h"

/* The most times we reopen a stream during a single call. */
#define AUDIO_RESTARTS_MAX	8

/* The milliseconds we wait before reopening a stream. */
#define AUDIO_RESTART_WAIT	500

/* The bytes in a frame, and how many frames we want to have queued. */
#define AUDIO_FRAME_LEN		(VIGIL_VOICE_SAMPLE_COUNT * sizeof(int16_t))
#define AUDIO_FRAMES_QUEUED	3

static void		*audio_capture(void *);
static void		*audio_playback(void *);
static pa_simple	*audio_open(pa_stream_direction_t);
static int		audio_reopen(pa_stream_direction_t, pa_simple **);

/* Our streams, owned by their threads while we are running. */
static pa_simple	*capture = NULL;
static pa_simple	*playback = NULL;

/* Our threads and if they should keep running. */
static pthread_t	capturer;
static pthread_t	player;
static volatile int	running = 0;

/*
 * Returns 1 while a call has our streams.
 */
int
runner_audio_busy(void)
{
	return (vigil_atomic_read(&running));
}

/*
 * Open our streams and start the threads that drive them for a call.
 */
int
vigil_platform_audio_start(int record)
{
	PRECOND(running == 0);
	PRECOND(record == 1);

	if ((capture = audio_open(PA_STREAM_RECORD)) == NULL)
		return (-1);

	if ((playback = audio_open(PA_STREAM_PLAYBACK)) == NULL) {
		pa_simple_free(capture);
		capture = NULL;
		return (-1);
	}

	vigil_atomic_write(&running, 1);

	if (pthread_create(&capturer, NULL, audio_capture, NULL) != 0)
		fatal("failed to start audio capture");

	if (pthread_create(&player, NULL, audio_playback, NULL) != 0)
		fatal("failed to start audio playback");

	return (0);
}

/*
 * Stop our threads and close our streams.
 */
void
vigil_platform_audio_stop(void)
{
	if (!vigil_atomic_read(&running))
		return;

	vigil_atomic_write(&running, 0);

	(void)pthread_join(capturer, NULL);
	(void)pthread_join(player, NULL);

	if (capture != NULL) {
		pa_simple_free(capture);
		capture = NULL;
	}

	if (playback != NULL) {
		pa_simple_free(playback);
		playback = NULL;
	}
}

/*
 * Hand what the microphone gives us to the core.
 */
static void *
audio_capture(void *arg)
{
	int		err;
	int16_t		pcm[VIGIL_VOICE_SAMPLE_COUNT];

	PRECOND(arg == NULL);

	while (vigil_atomic_read(&running)) {
		if (pa_simple_read(capture, pcm, sizeof(pcm), &err) == 0) {
			vigil_voice_capture(pcm, VIGIL_VOICE_SAMPLE_COUNT);
			continue;
		}

		vigil_log(LOG_NOTICE, "[audio] capture: %s", pa_strerror(err));

		if (audio_reopen(PA_STREAM_RECORD, &capture) == -1)
			break;
	}

	return (NULL);
}

/*
 * Play what the core gives us, the blocking write is what paces us.
 */
static void *
audio_playback(void *arg)
{
	int		err;
	int16_t		pcm[VIGIL_VOICE_SAMPLE_COUNT];

	PRECOND(arg == NULL);

	while (vigil_atomic_read(&running)) {
		vigil_voice_playback(pcm, VIGIL_VOICE_SAMPLE_COUNT);

		if (pa_simple_write(playback, pcm, sizeof(pcm), &err) == 0)
			continue;

		vigil_log(LOG_NOTICE, "[audio] playback: %s", pa_strerror(err));

		if (audio_reopen(PA_STREAM_PLAYBACK, &playback) == -1)
			break;
	}

	return (NULL);
}

/*
 * Open a stream in the given direction, 20ms of 48khz mono per frame.
 */
static pa_simple *
audio_open(pa_stream_direction_t dir)
{
	int			err;
	pa_buffer_attr		attr;
	pa_sample_spec		spec;
	pa_simple		*stream;

	spec.format = PA_SAMPLE_S16NE;
	spec.rate = VIGIL_VOICE_SAMPLE_RATE;
	spec.channels = 1;

	attr.maxlength = (u_int32_t)-1;
	attr.tlength = AUDIO_FRAME_LEN * AUDIO_FRAMES_QUEUED;
	attr.prebuf = (u_int32_t)-1;
	attr.minreq = (u_int32_t)-1;
	attr.fragsize = AUDIO_FRAME_LEN;

	stream = pa_simple_new(NULL, "Vigil", dir, NULL, "Call", &spec,
	    NULL, &attr, &err);

	if (stream == NULL)
		vigil_log(LOG_ERR, "[audio] open: %s", pa_strerror(err));

	return (stream);
}

/*
 * The sound server went away or the device changed, wait a little and
 * open the given stream again. Returns -1 if we have to give up on it.
 */
static int
audio_reopen(pa_stream_direction_t dir, pa_simple **stream)
{
	struct timespec		ts;
	int			idx;

	PRECOND(stream != NULL);

	ts.tv_sec = 0;
	ts.tv_nsec = AUDIO_RESTART_WAIT * 1000000;

	pa_simple_free(*stream);
	*stream = NULL;

	for (idx = 0; idx < AUDIO_RESTARTS_MAX; idx++) {
		if (!vigil_atomic_read(&running))
			return (-1);

		(void)nanosleep(&ts, NULL);

		if ((*stream = audio_open(dir)) != NULL)
			return (0);
	}

	return (-1);
}
