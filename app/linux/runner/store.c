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

/*
 * The store of the desktop, everything in it is sealed under a random
 * root key that is wrapped under the passphrase of the user:
 *
 *	ikm = nyfe_passphrase_kdf(passphrase, salt, STORE_PASS_LABEL)
 *	K   = KMAC256(ikm, STORE_WRAP_LABEL), 256-bit
 *
 * Every seal is AES-256-GCM with a fresh random nonce under a key of its
 * own, KMAC256 derives it from the root key with the label of what is
 * sealed and a length prefixed context:
 *
 *	op	KMAC256(root, STORE_RECORD_LABEL, len || chat || seq),
 *		aad chat || seq
 *	data	KMAC256(root, STORE_DATA_LABEL, len || name), aad name
 *	config	KMAC256(root, STORE_CONFIG_LABEL, len || ""), aad ""
 *
 * As on the phone every chat has a log of its own in chats/<chat>, the
 * data of its records is in data/<chat>-<ref>.dat and our state (groups,
 * names, avatars, prefs, who we are) is a file per item in state/. A log
 * holds the ops of its chat one after another as len || nonce || ct ||
 * tag with len as a 32-bit big endian number and seq the number of the op
 * in the log, an op that does not open cuts the log off there.
 */

#include <sys/types.h>
#include <sys/queue.h>
#include <sys/stat.h>

#include <endian.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sodium.h>

#include "vigil.h"
#include "runner.h"

#include <libkyrka/libnyfe.h>

/* The labels for our keys. */
#define STORE_PASS_LABEL	"VIGIL.COMPANION.PASSPHRASE"
#define STORE_WRAP_LABEL	"VIGIL.COMPANION.WRAP"
#define STORE_RECORD_LABEL	"VIGIL.COMPANION.RECORD"
#define STORE_DATA_LABEL	"VIGIL.COMPANION.DATA"
#define STORE_CONFIG_LABEL	"VIGIL.COMPANION.CONFIG"

/* The magic and version of our key file. */
#define STORE_MAGIC		"VGCK"
#define STORE_VERSION		1

/* The length of our keys, a nonce and a tag. */
#define STORE_KEY_LEN		32
#define STORE_NONCE_LEN		crypto_aead_aes256gcm_NPUBBYTES
#define STORE_TAG_LEN		crypto_aead_aes256gcm_ABYTES

/* What a seal adds to its plaintext. */
#define STORE_SEAL_LEN		(STORE_NONCE_LEN + STORE_TAG_LEN)

/* The largest record we write or read. */
#define STORE_RECORD_MAX	(1024 * 1024)

/* The largest piece of data we keep. */
#define STORE_DATA_MAX		(VIGIL_CHAT_DATA_MAX + STORE_SEAL_LEN)

/* The longest context of an op in the log of a chat, the chat and seq. */
#define STORE_CHAT_CTX		(STORE_CHAT_MAX - 1 + sizeof(u_int64_t))

/* The record we hand the user interface for an avatar we keep. */
#define STORE_AVATAR_RECORD	\
    "{\"file\":\"%s\",\"events\":[{\"op\":\"avatar\",\"peer\":\"%s\"}]}"

/*
 * What we know of the log of a chat, the number of records in it.
 */
struct store_chat {
	char			chat[STORE_CHAT_MAX];
	u_int64_t		count;
	LIST_ENTRY(store_chat)	list;
};

LIST_HEAD(store_chat_list, store_chat);

/*
 * The JSON list of the peers that have an avatar.
 */
struct store_peers {
	const char	*json;
	size_t		len;
};

/*
 * The JSON we build for the user interface.
 */
struct store_out {
	char		*buf;
	size_t		used;
	size_t		room;
	size_t		records;
};

/*
 * The header of the file our root key is wrapped in, it is the aad.
 */
struct store_key_hdr {
	u_int8_t	magic[4];
	u_int32_t	version;
	u_int8_t	salt[NYFE_KEY_FILE_SALT_LEN];
	u_int8_t	nonce[STORE_NONCE_LEN];
} __attribute__((packed));

/*
 * The file our root key is wrapped in.
 */
struct store_key_file {
	struct store_key_hdr	hdr;
	u_int8_t		root[STORE_KEY_LEN];
	u_int8_t		tag[STORE_TAG_LEN];
} __attribute__((packed));

static int	store_dir(char *, size_t);
static int	store_data_has(const char *);
static void	store_unlink(const char *);
static int	store_hex(const char *, size_t);
static int	store_mkdir(const char *, const char *);
static int	store_unwrap(struct store_key_file *, const char *, size_t);
static int	store_data_path(const char *, char *, size_t);
static int	store_write(const char *, const void *, size_t);
static void	store_key(const char *, const void *, size_t, u_int8_t *);
static void	store_wrap(const char *, size_t, const u_int8_t *, u_int8_t *);
static void	store_seal(const u_int8_t *, const void *, size_t,
		    const void *, size_t, u_int8_t *, size_t);
static int	store_unseal(const u_int8_t *, const void *, size_t,
		    const u_int8_t *, size_t, u_int8_t *, size_t);
static void	store_each(const char *,
		    void (*)(const char *, const char *, void *), void *);

static void	store_drop_of(const char *, const char *, void *);
static void	store_drop_any(const char *, const char *, void *);
static void	store_drop_peers(const char *, const char *, void *);
static void	store_drop_faces(const char *, const char *, void *);

static void	store_state_each(const char *, const char *, void *);
static void	store_state_add(struct store_out *, const char *);

static void	store_out_next(struct store_out *);
static void	store_out_add(struct store_out *, const char *, size_t);
static void	store_out_record(struct store_out *, const char *, size_t);

static void	store_chat_each(const char *, const char *, void *);
static void	store_chat_count(const char *, const char *, void *);
static void	store_chat_legacy(const char *, const char *, void *);
static void	store_data_legacy(const char *, const char *, void *);
static int	store_chat_path(const char *, char *, size_t);
static struct store_chat	*store_chat_find(const char *);
static size_t	store_chat_ctx(const char *, u_int64_t, u_int8_t *, size_t);
static void	store_chat_seal(const char *, u_int64_t, const char *, size_t,
		    u_int8_t *);
static int	store_chat_unseal(const char *, u_int64_t, const u_int8_t *,
		    size_t, u_int8_t *);

/* Our root key, only set once the user unlocked us. */
static int		unlocked = 0;
static u_int8_t		root[STORE_KEY_LEN];

/* The logs of our chats we know of. */
static struct store_chat_list	chats;

/*
 * Returns 1 if there is a key file, so the user picked a passphrase.
 */
int
store_exists(void)
{
	struct stat	st;
	char		path[1024];

	if (store_path(path, sizeof(path), "key") == -1)
		return (0);

	return (stat(path, &st) == 0);
}

/*
 * Make a new root key and wrap it under the given passphrase.
 */
int
store_create(const char *pass, size_t len)
{
	struct store_key_file	key;
	int			fd, ret;
	char			path[1024];
	u_int8_t		wrap[STORE_KEY_LEN];

	PRECOND(pass != NULL);
	PRECOND(len > 0);
	PRECOND(unlocked == 0);

	if (store_path(path, sizeof(path), "key") == -1)
		return (-1);

	memset(&key, 0, sizeof(key));
	memcpy(key.hdr.magic, STORE_MAGIC, sizeof(key.hdr.magic));
	key.hdr.version = htobe32(STORE_VERSION);

	store_random(root, sizeof(root));
	store_random(key.hdr.salt, sizeof(key.hdr.salt));
	store_random(key.hdr.nonce, sizeof(key.hdr.nonce));

	store_wrap(pass, len, key.hdr.salt, wrap);

	if (crypto_aead_aes256gcm_encrypt_detached(key.root, key.tag, NULL,
	    root, sizeof(root), (const u_int8_t *)&key.hdr, sizeof(key.hdr),
	    NULL, key.hdr.nonce, wrap) != 0) {
		sodium_memzero(wrap, sizeof(wrap));
		sodium_memzero(root, sizeof(root));
		return (-1);
	}

	sodium_memzero(wrap, sizeof(wrap));

	if ((fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600)) == -1) {
		fprintf(stderr, "vigil: open(%s): %s\n", path, strerror(errno));
		return (-1);
	}

	ret = write(fd, &key, sizeof(key));
	if (ret == -1 || (size_t)ret != sizeof(key) || fsync(fd) == -1) {
		fprintf(stderr, "vigil: write(%s) failed\n", path);
		(void)close(fd);
		(void)unlink(path);
		return (-1);
	}

	(void)close(fd);

	LIST_INIT(&chats);
	unlocked = 1;

	return (0);
}

/*
 * Unwrap our root key with the given passphrase, returns -1 if it is
 * not the right one.
 */
int
store_open(const char *pass, size_t len)
{
	size_t			got;
	struct store_key_file	key;
	u_int8_t		*data;
	char			path[1024];

	PRECOND(pass != NULL);
	PRECOND(len > 0);
	PRECOND(unlocked == 0);

	if (store_path(path, sizeof(path), "key") == -1)
		return (-1);

	if ((data = store_read(path, sizeof(key), &got)) == NULL)
		return (-1);

	if (got != sizeof(key)) {
		free(data);
		return (-1);
	}

	memcpy(&key, data, sizeof(key));
	free(data);

	if (store_unwrap(&key, pass, len) == -1)
		return (-1);

	LIST_INIT(&chats);
	unlocked = 1;

	store_each("chats", store_chat_count, NULL);

	return (0);
}

/*
 * Returns 1 if we hold a chat or data named from before our phone ran more
 * than one flock, a full resync from our phone replaces it.
 */
int
store_legacy(void)
{
	int		found;

	PRECOND(unlocked);

	found = 0;
	store_each("chats", store_chat_legacy, &found);
	store_each("data", store_data_legacy, &found);
	store_each("state", store_data_legacy, &found);

	return (found);
}

/*
 * Note a chat in the given directory whose name is not one we use.
 */
static void
store_chat_legacy(const char *dir, const char *chat, void *arg)
{
	int		*found;

	PRECOND(dir != NULL);
	PRECOND(chat != NULL);
	PRECOND(arg != NULL);

	found = arg;

	if (store_chat_valid(chat) == -1)
		*found = 1;
}

/*
 * Note data in the given directory under a name we do not use.
 */
static void
store_data_legacy(const char *dir, const char *name, void *arg)
{
	int		*found;
	char		path[1024];

	PRECOND(dir != NULL);
	PRECOND(name != NULL);
	PRECOND(arg != NULL);

	found = arg;

	if (store_data_path(name, path, sizeof(path)) == -1)
		*found = 1;
}

/*
 * Seal the given op and add it to the end of the log of the given chat,
 * it is on disk when we return 0.
 */
int
store_chat_append(const char *chat, const char *json, size_t len)
{
	int			fd;
	off_t			end;
	ssize_t			ret;
	u_int32_t		hdr;
	struct store_chat	*sc;
	u_int8_t		*out;
	size_t			total;
	char			path[1024];

	PRECOND(chat != NULL);
	PRECOND(json != NULL);
	PRECOND(len > 0);
	PRECOND(unlocked);

	if (len > STORE_RECORD_MAX)
		return (-1);

	if (store_chat_path(chat, path, sizeof(path)) == -1)
		return (-1);

	if ((sc = store_chat_find(chat)) == NULL) {
		if ((sc = calloc(1, sizeof(*sc))) == NULL)
			fatal("calloc");
		(void)snprintf(sc->chat, sizeof(sc->chat), "%s", chat);
		LIST_INSERT_HEAD(&chats, sc, list);
	}

	total = sizeof(hdr) + len + STORE_SEAL_LEN;

	if ((out = malloc(total)) == NULL)
		fatal("malloc");

	hdr = htobe32(len + STORE_SEAL_LEN);
	memcpy(out, &hdr, sizeof(hdr));
	store_chat_seal(chat, sc->count, json, len, out + sizeof(hdr));

	fd = open(path, O_CREAT | O_WRONLY | O_APPEND, 0600);
	if (fd == -1) {
		fprintf(stderr, "vigil: open(%s): %s\n", path, strerror(errno));
		free(out);
		return (-1);
	}

	if ((end = lseek(fd, 0, SEEK_END)) == -1) {
		fprintf(stderr, "vigil: lseek: %s\n", strerror(errno));
		(void)close(fd);
		free(out);
		return (-1);
	}

	ret = write(fd, out, total);
	free(out);

	if (ret == -1 || (size_t)ret != total || fdatasync(fd) == -1) {
		fprintf(stderr, "vigil: failed to write to %s\n", path);
		if (ftruncate(fd, end) == -1)
			fatal("ftruncate: %s, the log of %s is broken",
			    strerror(errno), chat);
		(void)close(fd);
		return (-1);
	}

	(void)close(fd);
	sc->count++;

	return (0);
}

/*
 * Returns 1 if we have a log for the given chat.
 */
int
store_chat_exists(const char *chat)
{
	struct stat	st;
	char		path[1024];

	PRECOND(chat != NULL);

	if (store_chat_path(chat, path, sizeof(path)) == -1)
		return (0);

	return (stat(path, &st) == 0);
}

/*
 * Drop the log of the given chat, its data stays.
 */
void
store_chat_drop(const char *chat)
{
	struct store_chat	*sc;
	char			path[1024];

	PRECOND(chat != NULL);
	PRECOND(unlocked);

	if (store_chat_path(chat, path, sizeof(path)) == -1)
		return;

	store_unlink(path);

	if ((sc = store_chat_find(chat)) != NULL) {
		LIST_REMOVE(sc, list);
		free(sc);
	}
}

/*
 * Drop everything we keep of the given chat: its log, its data and the
 * group info if it is a group.
 */
void
store_chat_delete(const char *chat)
{
	int		ret;
	char		id[STORE_CHAT_MAX];
	char		name[STORE_NAME_MAX];

	PRECOND(chat != NULL);
	PRECOND(unlocked);

	ret = snprintf(id, sizeof(id), "%s", chat);
	if (ret < 0 || (size_t)ret >= sizeof(id))
		return;

	store_chat_drop(id);
	store_each("data", store_drop_of, id);

	ret = snprintf(name, sizeof(name), "group-%s", chat);
	if (ret > 0 && (size_t)ret < sizeof(name))
		store_data_drop(name);
}

/*
 * Drop all chats, their data and all state but our settings and who we
 * are, a full resync of our phone follows.
 */
void
store_reset(void)
{
	struct store_chat	*sc;

	PRECOND(unlocked);

	store_each("chats", store_drop_any, NULL);
	store_each("data", store_drop_any, NULL);
	store_each("state", store_drop_peers, NULL);

	while ((sc = LIST_FIRST(&chats)) != NULL) {
		LIST_REMOVE(sc, list);
		free(sc);
	}
}

/*
 * Drop the avatars of the peers that are not in the given len bytes of
 * JSON that list the peers with an avatar.
 */
void
store_avatars(const char *peers, size_t len)
{
	struct store_peers	list;

	PRECOND(peers != NULL);
	PRECOND(unlocked);

	list.json = peers;
	list.len = len;

	store_each("state", store_drop_faces, &list);
}

/*
 * Hand back everything we keep as a JSON array of records for the user
 * interface to replay, our state first, then the logs of our chats. A
 * record in a log that does not open cuts that log off there.
 */
char *
store_records(size_t *outlen)
{
	struct store_out	out;

	PRECOND(outlen != NULL);
	PRECOND(unlocked);

	memset(&out, 0, sizeof(out));
	store_out_add(&out, "[", 1);

	store_state_add(&out, "prefs");
	store_state_add(&out, "self");
	store_each("state", store_state_each, &out);
	store_each("chats", store_chat_each, &out);

	store_out_add(&out, "]", 1);

	*outlen = out.used;

	return (out.buf);
}

/*
 * Hand the state item with the given name to out as a record, an avatar
 * as an avatar op with its name as the file.
 */
static void
store_state_add(struct store_out *out, const char *name)
{
	int		ret;
	size_t		len;
	u_int8_t	*data;
	const char	*file;
	char		rec[128];

	PRECOND(out != NULL);
	PRECOND(name != NULL);

	if (!strncmp(name, "avatar-", 7)) {
		if (!store_data_has(name))
			return;

		ret = snprintf(rec, sizeof(rec), STORE_AVATAR_RECORD,
		    name, name + 7);
		if (ret > 0 && (size_t)ret < sizeof(rec)) {
			store_out_next(out);
			store_out_add(out, rec, ret);
		}
		return;
	}

	if ((data = store_data_get(name, &len)) == NULL)
		return;

	file = "";

	if (!strcmp(name, "self") && store_data_has("self-avatar"))
		file = "self-avatar";

	ret = snprintf(rec, sizeof(rec), "{\"file\":\"%s\",\"events\":[", file);
	if (ret < 0 || (size_t)ret >= sizeof(rec))
		fatal("record head too long");

	store_out_next(out);
	store_out_add(out, rec, ret);
	store_out_add(out, (const char *)data, len);
	store_out_add(out, "]}", 2);

	sodium_memzero(data, len);
	free(data);
}

/*
 * Hand the names, avatars and groups in our state to the out we get.
 */
static void
store_state_each(const char *dir, const char *name, void *arg)
{
	PRECOND(name != NULL);
	PRECOND(arg != NULL);

	if (!strncmp(name, "name-", 5) || !strncmp(name, "avatar-", 7) ||
	    !strncmp(name, "group-", 6))
		store_state_add(arg, name);
}

/*
 * Hand every op in the log of the given chat to the out we get, cut the
 * log off at a record that does not open and remember how many records
 * the log holds.
 */
static void
store_chat_each(const char *dir, const char *chat, void *arg)
{
	u_int32_t		len;
	struct store_chat	*sc;
	struct store_out	*out;
	u_int64_t		count;
	size_t			size, off;
	char			path[1024];
	u_int8_t		*data, *plain;

	PRECOND(chat != NULL);
	PRECOND(arg != NULL);

	out = arg;

	if (store_chat_valid(chat) == -1)
		return;

	if (store_chat_path(chat, path, sizeof(path)) == -1)
		return;

	if ((data = store_read(path, SIZE_MAX, &size)) == NULL)
		return;

	if ((plain = malloc(STORE_RECORD_MAX)) == NULL)
		fatal("malloc");

	off = 0;
	count = 0;

	while (size - off >= sizeof(len)) {
		memcpy(&len, data + off, sizeof(len));
		len = be32toh(len);

		if (len <= STORE_SEAL_LEN || len > STORE_RECORD_MAX +
		    STORE_SEAL_LEN || len > size - off - sizeof(len))
			break;

		if (store_chat_unseal(chat, count, data + off + sizeof(len),
		    len, plain) == -1)
			break;

		store_out_record(out, (const char *)plain,
		    len - STORE_SEAL_LEN);

		off += sizeof(len) + len;
		count++;
	}

	sodium_memzero(plain, STORE_RECORD_MAX);
	free(plain);

	sodium_memzero(data, size);
	free(data);

	if (off != size) {
		fprintf(stderr, "vigil: log of %s damaged after %" PRIu64
		    " records, cut off\n", chat, count);
		if (truncate(path, off) == -1)
			fatal("truncate: %s", strerror(errno));
	}

	if ((sc = store_chat_find(chat)) != NULL)
		sc->count = count;
}

/*
 * Count the records in the log of the given chat so our appends get the
 * right seq, a record cut short at the end of the log is cut off.
 */
static void
store_chat_count(const char *dir, const char *chat, void *arg)
{
	u_int32_t		len;
	struct store_chat	*sc;
	u_int8_t		*data;
	u_int64_t		count;
	size_t			size, off;
	char			path[1024];

	PRECOND(chat != NULL);

	if (store_chat_path(chat, path, sizeof(path)) == -1)
		return;

	if ((data = store_read(path, SIZE_MAX, &size)) == NULL)
		return;

	off = 0;
	count = 0;

	while (size - off >= sizeof(len)) {
		memcpy(&len, data + off, sizeof(len));
		len = be32toh(len);

		if (len <= STORE_SEAL_LEN || len > STORE_RECORD_MAX +
		    STORE_SEAL_LEN || len > size - off - sizeof(len))
			break;

		off += sizeof(len) + len;
		count++;
	}

	free(data);

	if (off != size && truncate(path, off) == -1)
		fatal("truncate: %s", strerror(errno));

	if ((sc = calloc(1, sizeof(*sc))) == NULL)
		fatal("calloc");

	(void)snprintf(sc->chat, sizeof(sc->chat), "%s", chat);
	sc->count = count;

	LIST_INSERT_HEAD(&chats, sc, list);
}

/*
 * Add the given op to out as a record without a file.
 */
static void
store_out_record(struct store_out *out, const char *op, size_t len)
{
	PRECOND(out != NULL);
	PRECOND(op != NULL);

	store_out_next(out);
	store_out_add(out, "{\"file\":\"\",\"events\":[", 21);
	store_out_add(out, op, len);
	store_out_add(out, "]}", 2);
}

/*
 * Start the next record in out, after a comma if it is not the first.
 */
static void
store_out_next(struct store_out *out)
{
	PRECOND(out != NULL);

	if (out->records++ > 0)
		store_out_add(out, ",", 1);
}

/*
 * Add len bytes of str to out.
 */
static void
store_out_add(struct store_out *out, const char *str, size_t len)
{
	char		*grown;
	size_t		need, room;

	PRECOND(out != NULL);
	PRECOND(str != NULL);

	need = out->used + len + 1;

	if (need > out->room) {
		room = out->room * 2;
		if (room < need)
			room = need + 4096;

		if ((grown = malloc(room)) == NULL)
			fatal("malloc");

		if (out->buf != NULL) {
			memcpy(grown, out->buf, out->used);
			sodium_memzero(out->buf, out->room);
			free(out->buf);
		}

		out->buf = grown;
		out->room = room;
	}

	memcpy(out->buf + out->used, str, len);
	out->used += len;
}

/*
 * Call cb for every file in the given directory of our store.
 */
static void
store_each(const char *dir, void (*cb)(const char *, const char *, void *),
    void *arg)
{
	DIR		*dp;
	struct dirent	*ent;
	char		path[1024];

	PRECOND(dir != NULL);
	PRECOND(cb != NULL);

	if (store_path(path, sizeof(path), dir) == -1)
		return;

	if ((dp = opendir(path)) == NULL) {
		fprintf(stderr, "vigil: opendir(%s): %s\n", path,
		    strerror(errno));
		return;
	}

	while ((ent = readdir(dp)) != NULL) {
		if (ent->d_name[0] == '.')
			continue;
		cb(dir, ent->d_name, arg);
	}

	(void)closedir(dp);
}

/*
 * Drop the data of a record of the chat we get.
 */
static void
store_drop_of(const char *dir, const char *name, void *arg)
{
	size_t		len;
	const char	*chat;

	PRECOND(name != NULL);
	PRECOND(arg != NULL);

	chat = arg;
	len = strlen(chat);

	if (!strncmp(name, chat, len) && name[len] == '-')
		store_data_drop(name);
}

/*
 * Drop any file in the directory we get.
 */
static void
store_drop_any(const char *dir, const char *name, void *arg)
{
	int		ret;
	char		path[1024];
	char		file[STORE_NAME_MAX + 8];

	PRECOND(dir != NULL);
	PRECOND(name != NULL);

	ret = snprintf(file, sizeof(file), "%s/%s", dir, name);
	if (ret < 0 || (size_t)ret >= sizeof(file))
		return;

	if (store_path(path, sizeof(path), file) == -1)
		return;

	store_unlink(path);
}

/*
 * Drop the group infos, names and avatars of peers in our state.
 */
static void
store_drop_peers(const char *dir, const char *name, void *arg)
{
	PRECOND(name != NULL);

	if (!strncmp(name, "name-", 5) || !strncmp(name, "avatar-", 7) ||
	    !strncmp(name, "group-", 6))
		store_drop_any(dir, name, NULL);
}

/*
 * Drop an avatar of a peer that is not in the JSON list we get.
 */
static void
store_drop_faces(const char *dir, const char *name, void *arg)
{
	int			len;
	struct store_peers	*list;
	char			want[STORE_PEER_MAX + 2];

	PRECOND(name != NULL);
	PRECOND(arg != NULL);

	list = arg;

	if (strncmp(name, "avatar-", 7) || store_peer_valid(name + 7) == -1)
		return;

	len = snprintf(want, sizeof(want), "\"%s\"", name + 7);
	if (len < 0 || (size_t)len >= sizeof(want))
		return;

	if (memmem(list->json, list->len, want, len) == NULL)
		store_data_drop(name);
}

/*
 * Returns 1 if we keep data under the given name.
 */
static int
store_data_has(const char *name)
{
	struct stat	st;
	char		path[1024];

	PRECOND(name != NULL);

	if (store_data_path(name, path, sizeof(path)) == -1)
		return (0);

	return (stat(path, &st) == 0);
}

/*
 * Find what we know of the log of the given chat.
 */
static struct store_chat *
store_chat_find(const char *chat)
{
	struct store_chat	*sc;

	PRECOND(chat != NULL);

	LIST_FOREACH(sc, &chats, list) {
		if (!strcmp(sc->chat, chat))
			return (sc);
	}

	return (NULL);
}

/*
 * Seal the op with the given seq in the log of the given chat into out,
 * which has room for len + STORE_SEAL_LEN bytes.
 */
static void
store_chat_seal(const char *chat, u_int64_t seq, const char *json,
    size_t len, u_int8_t *out)
{
	size_t		ctxlen;
	u_int8_t	key[STORE_KEY_LEN];
	u_int8_t	ctx[STORE_CHAT_CTX];

	PRECOND(chat != NULL);
	PRECOND(json != NULL);
	PRECOND(out != NULL);

	ctxlen = store_chat_ctx(chat, seq, ctx, sizeof(ctx));

	store_key(STORE_RECORD_LABEL, ctx, ctxlen, key);
	store_seal(key, ctx, ctxlen, json, len, out, len + STORE_SEAL_LEN);
	sodium_memzero(key, sizeof(key));
}

/*
 * Open the sealed op with the given seq of the log of the given chat into
 * out, which has room for STORE_RECORD_MAX bytes.
 */
static int
store_chat_unseal(const char *chat, u_int64_t seq, const u_int8_t *in,
    size_t len, u_int8_t *out)
{
	int		ret;
	size_t		ctxlen;
	u_int8_t	key[STORE_KEY_LEN];
	u_int8_t	ctx[STORE_CHAT_CTX];

	PRECOND(chat != NULL);
	PRECOND(in != NULL);
	PRECOND(out != NULL);

	ctxlen = store_chat_ctx(chat, seq, ctx, sizeof(ctx));

	store_key(STORE_RECORD_LABEL, ctx, ctxlen, key);
	ret = store_unseal(key, ctx, ctxlen, in, len, out, STORE_RECORD_MAX);
	sodium_memzero(key, sizeof(key));

	return (ret);
}

/*
 * Put the context of the op with the given seq in the log of the given
 * chat in ctx, the chat and the seq as a 64-bit big endian number.
 */
static size_t
store_chat_ctx(const char *chat, u_int64_t seq, u_int8_t *ctx, size_t len)
{
	size_t		chatlen;

	PRECOND(chat != NULL);
	PRECOND(ctx != NULL);

	chatlen = strlen(chat);
	PRECOND(chatlen + sizeof(seq) <= len);

	seq = htobe64(seq);
	memcpy(ctx, chat, chatlen);
	memcpy(ctx + chatlen, &seq, sizeof(seq));

	return (chatlen + sizeof(seq));
}

/*
 * Put the path of the log of the given chat in out, returns -1 if the
 * chat is not one.
 */
static int
store_chat_path(const char *chat, char *out, size_t len)
{
	char		file[STORE_NAME_MAX + 8];

	PRECOND(chat != NULL);
	PRECOND(out != NULL);

	if (store_chat_valid(chat) == -1)
		return (-1);

	(void)snprintf(file, sizeof(file), "chats/%s", chat);

	return (store_path(out, len, file));
}

/*
 * Returns 0 if the given name is that of a chat, a peer as in
 * store_peer_valid() or a flock in hex, a dash, a g and 18 hex digits for
 * a group.
 */
int
store_chat_valid(const char *chat)
{
	size_t		len;
	const char	*dash;

	PRECOND(chat != NULL);

	if (store_peer_valid(chat) == 0)
		return (0);

	if ((dash = strchr(chat, '-')) == NULL)
		return (-1);

	len = dash - chat;

	if (len == 0 || len > 16 || store_hex(chat, len) == -1)
		return (-1);

	if (strlen(dash + 1) != 19 || dash[1] != 'g')
		return (-1);

	return (store_hex(dash + 2, 18));
}

/*
 * Returns 0 if the given name is that of a peer, a flock in hex, a dash
 * and two hex digits.
 */
int
store_peer_valid(const char *peer)
{
	size_t		len;
	const char	*dash;

	PRECOND(peer != NULL);

	if ((dash = strchr(peer, '-')) == NULL)
		return (-1);

	len = dash - peer;

	if (len == 0 || len > 16 || store_hex(peer, len) == -1)
		return (-1);

	if (strlen(dash + 1) != 2)
		return (-1);

	return (store_hex(dash + 1, 2));
}

/*
 * Returns 0 if the first len bytes of str are lower case hex digits.
 */
static int
store_hex(const char *str, size_t len)
{
	size_t		idx;

	PRECOND(str != NULL);

	for (idx = 0; idx < len; idx++) {
		if (!isdigit((unsigned char)str[idx]) &&
		    (str[idx] < 'a' || str[idx] > 'f'))
			return (-1);
	}

	return (0);
}

/*
 * Unlink the file at the given path, it may not be there.
 */
static void
store_unlink(const char *path)
{
	PRECOND(path != NULL);

	if (unlink(path) == -1 && errno != ENOENT)
		fprintf(stderr, "vigil: unlink(%s): %s\n", path,
		    strerror(errno));
}

/*
 * Return the companion configuration we keep, NULL if we have none.
 */
char *
store_config(size_t *len)
{
	size_t		size;
	char		path[1024];
	u_int8_t	*data, *out;
	u_int8_t	key[STORE_KEY_LEN];

	PRECOND(len != NULL);
	PRECOND(unlocked);

	if (store_path(path, sizeof(path), "config") == -1)
		return (NULL);

	if ((data = store_read(path, VIGIL_CONFIG_MAX + STORE_SEAL_LEN,
	    &size)) == NULL)
		return (NULL);

	if (size <= STORE_SEAL_LEN) {
		free(data);
		return (NULL);
	}

	if ((out = calloc(1, size)) == NULL)
		fatal("calloc");

	store_key(STORE_CONFIG_LABEL, "", 0, key);

	if (store_unseal(key, "", 0, data, size, out, size) == -1) {
		sodium_memzero(key, sizeof(key));
		free(data);
		free(out);
		return (NULL);
	}

	sodium_memzero(key, sizeof(key));
	free(data);

	*len = size - STORE_SEAL_LEN;

	return ((char *)out);
}

/*
 * Seal and keep the given companion configuration.
 */
int
store_config_save(const char *cfg, size_t len)
{
	int		ret;
	u_int8_t	*out;
	char		path[1024];
	u_int8_t	key[STORE_KEY_LEN];

	PRECOND(cfg != NULL);
	PRECOND(len > 0 && len <= VIGIL_CONFIG_MAX);
	PRECOND(unlocked);

	if (store_path(path, sizeof(path), "config") == -1)
		return (-1);

	if ((out = malloc(len + STORE_SEAL_LEN)) == NULL)
		fatal("malloc");

	store_key(STORE_CONFIG_LABEL, "", 0, key);
	store_seal(key, "", 0, cfg, len, out, len + STORE_SEAL_LEN);
	sodium_memzero(key, sizeof(key));

	ret = store_write(path, out, len + STORE_SEAL_LEN);
	free(out);

	return (ret);
}

/*
 * Drop the companion configuration we keep.
 */
void
store_config_drop(void)
{
	char		path[1024];

	if (store_path(path, sizeof(path), "config") == -1)
		return;

	if (unlink(path) == -1 && errno != ENOENT)
		fprintf(stderr, "vigil: unlink(%s): %s\n", path,
		    strerror(errno));
}

/*
 * Seal and keep the given data under the given name.
 */
int
store_data_put(const char *name, const u_int8_t *data, size_t len)
{
	int		ret;
	u_int8_t	*out;
	u_int8_t	key[STORE_KEY_LEN];
	char		path[1024];

	PRECOND(name != NULL);
	PRECOND(data != NULL);
	PRECOND(len > 0 && len <= VIGIL_CHAT_DATA_MAX);
	PRECOND(unlocked);

	if (store_data_path(name, path, sizeof(path)) == -1)
		return (-1);

	if ((out = malloc(len + STORE_SEAL_LEN)) == NULL)
		fatal("malloc");

	store_key(STORE_DATA_LABEL, name, strlen(name), key);
	store_seal(key, name, strlen(name), data, len, out,
	    len + STORE_SEAL_LEN);
	sodium_memzero(key, sizeof(key));

	ret = store_write(path, out, len + STORE_SEAL_LEN);
	free(out);

	return (ret);
}

/*
 * Wipe the data we keep under the given name, if any.
 */
void
store_data_drop(const char *name)
{
	char		path[1024];

	PRECOND(name != NULL);

	if (store_data_path(name, path, sizeof(path)) == -1)
		return;

	if (unlink(path) == -1 && errno != ENOENT)
		fprintf(stderr, "vigil: unlink(%s): %s\n", path,
		    strerror(errno));
}

/*
 * Return the data we keep under the given name, NULL if we have none.
 */
u_int8_t *
store_data_get(const char *name, size_t *len)
{
	size_t		size;
	u_int8_t	*data, *out;
	u_int8_t	key[STORE_KEY_LEN];
	char		path[1024];

	PRECOND(name != NULL);
	PRECOND(len != NULL);
	PRECOND(unlocked);

	if (store_data_path(name, path, sizeof(path)) == -1)
		return (NULL);

	if ((data = store_read(path, STORE_DATA_MAX, &size)) == NULL)
		return (NULL);

	if (size <= STORE_SEAL_LEN) {
		free(data);
		return (NULL);
	}

	if ((out = malloc(size)) == NULL)
		fatal("malloc");

	store_key(STORE_DATA_LABEL, name, strlen(name), key);

	if (store_unseal(key, name, strlen(name), data, size,
	    out, size) == -1) {
		sodium_memzero(key, sizeof(key));
		free(data);
		free(out);
		return (NULL);
	}

	sodium_memzero(key, sizeof(key));
	free(data);

	*len = size - STORE_SEAL_LEN;

	return (out);
}

/*
 * Fill buf with len random bytes from nyfe.
 */
void
store_random(void *buf, size_t len)
{
	PRECOND(buf != NULL);
	PRECOND(len > 0);

	vigil_kyrka_enter();
	nyfe_random_bytes(buf, len);
	vigil_kyrka_leave();
}

/*
 * Put the path of the given file in our directory into out.
 */
int
store_path(char *out, size_t len, const char *name)
{
	int		ret;
	char		dir[1024];

	PRECOND(out != NULL);
	PRECOND(name != NULL);

	if (store_dir(dir, sizeof(dir)) == -1)
		return (-1);

	ret = snprintf(out, len, "%s/%s", dir, name);
	if (ret < 0 || (size_t)ret >= len)
		return (-1);

	return (0);
}

/*
 * Put the path of what we keep under the given name in out, returns -1 if
 * the name is not one we keep. The data of a record is <chat>-<ref>.dat
 * in data, as on the phone, our state is in state: prefs, self, the
 * self-avatar, and group-<chat>, name-<peer> and avatar-<peer>.
 */
static int
store_data_path(const char *name, char *out, size_t len)
{
	size_t		namelen;
	char		chat[STORE_CHAT_MAX];
	const char	*dir, *dash;
	char		file[STORE_NAME_MAX + 8];

	PRECOND(name != NULL);
	PRECOND(out != NULL);

	namelen = strlen(name);

	if (namelen == 0 || namelen >= STORE_NAME_MAX)
		return (-1);

	dir = "state";

	if (!strncmp(name, "group-", 6)) {
		if (store_chat_valid(name + 6) == -1)
			return (-1);

		if (store_peer_valid(name + 6) == 0)
			return (-1);
	} else if (!strncmp(name, "name-", 5)) {
		if (store_peer_valid(name + 5) == -1)
			return (-1);
	} else if (!strncmp(name, "avatar-", 7)) {
		if (store_peer_valid(name + 7) == -1)
			return (-1);
	} else if (strcmp(name, "prefs") && strcmp(name, "self") &&
	    strcmp(name, "self-avatar")) {
		if ((dash = strrchr(name, '-')) == NULL)
			return (-1);

		if ((size_t)(dash - name) >= sizeof(chat))
			return (-1);

		memcpy(chat, name, dash - name);
		chat[dash - name] = '\0';

		if (store_chat_valid(chat) == -1)
			return (-1);

		if (strlen(dash + 1) != 20 || store_hex(dash + 1, 16) == -1 ||
		    strcmp(dash + 17, ".dat"))
			return (-1);

		dir = "data";
	}

	(void)snprintf(file, sizeof(file), "%s/%s", dir, name);

	return (store_path(out, len, file));
}

/*
 * Put our directory in out and make sure it exists, it is
 * $VIGIL_COMPANION_HOME if set, otherwise vigil-companion in the data
 * directory of the user.
 */
static int
store_dir(char *out, size_t len)
{
	int		ret;
	const char	*env, *home;

	PRECOND(out != NULL);

	if ((env = getenv("VIGIL_COMPANION_HOME")) != NULL) {
		ret = snprintf(out, len, "%s", env);
	} else if ((env = getenv("XDG_DATA_HOME")) != NULL && env[0] == '/') {
		ret = snprintf(out, len, "%s/vigil-companion", env);
	} else {
		if ((home = getenv("HOME")) == NULL)
			return (-1);
		ret = snprintf(out, len, "%s/.local/share/vigil-companion",
		    home);
	}

	if (ret < 0 || (size_t)ret >= len)
		return (-1);

	if (mkdir(out, 0700) == -1 && errno != EEXIST) {
		fprintf(stderr, "vigil: mkdir(%s): %s\n", out, strerror(errno));
		return (-1);
	}

	if (store_mkdir(out, "data") == -1)
		return (-1);

	if (store_mkdir(out, "chats") == -1)
		return (-1);

	if (store_mkdir(out, "state") == -1)
		return (-1);

	return (0);
}

/*
 * Make the given directory in our directory dir if it is not there.
 */
static int
store_mkdir(const char *dir, const char *name)
{
	int		ret;
	char		path[1024];

	PRECOND(dir != NULL);
	PRECOND(name != NULL);

	ret = snprintf(path, sizeof(path), "%s/%s", dir, name);
	if (ret < 0 || (size_t)ret >= sizeof(path))
		return (-1);

	if (mkdir(path, 0700) == -1 && errno != EEXIST) {
		fprintf(stderr, "vigil: mkdir(%s): %s\n", path,
		    strerror(errno));
		return (-1);
	}

	return (0);
}

/*
 * Unwrap the root key in the given key file with the given passphrase.
 */
static int
store_unwrap(struct store_key_file *key, const char *pass, size_t len)
{
	int		ret;
	u_int8_t	wrap[STORE_KEY_LEN];

	PRECOND(key != NULL);
	PRECOND(pass != NULL);

	if (memcmp(key->hdr.magic, STORE_MAGIC, sizeof(key->hdr.magic)))
		return (-1);

	if (be32toh(key->hdr.version) != STORE_VERSION)
		return (-1);

	store_wrap(pass, len, key->hdr.salt, wrap);

	ret = crypto_aead_aes256gcm_decrypt_detached(root, NULL, key->root,
	    sizeof(key->root), key->tag, (const u_int8_t *)&key->hdr,
	    sizeof(key->hdr), key->hdr.nonce, wrap);

	sodium_memzero(wrap, sizeof(wrap));

	if (ret != 0) {
		sodium_memzero(root, sizeof(root));
		return (-1);
	}

	return (0);
}

/*
 * Derive the key that wraps our root key from the given passphrase.
 */
static void
store_wrap(const char *pass, size_t len, const u_int8_t *salt, u_int8_t *out)
{
	struct nyfe_kmac256	kdf;
	u_int8_t		okm[NYFE_OKM_LEN];

	PRECOND(pass != NULL);
	PRECOND(salt != NULL);
	PRECOND(out != NULL);

	vigil_kyrka_enter();

	nyfe_passphrase_kdf(pass, len, salt, NYFE_KEY_FILE_SALT_LEN,
	    okm, sizeof(okm), STORE_PASS_LABEL, sizeof(STORE_PASS_LABEL) - 1);

	nyfe_kmac256_init(&kdf, okm, sizeof(okm),
	    STORE_WRAP_LABEL, sizeof(STORE_WRAP_LABEL) - 1);
	nyfe_mem_zero(okm, sizeof(okm));

	nyfe_kmac256_final(&kdf, out, STORE_KEY_LEN);
	nyfe_mem_zero(&kdf, sizeof(kdf));

	vigil_kyrka_leave();
}

/*
 * Derive the key for a seal from our root key, the given label and the
 * given context.
 */
static void
store_key(const char *label, const void *ctx, size_t len, u_int8_t *out)
{
	u_int64_t		be;
	struct nyfe_kmac256	kdf;

	PRECOND(label != NULL);
	PRECOND(ctx != NULL);
	PRECOND(out != NULL);
	PRECOND(unlocked);

	be = htobe64(len);

	vigil_kyrka_enter();

	nyfe_kmac256_init(&kdf, root, sizeof(root), label, strlen(label));
	nyfe_kmac256_update(&kdf, &be, sizeof(be));

	if (len > 0)
		nyfe_kmac256_update(&kdf, ctx, len);
	nyfe_kmac256_final(&kdf, out, STORE_KEY_LEN);
	nyfe_mem_zero(&kdf, sizeof(kdf));

	vigil_kyrka_leave();
}

/*
 * Seal len bytes of in under the given key into out as nonce || ct || tag.
 */
static void
store_seal(const u_int8_t *key, const void *aad, size_t aadlen,
    const void *in, size_t len, u_int8_t *out, size_t outlen)
{
	PRECOND(key != NULL);
	PRECOND(aad != NULL);
	PRECOND(in != NULL);
	PRECOND(out != NULL);
	PRECOND(outlen >= len + STORE_SEAL_LEN);

	store_random(out, STORE_NONCE_LEN);

	if (crypto_aead_aes256gcm_encrypt(out + STORE_NONCE_LEN, NULL, in, len,
	    aad, aadlen, NULL, out, key) != 0)
		fatal("crypto_aead_aes256gcm_encrypt failed");
}

/*
 * Open what store_seal() made into out, returns -1 if it does not open.
 */
static int
store_unseal(const u_int8_t *key, const void *aad, size_t aadlen,
    const u_int8_t *in, size_t len, u_int8_t *out, size_t outlen)
{
	PRECOND(key != NULL);
	PRECOND(aad != NULL);
	PRECOND(in != NULL);
	PRECOND(out != NULL);
	PRECOND(len > STORE_SEAL_LEN);
	PRECOND(outlen >= len - STORE_SEAL_LEN);

	if (crypto_aead_aes256gcm_decrypt(out, NULL, NULL,
	    in + STORE_NONCE_LEN, len - STORE_NONCE_LEN, aad, aadlen,
	    in, key) != 0)
		return (-1);

	return (0);
}

/*
 * Read the file at path if it is at most max bytes, the caller frees what
 * we return. Returns NULL if it is not there or too large.
 */
u_int8_t *
store_read(const char *path, size_t max, size_t *len)
{
	int		fd;
	struct stat	st;
	ssize_t		ret;
	size_t		off;
	u_int8_t	*data;

	PRECOND(path != NULL);
	PRECOND(len != NULL);

	if ((fd = open(path, O_RDONLY)) == -1)
		return (NULL);

	if (fstat(fd, &st) == -1 || (size_t)st.st_size > max) {
		(void)close(fd);
		return (NULL);
	}

	if ((data = malloc(st.st_size + 1)) == NULL)
		fatal("malloc");

	off = 0;

	while (off < (size_t)st.st_size) {
		ret = read(fd, data + off, st.st_size - off);
		if (ret == -1 && errno == EINTR)
			continue;

		if (ret <= 0) {
			(void)close(fd);
			free(data);
			return (NULL);
		}

		off += ret;
	}

	(void)close(fd);
	*len = off;

	return (data);
}

/*
 * Write data to path via a temporary file that takes its place once it is
 * on disk.
 */
static int
store_write(const char *path, const void *data, size_t len)
{
	int		fd;
	ssize_t		ret;
	char		tmp[1100];

	PRECOND(path != NULL);
	PRECOND(data != NULL);

	(void)snprintf(tmp, sizeof(tmp), "%s.tmp", path);

	if ((fd = open(tmp, O_CREAT | O_TRUNC | O_WRONLY, 0600)) == -1) {
		fprintf(stderr, "vigil: open(%s): %s\n", tmp, strerror(errno));
		return (-1);
	}

	ret = write(fd, data, len);

	if (ret == -1 || (size_t)ret != len || fsync(fd) == -1) {
		fprintf(stderr, "vigil: write(%s) failed\n", tmp);
		(void)close(fd);
		(void)unlink(tmp);
		return (-1);
	}

	(void)close(fd);

	if (rename(tmp, path) == -1) {
		fprintf(stderr, "vigil: rename(%s): %s\n", tmp,
		    strerror(errno));
		(void)unlink(tmp);
		return (-1);
	}

	return (0);
}
