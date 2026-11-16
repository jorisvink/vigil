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
#include <sys/stat.h>

#include <libkyrka/libkyrka-int.h>

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* We use the checks of vigil, not those of libkyrka. */
#undef PRECOND
#undef VERIFY

#include "vigil.h"

/* The directory of the journal with the given name in the data directory. */
#define JOURNAL_DIR_FMT		"%s/%s"

/* The file in the journal directory that holds the acked seq. */
#define JOURNAL_ACKED		"acked"

/* The file in the journal directory that holds if a desktop is synced. */
#define JOURNAL_SYNCED		"synced"

/* The label we derive the key of the journal under. */
#define JOURNAL_LABEL		"VIGIL.COMPANION.JOURNAL"

/* The length of an entry name, 16 hex digits. */
#define JOURNAL_NAME_LEN	16

/* What a seal adds to an entry, its nonce and its tag. */
#define JOURNAL_SEAL_LEN	(KYRKA_NONCE_LENGTH + KYRKA_TAG_LENGTH)

/* The largest entry we write or read, before its seal. */
#define JOURNAL_ENTRY_MAX	(VIGIL_CHAT_DATA_MAX + 64 * 1024)

static int	journal_path(struct vigil_journal *, char *, size_t,
		    const char *);
static int	journal_entry(struct vigil_journal *, u_int64_t,
		    char *, size_t);
static int	journal_close(int, const char *);
static int	journal_write(const char *, const void *, size_t);
static u_int8_t	*journal_load(const char *, size_t, size_t, size_t *);
static int	journal_name(const char *, u_int64_t *);
static int	journal_cipher(struct vigil_journal *, u_int64_t,
		    u_int8_t *, u_int8_t *, size_t, int);
static int	journal_scan(struct vigil_journal *);

/*
 * Open the journal with the given name in the given data directory. With a
 * key we derive the key the entries are sealed under from the key and the
 * name, a journal without a key only keeps the acked seq and if its
 * desktop is synced. Returns -1 if the journal cannot be used.
 */
int
vigil_journal_open(struct vigil_journal *jr, const char *datadir,
    const char *name, const u_int8_t *key, size_t keylen)
{
	int			len;
	size_t			got;
	struct nyfe_kmac256	kdf;
	u_int8_t		*file;
	u_int64_t		acked;
	char			path[1024];

	PRECOND(jr != NULL);
	PRECOND(datadir != NULL);
	PRECOND(name != NULL);
	PRECOND(key != NULL || keylen == 0);

	memset(jr, 0, sizeof(*jr));

	len = snprintf(jr->dir, sizeof(jr->dir), JOURNAL_DIR_FMT,
	    datadir, name);
	if (len < 0 || (size_t)len >= sizeof(jr->dir)) {
		vigil_log(LOG_NOTICE, "[journal] path too long");
		return (-1);
	}

	if (mkdir(jr->dir, 0700) == -1 && errno != EEXIST) {
		vigil_log(LOG_NOTICE, "[journal] mkdir(%s): %s",
		    jr->dir, errno_s);
		return (-1);
	}

	if (keylen > 0) {
		nyfe_kmac256_init(&kdf, key, keylen,
		    JOURNAL_LABEL, sizeof(JOURNAL_LABEL) - 1);
		nyfe_kmac256_update(&kdf, name, strlen(name));
		nyfe_kmac256_final(&kdf, jr->key, sizeof(jr->key));
		jr->sealed = 1;
	}

	if (journal_path(jr, path, sizeof(path), JOURNAL_ACKED) == -1)
		return (-1);

	file = journal_load(path, sizeof(acked), sizeof(acked), &got);
	if (file == NULL && errno != ENOENT)
		return (-1);

	if (file != NULL) {
		memcpy(&acked, file, sizeof(acked));
		jr->acked = be64toh(acked);
		free(file);
	}

	if (journal_path(jr, path, sizeof(path), JOURNAL_SYNCED) == -1)
		return (-1);

	file = journal_load(path, sizeof(jr->synced), sizeof(jr->synced), &got);
	if (file == NULL && errno != ENOENT)
		return (-1);

	if (file != NULL) {
		jr->synced = file[0];
		free(file);
	}

	jr->last = jr->acked;
	if (jr->sealed && journal_scan(jr) == -1)
		return (-1);

	return (0);
}

/*
 * Forget the key of the given journal, its files stay.
 */
void
vigil_journal_close(struct vigil_journal *jr)
{
	PRECOND(jr != NULL);

	nyfe_mem_zero(jr, sizeof(*jr));
}

/*
 * Seal the given entry under the next seq and write it to disk before we
 * return. Returns the seq of the entry, or 0 if it could not be written.
 */
u_int64_t
vigil_journal_append(struct vigil_journal *jr, const void *data, size_t len)
{
	int		ret;
	u_int64_t	seq;
	size_t		total;
	u_int8_t	*sealed;
	char		path[1024];

	PRECOND(jr != NULL);
	PRECOND(jr->sealed);
	PRECOND(data != NULL);
	PRECOND(len > 0 && len <= JOURNAL_ENTRY_MAX);

	seq = jr->last + 1;
	if (journal_entry(jr, seq, path, sizeof(path)) == -1)
		return (0);

	total = len + JOURNAL_SEAL_LEN;
	if ((sealed = malloc(total)) == NULL)
		fatal("malloc: failed to allocate journal entry");

	nyfe_random_bytes(sealed, KYRKA_NONCE_LENGTH);
	memcpy(sealed + KYRKA_NONCE_LENGTH, data, len);

	if (journal_cipher(jr, seq,
	    sealed, sealed + KYRKA_NONCE_LENGTH, len, 1) == -1) {
		nyfe_mem_zero(sealed, total);
		free(sealed);
		return (0);
	}

	ret = journal_write(path, sealed, total);
	free(sealed);

	if (ret == -1)
		return (0);

	jr->last = seq;
	jr->items++;
	jr->bytes += total;

	return (seq);
}

/*
 * Read the entry with the given seq, the caller wipes and frees it.
 * Returns NULL if we do not have the entry or it does not open.
 */
u_int8_t *
vigil_journal_read(struct vigil_journal *jr, u_int64_t seq, size_t *len)
{
	size_t			got;
	char			path[1024];
	u_int8_t		*sealed, *out;

	PRECOND(jr != NULL);
	PRECOND(jr->sealed);
	PRECOND(len != NULL);

	if (journal_entry(jr, seq, path, sizeof(path)) == -1)
		return (NULL);

	if ((sealed = journal_load(path, JOURNAL_SEAL_LEN + 1,
	    JOURNAL_SEAL_LEN + JOURNAL_ENTRY_MAX, &got)) == NULL)
		return (NULL);

	*len = got - JOURNAL_SEAL_LEN;

	if (journal_cipher(jr, seq,
	    sealed, sealed + KYRKA_NONCE_LENGTH, *len, 0) == -1) {
		vigil_log(LOG_NOTICE,
		    "[journal] entry %" PRIu64 " does not decrypt", seq);
		free(sealed);
		return (NULL);
	}

	if ((out = malloc(*len)) == NULL)
		fatal("malloc: failed to allocate journal entry");

	memcpy(out, sealed + KYRKA_NONCE_LENGTH, *len);

	nyfe_mem_zero(sealed, got);
	free(sealed);

	return (out);
}

/*
 * Our companion has everything up to the given seq, remember the seq and
 * remove those entries. A seq past our last one means our companion has
 * more than we know of, our next entries go after it. Returns -1 if we
 * could not remember the seq, we then keep the entries.
 */
int
vigil_journal_ack(struct vigil_journal *jr, u_int64_t seq)
{
	struct stat	st;
	u_int64_t	idx, be;
	char		path[1024];

	PRECOND(jr != NULL);

	if (seq <= jr->acked)
		return (0);

	if (jr->dir[0] != '\0') {
		if (journal_path(jr, path, sizeof(path), JOURNAL_ACKED) == -1)
			return (-1);

		be = htobe64(seq);

		if (journal_write(path, &be, sizeof(be)) == -1)
			return (-1);
	}

	for (idx = jr->acked + 1; jr->sealed && idx <= seq; idx++) {
		if (idx > jr->last)
			break;

		if (journal_entry(jr, idx, path, sizeof(path)) == -1)
			continue;

		if (stat(path, &st) == -1) {
			if (errno != ENOENT) {
				vigil_log(LOG_NOTICE, "[journal] stat(%s): %s",
				    path, errno_s);
			}
			continue;
		}

		if (unlink(path) == -1) {
			vigil_log(LOG_NOTICE, "[journal] unlink(%s): %s",
			    path, errno_s);
			continue;
		}

		jr->items--;
		jr->bytes -= st.st_size;
	}

	jr->acked = seq;

	if (seq > jr->last)
		jr->last = seq;

	return (0);
}

/*
 * Remember if our desktop holds a full copy of the chats of its phone.
 * Returns -1 if we could not remember it.
 */
int
vigil_journal_synced(struct vigil_journal *jr, u_int8_t synced)
{
	char		path[1024];

	PRECOND(jr != NULL);
	PRECOND(synced == VIGIL_JOURNAL_UNSYNCED ||
	    synced == VIGIL_JOURNAL_SYNCING ||
	    synced == VIGIL_JOURNAL_SYNCED);

	if (synced == jr->synced)
		return (0);

	if (jr->dir[0] != '\0') {
		if (journal_path(jr, path, sizeof(path), JOURNAL_SYNCED) == -1)
			return (-1);

		if (journal_write(path, &synced, sizeof(synced)) == -1)
			return (-1);
	}

	jr->synced = synced;

	return (0);
}

/*
 * Find the entries we still hold, entries at or below the acked seq are
 * left overs and go. Returns -1 if we cannot read the directory.
 */
static int
journal_scan(struct vigil_journal *jr)
{
	struct stat	st;
	int		ret;
	struct dirent	*dp;
	u_int64_t	seq;
	DIR		*dir;
	char		path[1024];

	PRECOND(jr != NULL);

	if ((dir = opendir(jr->dir)) == NULL) {
		vigil_log(LOG_NOTICE, "[journal] opendir(%s): %s",
		    jr->dir, errno_s);
		return (-1);
	}

	for (;;) {
		errno = 0;

		if ((dp = readdir(dir)) == NULL)
			break;

		if (journal_name(dp->d_name, &seq) == -1)
			continue;

		if (journal_path(jr, path, sizeof(path), dp->d_name) == -1)
			continue;

		if (seq <= jr->acked) {
			if (unlink(path) == -1) {
				vigil_log(LOG_NOTICE,
				    "[journal] unlink(%s): %s", path, errno_s);
			}
			continue;
		}

		if (stat(path, &st) == -1) {
			vigil_log(LOG_NOTICE, "[journal] stat(%s): %s",
			    path, errno_s);
			continue;
		}

		jr->items++;
		jr->bytes += st.st_size;

		if (seq > jr->last)
			jr->last = seq;
	}

	ret = 0;

	if (errno != 0) {
		vigil_log(LOG_NOTICE, "[journal] readdir(%s): %s",
		    jr->dir, errno_s);
		ret = -1;
	}

	if (closedir(dir) == -1) {
		vigil_log(LOG_NOTICE, "[journal] closedir(%s): %s",
		    jr->dir, errno_s);
		ret = -1;
	}

	return (ret);
}

/*
 * Seal or open len bytes of data in place with AES-256-GCM under the key
 * of the journal, the nonce sits in front of data and the tag after it.
 * The seq of the entry is the aad.
 */
static int
journal_cipher(struct vigil_journal *jr, u_int64_t seq, u_int8_t *nonce,
    u_int8_t *data, size_t len, int seal)
{
	int			ret;
	u_int64_t		aad;
	struct kyrka_cipher	cipher;

	PRECOND(jr != NULL);
	PRECOND(nonce != NULL);
	PRECOND(data != NULL);

	memset(&cipher, 0, sizeof(cipher));

	if ((cipher.ctx = kyrka_cipher_setup(jr->key, sizeof(jr->key))) == NULL)
		return (-1);

	aad = htobe64(seq);

	cipher.pt = data;
	cipher.ct = data;
	cipher.aad = &aad;
	cipher.nonce = nonce;
	cipher.data_len = len;
	cipher.tag = data + len;
	cipher.aad_len = sizeof(aad);
	cipher.nonce_len = KYRKA_NONCE_LENGTH;

	if (seal)
		ret = kyrka_cipher_encrypt(&cipher);
	else
		ret = kyrka_cipher_decrypt(&cipher);

	kyrka_cipher_cleanup(cipher.ctx);

	return (ret);
}

/*
 * Returns -1 if the given name is not that of an entry, else puts the seq
 * of the entry in seq.
 */
static int
journal_name(const char *name, u_int64_t *seq)
{
	int		idx;
	char		*ep;

	PRECOND(name != NULL);
	PRECOND(seq != NULL);

	if (strlen(name) != JOURNAL_NAME_LEN)
		return (-1);

	for (idx = 0; idx < JOURNAL_NAME_LEN; idx++) {
		if (!isxdigit((unsigned char)name[idx]))
			return (-1);
	}

	errno = 0;
	*seq = strtoull(name, &ep, 16);
	if (errno != 0 || *ep != '\0' || *seq == 0)
		return (-1);

	return (0);
}

/*
 * Put the path of the entry with the given seq in out.
 */
static int
journal_entry(struct vigil_journal *jr, u_int64_t seq, char *out, size_t len)
{
	int		ret;
	char		name[JOURNAL_NAME_LEN + 1];

	PRECOND(jr != NULL);
	PRECOND(out != NULL);

	ret = snprintf(name, sizeof(name), "%016" PRIx64, seq);
	if (ret < 0 || (size_t)ret >= sizeof(name))
		return (-1);

	return (journal_path(jr, out, len, name));
}

/*
 * Put the path of the given file in the journal directory in out.
 */
static int
journal_path(struct vigil_journal *jr, char *out, size_t len,
    const char *name)
{
	int		ret;

	PRECOND(jr != NULL);
	PRECOND(out != NULL);
	PRECOND(name != NULL);

	ret = snprintf(out, len, "%s/%s", jr->dir, name);
	if (ret < 0 || (size_t)ret >= len) {
		vigil_log(LOG_NOTICE, "[journal] path too long");
		return (-1);
	}

	return (0);
}

/*
 * Write the given data to the given path via a temporary file, the data
 * is on disk once we return 0.
 */
static int
journal_write(const char *path, const void *data, size_t len)
{
	ssize_t		ret;
	int		fd, n;
	char		tmp[1024];

	PRECOND(path != NULL);
	PRECOND(data != NULL);
	PRECOND(len > 0);

	n = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	if (n < 0 || (size_t)n >= sizeof(tmp))
		return (-1);

	if (unlink(tmp) == -1 && errno != ENOENT) {
		vigil_log(LOG_NOTICE, "[journal] unlink(%s): %s", tmp, errno_s);
		return (-1);
	}

	fd = open(tmp, O_CREAT | O_EXCL | O_TRUNC | O_WRONLY, 0600);
	if (fd == -1) {
		vigil_log(LOG_NOTICE, "[journal] open(%s): %s", tmp, errno_s);
		return (-1);
	}

	for (;;) {
		ret = write(fd, data, len);
		if (ret == -1) {
			if (errno == EINTR)
				continue;
			vigil_log(LOG_NOTICE, "[journal] write(%s): %s",
			    tmp, errno_s);
			goto cleanup;
		}

		if ((size_t)ret != len) {
			vigil_log(LOG_NOTICE, "[journal] short write on %s",
			    tmp);
			goto cleanup;
		}

		break;
	}

	if (fsync(fd) == -1) {
		vigil_log(LOG_NOTICE, "[journal] fsync(%s): %s", tmp, errno_s);
		goto cleanup;
	}

	if (journal_close(fd, tmp) == -1) {
		fd = -1;
		goto cleanup;
	}

	fd = -1;

	if (rename(tmp, path) == -1) {
		vigil_log(LOG_NOTICE, "[journal] rename(%s): %s", tmp, errno_s);
		goto cleanup;
	}

	return (0);

cleanup:
	if (fd != -1)
		(void)journal_close(fd, tmp);

	if (unlink(tmp) == -1)
		vigil_log(LOG_NOTICE, "[journal] unlink(%s): %s", tmp, errno_s);

	return (-1);
}

/*
 * Read the file at path if it is between min and max bytes long, the
 * caller wipes and frees what we return. Returns NULL with errno set to
 * ENOENT if there is no such file, NULL with any other errno on errors.
 */
static u_int8_t *
journal_load(const char *path, size_t min, size_t max, size_t *len)
{
	int		fd;
	struct stat	st;
	ssize_t		ret;
	size_t		off;
	u_int8_t	*data;

	PRECOND(path != NULL);
	PRECOND(min > 0 && min <= max);
	PRECOND(len != NULL);

	if ((fd = open(path, O_RDONLY)) == -1) {
		if (errno != ENOENT) {
			vigil_log(LOG_NOTICE, "[journal] open(%s): %s",
			    path, errno_s);
		}
		return (NULL);
	}

	if (fstat(fd, &st) == -1) {
		vigil_log(LOG_NOTICE, "[journal] fstat(%s): %s", path, errno_s);
		(void)journal_close(fd, path);
		errno = EIO;
		return (NULL);
	}

	if ((size_t)st.st_size < min || (size_t)st.st_size > max) {
		vigil_log(LOG_NOTICE, "[journal] %s has a bad size", path);
		(void)journal_close(fd, path);
		errno = EINVAL;
		return (NULL);
	}

	if ((data = malloc(st.st_size)) == NULL)
		fatal("malloc: failed to allocate journal file");

	off = 0;

	while (off < (size_t)st.st_size) {
		ret = read(fd, data + off, st.st_size - off);
		if (ret == -1) {
			if (errno == EINTR)
				continue;
			vigil_log(LOG_NOTICE, "[journal] read(%s): %s",
			    path, errno_s);
			break;
		}

		if (ret == 0) {
			vigil_log(LOG_NOTICE, "[journal] short read on %s",
			    path);
			break;
		}

		off += ret;
	}

	if (journal_close(fd, path) == -1 || off != (size_t)st.st_size) {
		nyfe_mem_zero(data, st.st_size);
		free(data);
		errno = EIO;
		return (NULL);
	}

	*len = off;

	return (data);
}

/*
 * Close the given fd of the file at path, returns -1 if that fails.
 */
static int
journal_close(int fd, const char *path)
{
	PRECOND(fd >= 0);
	PRECOND(path != NULL);

	if (close(fd) == -1) {
		vigil_log(LOG_NOTICE, "[journal] close(%s): %s", path, errno_s);
		return (-1);
	}

	return (0);
}
