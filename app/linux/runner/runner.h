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

#ifndef __H_RUNNER_H
#define __H_RUNNER_H

#include <sys/types.h>

#include <flutter_linux/flutter_linux.h>

/* The longest name a piece of data in the store has, with its NUL. */
#define STORE_NAME_MAX		48

/* desktop.c */
int	runner_single(void);
void	runner_stop(void);
int	runner_channel_init(FlView *, GtkWindow *);

/* audio.c */
int	runner_audio_busy(void);

/* memo.c */
void	memo_stop(void);
int	memo_record(void);
void	memo_seek(int);
void	memo_speed(double);
int	memo_position(u_int64_t *, u_int64_t *);
int	memo_play(const u_int8_t *, size_t, int, double, FlMethodCall *);

u_int8_t	*memo_finish(size_t *);

/* dialog.c */
int	runner_pick(GtkWindow *, const char *, char **);
int	runner_save(GtkWindow *, const char *, char **);
int	runner_passphrase(GtkWindow *, const char *, int, char **, size_t *);

/* store.c */
int	store_exists(void);
int	store_path(char *, size_t, const char *);
void	store_random(void *, size_t);
int	store_open(const char *, size_t);
int	store_create(const char *, size_t);
void	store_reset(void);
void	store_avatars(const char *, size_t);
void	store_chat_drop(const char *);
int	store_chat_valid(const char *);
int	store_chat_exists(const char *);
void	store_chat_delete(const char *);
int	store_chat_append(const char *, const char *, size_t);
char	*store_records(size_t *);
char	*store_config(size_t *);
int	store_config_save(const char *, size_t);
void	store_config_drop(void);
void	store_data_drop(const char *);
int	store_data_put(const char *, const u_int8_t *, size_t);
u_int8_t	*store_data_get(const char *, size_t *);
u_int8_t	*store_read(const char *, size_t, size_t *);

#endif
