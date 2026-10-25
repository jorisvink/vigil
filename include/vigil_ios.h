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

#ifndef __H_VIGIL_IOS_H
#define __H_VIGIL_IOS_H

#include <sys/types.h>

#include "vigil_shared.h"

/*
 * The video of a call, the app does it and gives us these. They are
 * called from the voice thread.
 */
struct vigil_ios_video {
	int	(*start)(void);
	void	(*stop)(void);
	void	(*camera)(int);
	void	(*keyframe)(void);
	void	(*bitrate)(u_int32_t);
};

int	vigil_ios_utun_fd(void);
void	vigil_ios_audio_restart(void);
void	vigil_ios_video_set(const struct vigil_ios_video *);

/* XXX - from libkyrka. */
const char	*kyrka_version(void);

#endif
