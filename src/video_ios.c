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

#include "vigil.h"
#include "vigil_ios.h"

/*
 * The camera, encoder and decoder live in VigilVideo.swift, it gives us
 * its functions before any call is made. Without them calls stay voice.
 */

/* The functions the app gave us. */
static struct vigil_ios_video	hooks;
static volatile int		hooked = 0;

/*
 * Called by the app with its video functions.
 */
void
vigil_ios_video_set(const struct vigil_ios_video *video)
{
	PRECOND(video != NULL);
	PRECOND(video->start != NULL);
	PRECOND(video->stop != NULL);
	PRECOND(video->camera != NULL);
	PRECOND(video->keyframe != NULL);
	PRECOND(video->bitrate != NULL);

	hooks = *video;
	vigil_atomic_write(&hooked, 1);
}

/*
 * Start the decoder, returns -1 if there is none.
 */
int
vigil_platform_video_start(void)
{
	if (!vigil_atomic_read(&hooked))
		return (-1);

	if (hooks.start() == -1)
		return (-1);

	return (0);
}

/*
 * Stop the camera, encoder and decoder, they do not call us after this.
 */
void
vigil_platform_video_stop(void)
{
	if (!vigil_atomic_read(&hooked))
		return;

	hooks.stop();
}

/*
 * Turn the camera and encoder on or off.
 */
void
vigil_platform_video_camera(int on)
{
	PRECOND(on == 0 || on == 1);

	if (!vigil_atomic_read(&hooked))
		return;

	hooks.camera(on);
}

/*
 * Ask the encoder for a keyframe.
 */
void
vigil_platform_video_keyframe(void)
{
	if (!vigil_atomic_read(&hooked))
		return;

	hooks.keyframe();
}

/*
 * Give the encoder the bitrate to aim for.
 */
void
vigil_platform_video_bitrate(u_int32_t bps)
{
	if (!vigil_atomic_read(&hooked))
		return;

	hooks.bitrate(bps);
}
