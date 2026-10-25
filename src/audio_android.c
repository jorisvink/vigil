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

#include <aaudio/AAudio.h>

#include <pthread.h>

#include "vigil.h"

/* The most times we reopen our streams during a single call. */
#define AUDIO_RESTARTS_MAX		8

static void				audio_lock(void);
static int				audio_open(void);
static void				audio_close(void);
static void				audio_unlock(void);
static void				*audio_restart(void *);
static int				audio_stream(aaudio_direction_t,
					    AAudioStream **);
static aaudio_data_callback_result_t	audio_data(AAudioStream *, void *,
					    void *, int32_t);
static void				audio_error(AAudioStream *, void *,
					    aaudio_result_t);

/* Set while we want audio, a restart only reopens the streams then. */
static volatile int		running = 0;

/* Set while a restart is underway, and how many we did this call. */
static volatile int		restarts = 0;
static volatile int		restarting = 0;

/* Our capture and playback streams. */
static AAudioStream		*capture = NULL;
static AAudioStream		*playback = NULL;

/* Protects the streams against a restart running at the same time. */
static pthread_mutex_t		lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * Open and start our capture and playback streams for a call.
 */
int
vigil_platform_audio_start(void)
{
	int		ret;

	audio_lock();

	vigil_atomic_write(&restarts, 0);
	vigil_atomic_write(&running, 1);

	ret = audio_open();
	if (ret == -1) {
		vigil_atomic_write(&running, 0);
		audio_close();
	}

	audio_unlock();

	return (ret);
}

/*
 * Stop and close our streams.
 */
void
vigil_platform_audio_stop(void)
{
	audio_lock();

	vigil_atomic_write(&running, 0);
	audio_close();

	audio_unlock();
}

/*
 * Open both streams and start them, called with the lock held.
 */
static int
audio_open(void)
{
	aaudio_result_t		ret;

	PRECOND(capture == NULL);
	PRECOND(playback == NULL);

	if (audio_stream(AAUDIO_DIRECTION_INPUT, &capture) == -1)
		return (-1);

	if (audio_stream(AAUDIO_DIRECTION_OUTPUT, &playback) == -1)
		return (-1);

	ret = AAudioStream_requestStart(capture);
	if (ret != AAUDIO_OK) {
		vigil_log(LOG_ERR, "[audio] capture start: %s",
		    AAudio_convertResultToText(ret));
		return (-1);
	}

	ret = AAudioStream_requestStart(playback);
	if (ret != AAUDIO_OK) {
		vigil_log(LOG_ERR, "[audio] playback start: %s",
		    AAudio_convertResultToText(ret));
		return (-1);
	}

	vigil_log(LOG_INFO, "[audio] running at %d/%d hz",
	    AAudioStream_getSampleRate(capture),
	    AAudioStream_getSampleRate(playback));

	return (0);
}

/*
 * Stop and close both streams, called with the lock held.
 */
static void
audio_close(void)
{
	if (capture != NULL) {
		(void)AAudioStream_requestStop(capture);
		(void)AAudioStream_close(capture);
		capture = NULL;
	}

	if (playback != NULL) {
		(void)AAudioStream_requestStop(playback);
		(void)AAudioStream_close(playback);
		playback = NULL;
	}
}

/*
 * Open a stream in the given direction, 16-bit mono at our sample rate
 * and set up for voice so the platform does echo cancellation.
 */
static int
audio_stream(aaudio_direction_t dir, AAudioStream **stream)
{
	aaudio_result_t		ret;
	AAudioStreamBuilder	*builder;

	PRECOND(stream != NULL);
	PRECOND(*stream == NULL);

	ret = AAudio_createStreamBuilder(&builder);
	if (ret != AAUDIO_OK) {
		vigil_log(LOG_ERR, "[audio] stream builder: %s",
		    AAudio_convertResultToText(ret));
		return (-1);
	}

	AAudioStreamBuilder_setDirection(builder, dir);
	AAudioStreamBuilder_setChannelCount(builder, 1);
	AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
	AAudioStreamBuilder_setSampleRate(builder, VIGIL_VOICE_SAMPLE_RATE);
	AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
	AAudioStreamBuilder_setPerformanceMode(builder,
	    AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
	AAudioStreamBuilder_setDataCallback(builder, audio_data, NULL);
	AAudioStreamBuilder_setErrorCallback(builder, audio_error, NULL);

	if (dir == AAUDIO_DIRECTION_INPUT) {
		AAudioStreamBuilder_setInputPreset(builder,
		    AAUDIO_INPUT_PRESET_VOICE_COMMUNICATION);
	} else {
		AAudioStreamBuilder_setUsage(builder,
		    AAUDIO_USAGE_VOICE_COMMUNICATION);
		AAudioStreamBuilder_setContentType(builder,
		    AAUDIO_CONTENT_TYPE_SPEECH);
	}

	ret = AAudioStreamBuilder_openStream(builder, stream);
	AAudioStreamBuilder_delete(builder);

	if (ret != AAUDIO_OK) {
		vigil_log(LOG_ERR, "[audio] %s stream: %s",
		    dir == AAUDIO_DIRECTION_INPUT ? "capture" : "playback",
		    AAudio_convertResultToText(ret));
		*stream = NULL;
		return (-1);
	}

	/* Our frames are 20ms at our sample rate, nothing else will do. */
	if (AAudioStream_getSampleRate(*stream) != VIGIL_VOICE_SAMPLE_RATE ||
	    AAudioStream_getChannelCount(*stream) != 1 ||
	    AAudioStream_getFormat(*stream) != AAUDIO_FORMAT_PCM_I16) {
		vigil_log(LOG_ERR, "[audio] %s stream is %d hz, %d channels",
		    dir == AAUDIO_DIRECTION_INPUT ? "capture" : "playback",
		    AAudioStream_getSampleRate(*stream),
		    AAudioStream_getChannelCount(*stream));
		AAudioStream_close(*stream);
		*stream = NULL;
		return (-1);
	}

	return (0);
}

/*
 * Called by AAudio when it has captured samples for us or wants
 * samples from us to play.
 */
static aaudio_data_callback_result_t
audio_data(AAudioStream *stream, void *udata, void *data, int32_t frames)
{
	PRECOND(stream != NULL);
	PRECOND(data != NULL);

	if (frames <= 0)
		return (AAUDIO_CALLBACK_RESULT_CONTINUE);

	if (AAudioStream_getDirection(stream) == AAUDIO_DIRECTION_INPUT)
		vigil_voice_capture(data, frames);
	else
		vigil_voice_playback(data, frames);

	return (AAUDIO_CALLBACK_RESULT_CONTINUE);
}

/*
 * Called by AAudio when a stream broke, for example because the audio
 * route changed. The stream is of no use to us anymore and we may not
 * reopen it from here, so a thread does it.
 */
static void
audio_error(AAudioStream *stream, void *udata, aaudio_result_t error)
{
	pthread_t	thread;

	PRECOND(stream != NULL);

	vigil_log(LOG_NOTICE, "[audio] stream error: %s",
	    AAudio_convertResultToText(error));

	if (!vigil_atomic_read(&running))
		return;

	/* Both streams break together, one restart reopens them both. */
	if (!vigil_atomic_cas_simple(&restarting, 0, 1))
		return;

	if (vigil_atomic_add(&restarts, 1) >= AUDIO_RESTARTS_MAX) {
		vigil_log(LOG_ERR, "[audio] streams keep breaking, giving up");
		vigil_atomic_write(&restarting, 0);
		return;
	}

	if (pthread_create(&thread, NULL, audio_restart, NULL) != 0) {
		vigil_log(LOG_ERR, "[audio] no thread to restart audio");
		vigil_atomic_write(&restarting, 0);
		return;
	}

	(void)pthread_detach(thread);
}

/*
 * Reopen our streams after they broke, if we still want audio.
 */
static void *
audio_restart(void *arg)
{
	PRECOND(arg == NULL);

	audio_lock();

	if (vigil_atomic_read(&running)) {
		vigil_log(LOG_INFO, "[audio] reopening our streams");
		audio_close();
		if (audio_open() == -1)
			audio_close();
	}

	vigil_atomic_write(&restarting, 0);
	audio_unlock();

	return (NULL);
}

/*
 * Take the lock that protects our streams.
 */
static void
audio_lock(void)
{
	if (pthread_mutex_lock(&lock) != 0)
		fatal("failed to take the audio lock");
}

/*
 * Release the lock that protects our streams.
 */
static void
audio_unlock(void)
{
	if (pthread_mutex_unlock(&lock) != 0)
		fatal("failed to release the audio lock");
}
