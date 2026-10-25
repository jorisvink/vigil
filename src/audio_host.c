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

#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>

#include "vigil.h"

/*
 * Hosts have no microphone for us, we capture a tone instead and log
 * how loud the audio is that we get to play.
 */

/* The frequency of the tone we send. */
#define AUDIO_TONE_HZ		440

/* The milliseconds between level reports. */
#define AUDIO_REPORT_INTERVAL	2000

static void	*audio_thread(void *);

/* The audio thread and if it should keep running. */
static pthread_t		thread;
static volatile int		running = 0;

/* The loudest playback level since the last report, for the tests. */
volatile u_int32_t		host_audio_level = 0;

/*
 * Start our fake audio.
 */
int
vigil_platform_audio_start(void)
{
	PRECOND(running == 0);

	vigil_atomic_write(&running, 1);

	if (pthread_create(&thread, NULL, audio_thread, NULL) != 0) {
		vigil_atomic_write(&running, 0);
		return (-1);
	}

	return (0);
}

/*
 * Stop our fake audio.
 */
void
vigil_platform_audio_stop(void)
{
	if (!vigil_atomic_read(&running))
		return;

	vigil_atomic_write(&running, 0);
	(void)pthread_join(thread, NULL);
}

/*
 * Every 20ms we capture a frame of our tone and play a frame, the
 * same pace a real sound card would have.
 */
static void *
audio_thread(void *arg)
{
	double			sum;
	u_int32_t		level;
	struct timespec		duration;
	size_t			idx, phase;
	u_int64_t		next, report;
	int16_t			pcm[VIGIL_VOICE_SAMPLE_COUNT];

	PRECOND(arg == NULL);

	phase = 0;
	level = 0;

	duration.tv_sec = 0;
	duration.tv_nsec = 1000000;

	next = vigil_time_ms();
	report = next + AUDIO_REPORT_INTERVAL;

	while (vigil_atomic_read(&running)) {
		for (idx = 0; idx < VIGIL_VOICE_SAMPLE_COUNT; idx++) {
			pcm[idx] = 8000 * sin(2 * M_PI * AUDIO_TONE_HZ *
			    phase++ / VIGIL_VOICE_SAMPLE_RATE);
		}

		vigil_voice_capture(pcm, VIGIL_VOICE_SAMPLE_COUNT);
		vigil_voice_playback(pcm, VIGIL_VOICE_SAMPLE_COUNT);

		sum = 0;
		for (idx = 0; idx < VIGIL_VOICE_SAMPLE_COUNT; idx++)
			sum += (double)pcm[idx] * pcm[idx];

		if ((u_int32_t)sqrt(sum / VIGIL_VOICE_SAMPLE_COUNT) > level)
			level = sqrt(sum / VIGIL_VOICE_SAMPLE_COUNT);

		if (vigil_time_ms() >= report) {
			vigil_log(LOG_INFO, "[audio] playback level %u", level);
			vigil_atomic_write(&host_audio_level, level);
			report = vigil_time_ms() + AUDIO_REPORT_INTERVAL;
			level = 0;
		}

		next += 20;

		while (vigil_time_ms() < next)
			(void)nanosleep(&duration, NULL);
	}

	return (NULL);
}
