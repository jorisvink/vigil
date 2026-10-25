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

#include <AudioToolbox/AudioToolbox.h>

#include <pthread.h>

#include "vigil.h"
#include "vigil_ios.h"

/*
 * We use the voice processing i/o unit so we get echo cancellation,
 * the audio session itself is set up and activated by CallKit.
 */

/* The busses of an i/o unit. */
#define AUDIO_BUS_OUTPUT	0
#define AUDIO_BUS_INPUT		1

/* The most frames we get to render in one go. */
#define AUDIO_FRAMES_MAX	4096

static int	audio_open(void);
static void	audio_lock(void);
static int	audio_setup(void);
static void	audio_close(void);
static void	audio_unlock(void);

static OSStatus	audio_capture(void *, AudioUnitRenderActionFlags *,
		    const AudioTimeStamp *, UInt32, UInt32, AudioBufferList *);
static OSStatus	audio_playback(void *, AudioUnitRenderActionFlags *,
		    const AudioTimeStamp *, UInt32, UInt32, AudioBufferList *);

/* Our voice processing i/o unit. */
static AudioUnit	unit = NULL;

/* Are we actively running or not. */
static volatile int	running = 0;

/* Where we render captured audio into. */
static int16_t		captured[AUDIO_FRAMES_MAX];

/* We have a mutex to prevent simultaneous restarts. */
static pthread_mutex_t	lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * Create and start our audio unit for a call.
 */
int
vigil_platform_audio_start(void)
{
	int		ret;

	audio_lock();
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
 * Stop and release our audio unit.
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
 * Called by the app when our audio unit stopped working, for example
 * after an interruption ended or the media services were reset. We
 * build a new unit if we still want audio. May be called from any thread.
 */
void
vigil_ios_audio_restart(void)
{
	audio_lock();

	if (vigil_atomic_read(&running)) {
		vigil_log(LOG_INFO, "[audio] reopening our unit");
		audio_close();
		if (audio_open() == -1)
			audio_close();
	}

	audio_unlock();
}

/*
 * Create our audio unit and start it, called with the lock held.
 */
static int
audio_open(void)
{
	OSStatus	ret;

	PRECOND(unit == NULL);

	if (audio_setup() == -1)
		return (-1);

	ret = AudioOutputUnitStart(unit);
	if (ret != noErr) {
		vigil_log(LOG_ERR, "[audio] start: %d", (int)ret);
		return (-1);
	}

	vigil_log(LOG_INFO, "[audio] running");

	return (0);
}

/*
 * Stop and release our audio unit, called with the lock held.
 */
static void
audio_close(void)
{
	if (unit == NULL)
		return;

	(void)AudioOutputUnitStop(unit);
	(void)AudioUnitUninitialize(unit);
	(void)AudioComponentInstanceDispose(unit);

	unit = NULL;
}

/*
 * Create the audio unit with capture and playback enabled, both in
 * 16-bit mono at our sample rate.
 */
static int
audio_setup(void)
{
	UInt32				on;
	AURenderCallbackStruct		cb;
	OSStatus			ret;
	AudioStreamBasicDescription	fmt;
	AudioComponent			comp;
	AudioComponentDescription	desc;

	PRECOND(unit == NULL);

	memset(&desc, 0, sizeof(desc));

	desc.componentType = kAudioUnitType_Output;
	desc.componentSubType = kAudioUnitSubType_VoiceProcessingIO;
	desc.componentManufacturer = kAudioUnitManufacturer_Apple;

	comp = AudioComponentFindNext(NULL, &desc);
	if (comp == NULL) {
		vigil_log(LOG_ERR, "[audio] no voice processing unit");
		return (-1);
	}

	ret = AudioComponentInstanceNew(comp, &unit);
	if (ret != noErr) {
		vigil_log(LOG_ERR, "[audio] instance: %d", (int)ret);
		unit = NULL;
		return (-1);
	}

	on = 1;

	ret = AudioUnitSetProperty(unit,
	    kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input,
	    AUDIO_BUS_INPUT, &on, sizeof(on));
	if (ret != noErr) {
		vigil_log(LOG_ERR, "[audio] enable input: %d", (int)ret);
		return (-1);
	}

	memset(&fmt, 0, sizeof(fmt));

	fmt.mBitsPerChannel = 16;
	fmt.mFramesPerPacket = 1;
	fmt.mChannelsPerFrame = 1;
	fmt.mBytesPerFrame = sizeof(int16_t);
	fmt.mBytesPerPacket = sizeof(int16_t);
	fmt.mFormatID = kAudioFormatLinearPCM;
	fmt.mSampleRate = VIGIL_VOICE_SAMPLE_RATE;
	fmt.mFormatFlags = kAudioFormatFlagIsSignedInteger |
	    kAudioFormatFlagIsPacked;

	if ((ret = AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat,
	    kAudioUnitScope_Output, AUDIO_BUS_INPUT, &fmt,
	    sizeof(fmt))) != noErr) {
		vigil_log(LOG_ERR, "[audio] capture format: %d", (int)ret);
		return (-1);
	}

	if ((ret = AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat,
	    kAudioUnitScope_Input, AUDIO_BUS_OUTPUT, &fmt,
	    sizeof(fmt))) != noErr) {
		vigil_log(LOG_ERR, "[audio] playback format: %d", (int)ret);
		return (-1);
	}

	cb.inputProcRefCon = NULL;
	cb.inputProc = audio_capture;

	if ((ret = AudioUnitSetProperty(unit,
	    kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global,
	    AUDIO_BUS_INPUT, &cb, sizeof(cb))) != noErr) {
		vigil_log(LOG_ERR, "[audio] capture callback: %d", (int)ret);
		return (-1);
	}

	cb.inputProc = audio_playback;
	cb.inputProcRefCon = NULL;

	if ((ret = AudioUnitSetProperty(unit,
	    kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input,
	    AUDIO_BUS_OUTPUT, &cb, sizeof(cb))) != noErr) {
		vigil_log(LOG_ERR, "[audio] playback callback: %d", (int)ret);
		return (-1);
	}

	if ((ret = AudioUnitInitialize(unit)) != noErr) {
		vigil_log(LOG_ERR, "[audio] initialize: %d", (int)ret);
		return (-1);
	}

	return (0);
}

/*
 * Called by the audio unit when it captured frames, we render them
 * into our own buffer and hand them to the voice session.
 */
static OSStatus
audio_capture(void *udata, AudioUnitRenderActionFlags *flags,
    const AudioTimeStamp *ts, UInt32 bus, UInt32 frames, AudioBufferList *data)
{
	OSStatus		ret;
	AudioBufferList		list;

	PRECOND(flags != NULL);
	PRECOND(ts != NULL);
	PRECOND(bus == AUDIO_BUS_INPUT);
	PRECOND(unit != NULL);

	if (frames == 0 || frames > AUDIO_FRAMES_MAX)
		return (noErr);

	list.mNumberBuffers = 1;
	list.mBuffers[0].mNumberChannels = 1;
	list.mBuffers[0].mData = captured;
	list.mBuffers[0].mDataByteSize = frames * sizeof(int16_t);

	ret = AudioUnitRender(unit, flags, ts, bus, frames, &list);
	if (ret != noErr)
		return (ret);

	vigil_voice_capture(captured, frames);

	return (noErr);
}

/*
 * Called by the audio unit when it wants frames to play.
 */
static OSStatus
audio_playback(void *udata, AudioUnitRenderActionFlags *flags,
    const AudioTimeStamp *ts, UInt32 bus, UInt32 frames, AudioBufferList *data)
{
	UInt32		idx;

	PRECOND(bus == AUDIO_BUS_OUTPUT);
	PRECOND(data != NULL);

	for (idx = 0; idx < data->mNumberBuffers; idx++) {
		vigil_voice_playback(data->mBuffers[idx].mData,
		    data->mBuffers[idx].mDataByteSize / sizeof(int16_t));
	}

	return (noErr);
}

/*
 * Take the lock that protects our audio unit.
 */
static void
audio_lock(void)
{
	if (pthread_mutex_lock(&lock) != 0)
		fatal("failed to take the audio lock");
}

/*
 * Release the lock that protects our audio unit.
 */
static void
audio_unlock(void)
{
	if (pthread_mutex_unlock(&lock) != 0)
		fatal("failed to release the audio lock");
}
