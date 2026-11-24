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
 * The native side of the desktop of a phone. We run the core with the
 * companion configuration, store every mirror parcel from our phone before
 * we ack the parcel, and hand the stored records to the user interface
 * when the user interface polls.
 */

#include <sys/types.h>
#include <sys/file.h>
#include <sys/stat.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sodium.h>

#include "vigil.h"
#include "runner.h"

#include <libkyrka/libnyfe.h>

/* The name of our channel. */
#define CHANNEL			"se.sanctorum.vigil/desktop"

/* The milliseconds between two polls of the events of the core. */
#define DESKTOP_PUMP		100

/* The milliseconds between two polls while our phone sends us parcels. */
#define DESKTOP_PUMP_FAST	5

/* The milliseconds we keep polling fast after the last event. */
#define DESKTOP_PUMP_LINGER	1000

/* What we ask of our phone for our avatar, the picture follows. */
#define DESKTOP_AVATAR		"{\"what\":\"avatar\",\"id\":%" PRIu64 "}"

/* The record we hand the user interface for a request that did not go out. */
#define DESKTOP_UNSENT		\
    "{\"at\":%" PRIu64 ",\"file\":\"\",\"events\":[{\"op\":\"answer\","	\
    "\"what\":\"send\",\"ok\":false,\"reason\":\"Vigil could not keep "	\
    "that for your phone, it was not sent.\",\"chat\":\"%s\",\"ref\":%"	\
    PRIu64 "}]}"

/* The record we hand the user interface for a send we held for our phone. */
#define DESKTOP_QUEUED		\
    "{\"file\":\"\",\"events\":[{\"op\":\"queued\",\"event\":%.*s}]}"

/* The JSON a record wraps the events of our phone in. */
#define DESKTOP_RECORD		\
    "{\"at\":%" PRIu64 ",\"file\":\"%s\",\"events\":"

/*
 * A passphrase to open our store with on a thread of its own.
 */
struct unlock {
	FlMethodCall	*call;
	char		*pass;
	size_t		len;
	int		create;
	int		ret;
};

/*
 * Records held for the next poll of the user interface, as a JSON list
 * without its brackets.
 */
struct desktop_held {
	char		*buf;
	size_t		len;
};

/*
 * A run of the tunnel to covenant on a thread of its own.
 */
struct remote {
	FlMethodCall	*call;
	int		state;
};

static void	desktop_stop(void);
static int	desktop_start(void);
static void	*desktop_core(void *);
static gboolean	desktop_pump(gpointer);
static int	desktop_events(void);
static void	*desktop_unlock_run(void *);
static void	desktop_pending_drop(struct desktop_held *);
static gboolean	desktop_unlock_done(gpointer);
static void	desktop_mirror(const char *, size_t);
static void	desktop_unsent(const char *, size_t);
static void	desktop_queued(const char *, size_t);
static void	desktop_pending(struct desktop_held *, const char *, size_t);
static int	desktop_link(const char *, size_t, char *, size_t);
static void	desktop_call(FlMethodChannel *, FlMethodCall *, gpointer);

static void	desktop_ask(FlMethodCall *);
static void	desktop_url(FlMethodCall *);
static void	desktop_poll(FlMethodCall *);
static void	desktop_send(FlMethodCall *);
static void	desktop_data(FlMethodCall *);
static void	desktop_save(FlMethodCall *);
static void	*desktop_remote_wait(void *);
static void	desktop_open(FlMethodCall *);
static void	desktop_hash(FlMethodCall *);
static void	desktop_drop(FlMethodCall *);
static void	desktop_paste(FlMethodCall *);
static void	desktop_words(FlMethodCall *);
static void	desktop_route(FlMethodCall *);
static gboolean	desktop_remote_done(gpointer);
static void	desktop_voice(FlMethodCall *);
static void	desktop_unlock(FlMethodCall *);
static void	desktop_status(FlMethodCall *);
static void	desktop_digest(FlMethodCall *);
static void	desktop_import(FlMethodCall *);
static void	desktop_unlink(FlMethodCall *);
static void	desktop_resync(FlMethodCall *);
static void	desktop_avatar(FlMethodCall *);
static void	desktop_typing(FlMethodCall *);
static void	desktop_records(FlMethodCall *);
static void	desktop_call_op(FlMethodCall *);
static void	desktop_send_file(FlMethodCall *);
static void	desktop_send_data(FlMethodCall *);
static void	desktop_clipboard(FlMethodCall *);
static void	desktop_remote_run(FlMethodCall *);
static void	desktop_remote_link(FlMethodCall *);
static void	desktop_remote_start(FlMethodCall *);

static void	desktop_memo_seek(FlMethodCall *);
static void	desktop_memo_play(FlMethodCall *);
static void	desktop_memo_send(FlMethodCall *);
static void	desktop_memo_speed(FlMethodCall *);
static void	desktop_memo_cancel(FlMethodCall *);
static void	desktop_memo_record(FlMethodCall *);
static void	desktop_memo_position(FlMethodCall *);

static u_int64_t	desktop_ref(void);
static int	desktop_targets(FlValue *, u_int8_t *);
static int	desktop_hex(FlValue *, const char *, u_int64_t *);
static int	desktop_creates(const char *, const char *);
static int	desktop_state(const char *, size_t, const char *, const char *);
static int	desktop_keep(FlValue *, u_int64_t, const u_int8_t *, size_t,
		    char *, size_t);
static void	desktop_send_kept(FlMethodCall *, FlValue *, u_int64_t, int,
		    const char *, const u_int8_t *, size_t);
static int	desktop_send_to(FlValue *, u_int64_t, const char *, size_t,
		    int, const char *, const u_int8_t *, size_t);
static int	desktop_face(const u_int8_t *, size_t, const char *,
		    const char *, char *, size_t);
static int	desktop_op(const char *, size_t, const u_int8_t *, size_t,
		    char *, size_t);

static u_int8_t		*desktop_read(const char *, size_t *);
static const char	*desktop_skip(const char *);
static const char	*desktop_value(const char *, const char *);
static const char	*desktop_string(const char *, const char *);
static const char	*desktop_field(const char *, const char *,
			    const char *);
static const char	*desktop_key(const char *, const char *, const char *,
			    char *, size_t);

/* Our window and channel. */
static GtkWindow	*window = NULL;
static FlMethodChannel	*channel = NULL;

/* The core and the run it is in, 0 when it does not run. */
static pthread_t	core;
static u_int64_t	run_id = 0;

/* Set while we hold chats from before our phone ran more than one flock. */
static int		legacy = 0;

static guint		pump_interval = DESKTOP_PUMP;
static gint64		pump_active = 0;

/* The companion configuration the core runs with. */
static size_t		config_len = 0;
static char		config[VIGIL_CONFIG_MAX];

/*
 * The records we stored and the notes we do not store, such as a send that
 * did not go out, that the user interface did not pick up yet.
 */
static struct desktop_held	pending;
static struct desktop_held	notes;

/* Where the core keeps its remembrances. */
static char		datadir[1024];

/*
 * Take the lock on our directory so only one of us runs at a time,
 * returns -1 with errno set if we cannot.
 */
int
runner_single(void)
{
	int		fd;
	char		path[1024];

	if (store_path(path, sizeof(path), "lock") == -1) {
		errno = ENAMETOOLONG;
		return (-1);
	}

	if ((fd = open(path, O_CREAT | O_RDWR, 0600)) == -1)
		return (-1);

	if (flock(fd, LOCK_EX | LOCK_NB) == -1) {
		(void)close(fd);
		return (-1);
	}

	return (0);
}

/*
 * Set up our channel on the given view.
 */
int
runner_channel_init(FlView *view, GtkWindow *win)
{
	FlEngine			*engine;
	FlBinaryMessenger		*messenger;
	g_autoptr(FlStandardMethodCodec)	codec = NULL;

	PRECOND(view != NULL);
	PRECOND(win != NULL);

	if (sodium_init() == -1)
		return (-1);

	if (crypto_aead_aes256gcm_is_available() == 0) {
		fprintf(stderr, "vigil: this cpu cannot do AES-GCM\n");
		return (-1);
	}

	if (store_path(datadir, sizeof(datadir), "core") == -1)
		return (-1);

	if (mkdir(datadir, 0700) == -1 && errno != EEXIST) {
		fprintf(stderr, "vigil: mkdir(%s): %s\n", datadir,
		    strerror(errno));
		return (-1);
	}

	vigil_kyrka_enter();
	nyfe_random_init();
	vigil_kyrka_leave();

	window = win;
	engine = fl_view_get_engine(view);
	messenger = fl_engine_get_binary_messenger(engine);
	codec = fl_standard_method_codec_new();

	channel = fl_method_channel_new(messenger, CHANNEL,
	    FL_METHOD_CODEC(codec));
	fl_method_channel_set_method_call_handler(channel, desktop_call,
	    NULL, NULL);

	(void)g_timeout_add(pump_interval, desktop_pump, NULL);

	return (0);
}

/*
 * Stop the core if it runs, we are going away.
 */
void
runner_stop(void)
{
	memo_stop();
	desktop_stop();
}

/*
 * A call from the user interface.
 */
static void
desktop_call(FlMethodChannel *chan, FlMethodCall *call, gpointer udata)
{
	const char	*method;

	method = fl_method_call_get_name(call);

	if (!strcmp(method, "unlock")) {
		desktop_unlock(call);
	} else if (!strcmp(method, "records")) {
		desktop_records(call);
	} else if (!strcmp(method, "poll")) {
		desktop_poll(call);
	} else if (!strcmp(method, "status")) {
		desktop_status(call);
	} else if (!strcmp(method, "send")) {
		desktop_send(call);
	} else if (!strcmp(method, "sendFile")) {
		desktop_send_file(call);
	} else if (!strcmp(method, "data")) {
		desktop_data(call);
	} else if (!strcmp(method, "save")) {
		desktop_save(call);
	} else if (!strcmp(method, "digest")) {
		desktop_digest(call);
	} else if (!strcmp(method, "import")) {
		desktop_import(call);
	} else if (!strcmp(method, "paste")) {
		desktop_paste(call);
	} else if (!strcmp(method, "unlink")) {
		desktop_unlink(call);
	} else if (!strcmp(method, "resync")) {
		desktop_resync(call);
	} else if (!strcmp(method, "words")) {
		desktop_words(call);
	} else if (!strcmp(method, "route")) {
		desktop_route(call);
	} else if (!strcmp(method, "remoteStart")) {
		desktop_remote_start(call);
	} else if (!strcmp(method, "remoteRun")) {
		desktop_remote_run(call);
	} else if (!strcmp(method, "remoteLink")) {
		desktop_remote_link(call);
	} else if (!strcmp(method, "remoteStop")) {
		vigil_remote_stop();
		fl_method_call_respond_success(call, NULL, NULL);
	} else if (!strcmp(method, "sendData")) {
		desktop_send_data(call);
	} else if (!strcmp(method, "clipboard")) {
		desktop_clipboard(call);
	} else if (!strcmp(method, "ask")) {
		desktop_ask(call);
	} else if (!strcmp(method, "avatar")) {
		desktop_avatar(call);
	} else if (!strcmp(method, "typing")) {
		desktop_typing(call);
	} else if (!strcmp(method, "open")) {
		desktop_open(call);
	} else if (!strcmp(method, "hash")) {
		desktop_hash(call);
	} else if (!strcmp(method, "drop")) {
		desktop_drop(call);
	} else if (!strcmp(method, "url")) {
		desktop_url(call);
	} else if (!strcmp(method, "call")) {
		desktop_call_op(call);
	} else if (!strcmp(method, "voice")) {
		desktop_voice(call);
	} else if (!strcmp(method, "memoRecord")) {
		desktop_memo_record(call);
	} else if (!strcmp(method, "memoSend")) {
		desktop_memo_send(call);
	} else if (!strcmp(method, "memoCancel")) {
		desktop_memo_cancel(call);
	} else if (!strcmp(method, "memoPlay")) {
		desktop_memo_play(call);
	} else if (!strcmp(method, "memoStop")) {
		memo_stop();
		fl_method_call_respond_success(call, NULL, NULL);
	} else if (!strcmp(method, "memoSeek")) {
		desktop_memo_seek(call);
	} else if (!strcmp(method, "memoSpeed")) {
		desktop_memo_speed(call);
	} else if (!strcmp(method, "memoPosition")) {
		desktop_memo_position(call);
	} else {
		fl_method_call_respond_not_implemented(call, NULL);
	}
}

/*
 * Ask for the passphrase, a new one the first time, and open our store
 * with it on a thread of its own.
 */
static void
desktop_unlock(FlMethodCall *call)
{
	pthread_t		tid;
	struct unlock		*job;
	const char		*reason;

	if ((job = calloc(1, sizeof(*job))) == NULL)
		fatal("calloc");

	job->create = !store_exists();

	if (job->create) {
		reason = "Pick a passphrase for Vigil on this computer, it "
		    "protects the messages it keeps.";
	} else {
		reason = "Type your passphrase to open Vigil.";
	}

	if (runner_passphrase(window, reason, job->create,
	    &job->pass, &job->len) == -1) {
		fl_method_call_respond_error(call, "cancelled",
		    "Vigil stays locked.", NULL, NULL);
		free(job);
		return;
	}

	job->call = g_object_ref(call);

	if (pthread_create(&tid, NULL, desktop_unlock_run, job) != 0)
		fatal("pthread_create");

	if (pthread_detach(tid) != 0)
		fatal("pthread_detach");
}

/*
 * Open or create our store with the passphrase we got.
 */
static void *
desktop_unlock_run(void *arg)
{
	struct unlock	*job;

	job = arg;

	if (job->create)
		job->ret = store_create(job->pass, job->len);
	else
		job->ret = store_open(job->pass, job->len);

	sodium_memzero(job->pass, job->len);
	free(job->pass);
	job->pass = NULL;

	(void)g_idle_add(desktop_unlock_done, job);

	return (NULL);
}

/*
 * Back on the main thread, start the core if we have a configuration.
 */
static gboolean
desktop_unlock_done(gpointer arg)
{
	size_t		len;
	char		*cfg;
	struct unlock	*job;
	FlValue		*reply;

	job = arg;

	if (job->ret == -1) {
		fl_method_call_respond_error(job->call, "passphrase",
		    "That passphrase does not open Vigil.", NULL, NULL);
	} else {
		legacy = store_legacy();

		if ((cfg = store_config(&len)) != NULL) {
			memcpy(config, cfg, len);
			config_len = len;
			sodium_memzero(cfg, len);
			free(cfg);

			if (desktop_start() == -1)
				config_len = 0;
		}

		reply = fl_value_new_bool(config_len > 0);
		fl_method_call_respond_success(job->call, reply, NULL);
		fl_value_unref(reply);
	}

	g_object_unref(job->call);
	free(job);

	return (G_SOURCE_REMOVE);
}

/*
 * Hand back every record we have as one JSON array.
 */
static void
desktop_records(FlMethodCall *call)
{
	size_t		len;
	FlValue		*reply;
	char		*json, *str;

	if ((json = store_records(&len)) == NULL) {
		fl_method_call_respond_error(call, "store",
		    "The messages could not be read.", NULL, NULL);
		return;
	}

	if ((str = malloc(len + 1)) == NULL)
		fatal("malloc");

	memcpy(str, json, len);
	str[len] = '\0';

	sodium_memzero(json, len);
	free(json);

	desktop_pending_drop(&pending);

	reply = fl_value_new_string(str);
	fl_method_call_respond_success(call, reply, NULL);
	fl_value_unref(reply);

	sodium_memzero(str, len);
	free(str);
}

/*
 * Hand back the records we stored since the last poll, as a JSON array.
 */
static void
desktop_poll(FlMethodCall *call)
{
	char		*str;
	FlValue		*reply;
	size_t		len, off;

	len = pending.len + notes.len + 4;

	if ((str = malloc(len)) == NULL)
		fatal("malloc");

	off = 0;
	str[off++] = '[';

	if (pending.len > 0) {
		memcpy(str + off, pending.buf, pending.len);
		off += pending.len;
	}

	if (pending.len > 0 && notes.len > 0)
		str[off++] = ',';

	if (notes.len > 0) {
		memcpy(str + off, notes.buf, notes.len);
		off += notes.len;
	}

	str[off++] = ']';
	str[off] = '\0';

	desktop_pending_drop(&pending);
	desktop_pending_drop(&notes);

	reply = fl_value_new_string(str);
	fl_method_call_respond_success(call, reply, NULL);
	fl_value_unref(reply);

	sodium_memzero(str, len);
	free(str);
}

/*
 * Hand back the status of the core.
 */
static void
desktop_status(FlMethodCall *call)
{
	char		*buf;
	FlValue		*reply;

	if ((buf = calloc(1, VIGIL_STATUS_MAX)) == NULL)
		fatal("calloc");

	if (run_id == 0 || vigil_status(buf, VIGIL_STATUS_MAX) == 0)
		(void)snprintf(buf, VIGIL_STATUS_MAX, "{}");

	if (legacy && run_id != 0 && vigil_chat_companion_unsync() == 0)
		legacy = 0;

	reply = fl_value_new_string(buf);
	fl_method_call_respond_success(call, reply, NULL);
	fl_value_unref(reply);
	free(buf);
}

/*
 * Have our phone send a text for us, to a peer or to the members of a
 * group, under the ref the user interface picked.
 */
static void
desktop_send(FlMethodCall *call)
{
	u_int64_t	ref;
	const char	*str;
	FlValue		*args, *text;

	args = fl_method_call_get_args(call);

	if (run_id == 0) {
		fl_method_call_respond_error(call, "offline",
		    "Vigil is not linked to a phone.", NULL, NULL);
		return;
	}

	if (fl_value_get_type(args) != FL_VALUE_TYPE_MAP ||
	    (text = fl_value_lookup_string(args, "text")) == NULL ||
	    fl_value_get_type(text) != FL_VALUE_TYPE_STRING ||
	    desktop_hex(args, "ref", &ref) == -1 ||
	    !(ref & VIGIL_CHAT_COMPANION_REF)) {
		fl_method_call_respond_error(call, "args", "Bad message.",
		    NULL, NULL);
		return;
	}

	str = fl_value_get_string(text);

	if (desktop_send_to(args, ref, str, strlen(str), 0,
	    NULL, NULL, 0) == -1) {
		fl_method_call_respond_error(call, "send",
		    "The message could not be sent.", NULL, NULL);
		return;
	}

	fl_method_call_respond_success(call, NULL, NULL);
}

/*
 * Let the user pick a file, keep the file and have our phone send the file.
 * Answers with the name we keep the file under, its name and size, or
 * nothing if the user cancelled.
 */
static void
desktop_send_file(FlMethodCall *call)
{
	u_int64_t	ref;
	size_t		len;
	u_int8_t	*data;
	int		which;
	char		*path, *base;
	FlValue		*args, *kind;

	args = fl_method_call_get_args(call);

	if (run_id == 0) {
		fl_method_call_respond_error(call, "offline",
		    "Vigil is not linked to a phone.", NULL, NULL);
		return;
	}

	if (fl_value_get_type(args) != FL_VALUE_TYPE_MAP ||
	    (kind = fl_value_lookup_string(args, "kind")) == NULL ||
	    fl_value_get_type(kind) != FL_VALUE_TYPE_STRING ||
	    desktop_hex(args, "ref", &ref) == -1 ||
	    !(ref & VIGIL_CHAT_COMPANION_REF)) {
		fl_method_call_respond_error(call, "args", "Bad file.",
		    NULL, NULL);
		return;
	}

	which = VIGIL_CHAT_DATA_FILE;
	if (!strcmp(fl_value_get_string(kind), "image"))
		which = VIGIL_CHAT_DATA_IMAGE;

	if (runner_pick(window, which == VIGIL_CHAT_DATA_IMAGE ?
	    "Send an image" : "Send a file", &path) == -1) {
		fl_method_call_respond_success(call, NULL, NULL);
		return;
	}

	if ((data = desktop_read(path, &len)) == NULL) {
		fl_method_call_respond_error(call, "file",
		    "That file cannot be read, is empty or is over 4 MB.",
		    NULL, NULL);
		free(path);
		return;
	}

	if ((base = strrchr(path, '/')) != NULL)
		base++;
	else
		base = path;

	if (strlen(base) > VIGIL_CHAT_DATA_NAME_MAX)
		base[VIGIL_CHAT_DATA_NAME_MAX] = '\0';

	desktop_send_kept(call, args, ref, which, base, data, len);

	sodium_memzero(data, len);
	free(data);
	free(path);
}

/*
 * Hand our text or data to the core for the peer in the map, or for every
 * member when the map holds a group. Returns -1 if it went to no one.
 */
static int
desktop_send_to(FlValue *args, u_int64_t ref, const char *text, size_t len,
    int kind, const char *name, const u_int8_t *data, size_t size)
{
	int				ret;
	long				idx;
	struct vigil_chat_request	req;
	int				sent;
	u_int64_t			hash, topic, flock;
	u_int8_t			targets[VIGIL_CHAT_GROUP_ROSTER];
	u_int64_t			peer, owner, group, serial, author;

	PRECOND(args != NULL);

	if (desktop_hex(args, "flock", &flock) == -1 || flock == 0)
		return (-1);

	sent = 0;
	peer = 0;
	hash = 0;
	topic = 0;
	owner = 0;
	group = 0;
	author = 0;
	serial = 0;

	(void)desktop_hex(args, "hash", &hash);
	(void)desktop_hex(args, "topic", &topic);
	(void)desktop_hex(args, "author", &author);

	if (author >= KYRKA_PEERS_PER_FLOCK)
		return (-1);

	memset(targets, 0, sizeof(targets));

	if (desktop_hex(args, "group", &group) == 0 && group != 0) {
		if (desktop_hex(args, "owner", &owner) == -1 ||
		    owner == 0 || owner >= KYRKA_PEERS_PER_FLOCK)
			return (-1);

		if (desktop_hex(args, "serial", &serial) == -1)
			return (-1);

		if (desktop_targets(args, targets) == -1)
			return (-1);
	} else {
		if (desktop_hex(args, "peer", &peer) == -1 ||
		    peer == 0 || peer >= KYRKA_PEERS_PER_FLOCK)
			return (-1);
		targets[peer / 8] |= 1 << (peer % 8);
	}

	if (kind == 0) {
		if (len == 0)
			return (-1);

		if (group != 0 && topic != 0 &&
		    len > VIGIL_CHAT_GROUP_TOPIC_MAX)
			return (-1);

		if (group != 0 && hash != 0 && len > VIGIL_CHAT_GROUP_REPLY_MAX)
			return (-1);

		if (group != 0 && len > VIGIL_CHAT_GROUP_MAX)
			return (-1);

		if (topic != 0 && len > VIGIL_CHAT_TOPIC_MAX)
			return (-1);

		if (hash != 0 && len > VIGIL_CHAT_REPLY_MAX)
			return (-1);

		if (len > VIGIL_CHAT_MESSAGE_MAX)
			return (-1);
	}

	for (idx = 1; idx < KYRKA_PEERS_PER_FLOCK; idx++) {
		if (targets[idx / 8] & (1 << (idx % 8)))
			sent++;
	}

	if (sent == 0)
		return (-1);

	memset(&req, 0, sizeof(req));

	req.ref = ref;
	req.hash = hash;
	req.flock = flock;
	req.topic = topic;
	req.author = author;
	req.op = VIGIL_COMPANION_SEND;

	if (group != 0) {
		req.owner = owner;
		req.group = group;
		req.serial = serial;
		memcpy(req.members, targets, sizeof(req.members));
	} else {
		req.peer = peer;
	}

	if (kind != 0) {
		if (strlen(name) > sizeof(req.name))
			return (-1);
		req.kind = kind;
		req.len = strlen(name);
		memcpy(req.name, name, req.len);
		ret = vigil_chat_companion_send(&req, data, size);
	} else {
		ret = vigil_chat_companion_send(&req, text, len);
	}

	return (ret);
}

/*
 * Hand back the data we keep under the given name.
 */
static void
desktop_data(FlMethodCall *call)
{
	size_t		len;
	FlValue		*args;
	u_int8_t	*data;
	FlValue		*reply;

	args = fl_method_call_get_args(call);

	if (fl_value_get_type(args) != FL_VALUE_TYPE_STRING ||
	    strlen(fl_value_get_string(args)) >= STORE_NAME_MAX) {
		fl_method_call_respond_error(call, "args", "Bad name.",
		    NULL, NULL);
		return;
	}

	if ((data = store_data_get(fl_value_get_string(args), &len)) == NULL) {
		fl_method_call_respond_success(call, NULL, NULL);
		return;
	}

	reply = fl_value_new_uint8_list(data, len);
	fl_method_call_respond_success(call, reply, NULL);
	fl_value_unref(reply);

	sodium_memzero(data, len);
	free(data);
}

/*
 * Let the user pick where to save the data we keep under the given name.
 */
static void
desktop_save(FlMethodCall *call)
{
	FILE		*fp;
	size_t		len;
	u_int8_t	*data;
	char		*path;
	FlValue		*reply;
	FlValue		*args, *file, *name;

	args = fl_method_call_get_args(call);

	if (fl_value_get_type(args) != FL_VALUE_TYPE_MAP ||
	    (file = fl_value_lookup_string(args, "file")) == NULL ||
	    (name = fl_value_lookup_string(args, "name")) == NULL ||
	    fl_value_get_type(file) != FL_VALUE_TYPE_STRING ||
	    fl_value_get_type(name) != FL_VALUE_TYPE_STRING ||
	    strlen(fl_value_get_string(file)) >= STORE_NAME_MAX) {
		fl_method_call_respond_error(call, "args", "Bad file.",
		    NULL, NULL);
		return;
	}

	if ((data = store_data_get(fl_value_get_string(file), &len)) == NULL) {
		fl_method_call_respond_error(call, "file",
		    "That file is no longer here.", NULL, NULL);
		return;
	}

	if (runner_save(window, fl_value_get_string(name), &path) == -1) {
		reply = fl_value_new_bool(FALSE);
		fl_method_call_respond_success(call, reply, NULL);
		fl_value_unref(reply);
		sodium_memzero(data, len);
		free(data);
		return;
	}

	if ((fp = fopen(path, "w")) == NULL ||
	    fwrite(data, 1, len, fp) != len) {
		if (fp != NULL)
			fclose(fp);
		fl_method_call_respond_error(call, "file",
		    "The file could not be written.", NULL, NULL);
	} else {
		fclose(fp);
		reply = fl_value_new_bool(TRUE);
		fl_method_call_respond_success(call, reply, NULL);
		fl_value_unref(reply);
	}

	sodium_memzero(data, len);
	free(data);
	free(path);
}

/*
 * Hand back the hash our phones use in replies for every given string, the
 * first 8 bytes of its SHA-256 as hex.
 */
static void
desktop_digest(FlMethodCall *call)
{
	const char	*str;
	char		hex[17];
	size_t		idx, count;
	FlValue		*args, *out, *item;
	u_int8_t	sum[crypto_hash_sha256_BYTES];

	args = fl_method_call_get_args(call);

	if (fl_value_get_type(args) != FL_VALUE_TYPE_LIST) {
		fl_method_call_respond_error(call, "args", "No list.",
		    NULL, NULL);
		return;
	}

	out = fl_value_new_list();
	count = fl_value_get_length(args);

	for (idx = 0; idx < count; idx++) {
		item = fl_value_get_list_value(args, idx);
		str = "";

		if (fl_value_get_type(item) == FL_VALUE_TYPE_STRING)
			str = fl_value_get_string(item);

		crypto_hash_sha256(sum, (const u_int8_t *)str, strlen(str));
		(void)snprintf(hex, sizeof(hex),
		    "%02x%02x%02x%02x%02x%02x%02x%02x", sum[0], sum[1],
		    sum[2], sum[3], sum[4], sum[5], sum[6], sum[7]);

		fl_value_append_take(out, fl_value_new_string(hex));
	}

	fl_method_call_respond_success(call, out, NULL);
	fl_value_unref(out);
}

/*
 * Let the user pick the companion configuration covenant made for us.
 */
static void
desktop_import(FlMethodCall *call)
{
	FILE		*fp;
	size_t		len;
	char		*path;
	FlValue		*reply;
	char		cfg[VIGIL_CONFIG_MAX], err[256];

	if (runner_pick(window, "Open your companion configuration",
	    &path) == -1) {
		reply = fl_value_new_bool(FALSE);
		fl_method_call_respond_success(call, reply, NULL);
		fl_value_unref(reply);
		return;
	}

	if ((fp = fopen(path, "r")) == NULL) {
		fl_method_call_respond_error(call, "file",
		    "That file could not be opened.", NULL, NULL);
		free(path);
		return;
	}

	len = fread(cfg, 1, sizeof(cfg), fp);
	fclose(fp);
	free(path);

	if (desktop_link(cfg, len, err, sizeof(err)) == -1) {
		fl_method_call_respond_error(call, "config", err, NULL, NULL);
		sodium_memzero(cfg, sizeof(cfg));
		return;
	}

	sodium_memzero(cfg, sizeof(cfg));
	reply = fl_value_new_bool(TRUE);
	fl_method_call_respond_success(call, reply, NULL);
	fl_value_unref(reply);
}

/*
 * Take the companion configuration the user pasted.
 */
static void
desktop_paste(FlMethodCall *call)
{
	const char	*cfg;
	FlValue		*args;
	FlValue		*reply;
	char		err[256];

	args = fl_method_call_get_args(call);

	if (fl_value_get_type(args) != FL_VALUE_TYPE_STRING) {
		fl_method_call_respond_error(call, "args", "No configuration.",
		    NULL, NULL);
		return;
	}

	cfg = fl_value_get_string(args);

	if (desktop_link(cfg, strlen(cfg), err, sizeof(err)) == -1) {
		fl_method_call_respond_error(call, "config", err, NULL, NULL);
		return;
	}

	reply = fl_value_new_bool(TRUE);
	fl_method_call_respond_success(call, reply, NULL);
	fl_value_unref(reply);
}

/*
 * Check the given configuration is one for the desktop of a phone, keep
 * the configuration and run the core with that configuration.
 */
static int
desktop_link(const char *cfg, size_t len, char *err, size_t errlen)
{
	const char	*spi;
	char		summary[4096];

	PRECOND(cfg != NULL);
	PRECOND(err != NULL);

	if (len == 0 || len > VIGIL_CONFIG_MAX) {
		(void)snprintf(err, errlen, "That is not a configuration.");
		return (-1);
	}

	if (vigil_config_check(cfg, len, summary, sizeof(summary)) == -1) {
		(void)snprintf(err, errlen, "%s", summary);
		return (-1);
	}

	if (strstr(summary, "\"mode\":\"companion\"") == NULL ||
	    (spi = strstr(summary, "\"spi\":\"")) == NULL ||
	    strncmp(spi + 7, "0201", 4)) {
		(void)snprintf(err, errlen,
		    "That is not the configuration for a desktop.");
		return (-1);
	}

	if (store_config_save(cfg, len) == -1) {
		(void)snprintf(err, errlen, "It could not be stored.");
		return (-1);
	}

	desktop_stop();

	memcpy(config, cfg, len);
	config_len = len;

	if (desktop_start() == -1) {
		(void)snprintf(err, errlen, "The core did not start.");
		return (-1);
	}

	return (0);
}

/*
 * Stop the core and forget our companion configuration, what we have
 * stored stays.
 */
static void
desktop_unlink(FlMethodCall *call)
{
	desktop_stop();
	store_config_drop();

	sodium_memzero(config, sizeof(config));
	config_len = 0;

	fl_method_call_respond_success(call, NULL, NULL);
}

/*
 * Have our phone send us a full resync of its chats.
 */
static void
desktop_resync(FlMethodCall *call)
{
	if (vigil_chat_companion_unsync() == -1) {
		fl_method_call_respond_error(call, "resync",
		    "This computer is not linked to a phone.", NULL, NULL);
		return;
	}

	fl_method_call_respond_success(call, NULL, NULL);
}

/*
 * Hand back the word list covenant picks secret words from.
 */
static void
desktop_words(FlMethodCall *call)
{
	u_int16_t	idx;
	FlValue		*out;
	const char	*word;

	out = fl_value_new_list();

	for (idx = 0; (word = vigil_remote_word(idx)) != NULL; idx++)
		fl_value_append_take(out, fl_value_new_string(word));

	fl_method_call_respond_success(call, out, NULL);
	fl_value_unref(out);
}

/*
 * Read the route to covenant from the secret words, answers with the
 * flock, cathedral and our cosk as JSON or nothing if the words are wrong.
 */
static void
desktop_route(FlMethodCall *call)
{
	FlValue		*args;
	FlValue		*reply;
	char		out[256];

	args = fl_method_call_get_args(call);

	if (fl_value_get_type(args) != FL_VALUE_TYPE_STRING) {
		fl_method_call_respond_error(call, "args", "No words.",
		    NULL, NULL);
		return;
	}

	if (vigil_remote_words(fl_value_get_string(args), out,
	    sizeof(out)) == -1) {
		fl_method_call_respond_success(call, NULL, NULL);
		return;
	}

	reply = fl_value_new_string(out);
	fl_method_call_respond_success(call, reply, NULL);
	fl_value_unref(reply);
	sodium_memzero(out, sizeof(out));
}

/*
 * Set up the tunnel to covenant under the cathedral id and secret
 * Reliquary gave us.
 */
static void
desktop_remote_start(FlMethodCall *call)
{
	FlValue		*args, *id, *secret;

	args = fl_method_call_get_args(call);

	if (fl_value_get_type(args) != FL_VALUE_TYPE_MAP ||
	    (id = fl_value_lookup_string(args, "id")) == NULL ||
	    (secret = fl_value_lookup_string(args, "secret")) == NULL ||
	    fl_value_get_type(id) != FL_VALUE_TYPE_STRING ||
	    fl_value_get_type(secret) != FL_VALUE_TYPE_STRING) {
		fl_method_call_respond_error(call, "args", "No device.",
		    NULL, NULL);
		return;
	}

	if (vigil_remote_start(fl_value_get_string(id),
	    fl_value_get_string(secret)) == -1) {
		fl_method_call_respond_error(call, "remote",
		    "The tunnel to Covenant could not be set up.", NULL, NULL);
		return;
	}

	fl_method_call_respond_success(call, NULL, NULL);
}

/*
 * Run the tunnel to covenant for up to a second on a thread of its own.
 */
static void
desktop_remote_run(FlMethodCall *call)
{
	pthread_t		tid;
	struct remote		*job;

	if ((job = calloc(1, sizeof(*job))) == NULL)
		fatal("calloc");

	job->call = g_object_ref(call);

	if (pthread_create(&tid, NULL, desktop_remote_wait, job) != 0)
		fatal("pthread_create");

	if (pthread_detach(tid) != 0)
		fatal("pthread_detach");
}

/*
 * The thread that runs the tunnel to covenant once.
 */
static void *
desktop_remote_wait(void *arg)
{
	struct remote	*job;

	job = arg;
	job->state = vigil_remote_run();

	(void)g_idle_add(desktop_remote_done, job);

	return (NULL);
}

/*
 * Back on the main thread, answer with the state of the tunnel.
 */
static gboolean
desktop_remote_done(gpointer arg)
{
	struct remote	*job;
	FlValue		*reply;

	job = arg;

	reply = fl_value_new_int(job->state);
	fl_method_call_respond_success(job->call, reply, NULL);
	fl_value_unref(reply);

	g_object_unref(job->call);
	free(job);

	return (G_SOURCE_REMOVE);
}

/*
 * Link with the configuration covenant sent us, false until we have it.
 */
static void
desktop_remote_link(FlMethodCall *call)
{
	FlValue		*reply;
	char		err[256];
	char		cfg[VIGIL_CONFIG_MAX];

	if (vigil_remote_config(cfg, sizeof(cfg)) == -1) {
		reply = fl_value_new_bool(FALSE);
		fl_method_call_respond_success(call, reply, NULL);
		fl_value_unref(reply);
		return;
	}

	vigil_remote_stop();

	if (desktop_link(cfg, strlen(cfg), err, sizeof(err)) == -1) {
		sodium_memzero(cfg, sizeof(cfg));
		fl_method_call_respond_error(call, "config", err, NULL, NULL);
		return;
	}

	sodium_memzero(cfg, sizeof(cfg));
	reply = fl_value_new_bool(TRUE);
	fl_method_call_respond_success(call, reply, NULL);
	fl_value_unref(reply);
}

/*
 * Run the core with our configuration on a thread of its own.
 */
static int
desktop_start(void)
{
	PRECOND(run_id == 0);
	PRECOND(config_len > 0);

	run_id = vigil_run_id();

	if (pthread_create(&core, NULL, desktop_core, &run_id) != 0) {
		run_id = 0;
		return (-1);
	}

	return (0);
}

/*
 * Stop the core if it runs.
 */
static void
desktop_stop(void)
{
	if (run_id == 0)
		return;

	vigil_stop(run_id);
	pthread_join(core, NULL);

	run_id = 0;
}

/*
 * The thread the core runs on.
 */
static void *
desktop_core(void *arg)
{
	u_int64_t	id;

	PRECOND(arg != NULL);

	id = *(u_int64_t *)arg;

	if (vigil_run(id, -1, config, config_len, NULL, 0, datadir) == -1)
		fprintf(stderr, "vigil: the core did not run\n");

	return (NULL);
}

/*
 * Poll the core for events, fast for as long as they keep coming so the
 * next parcel of our phone is acked right away, at our normal pace again
 * once they stop.
 */
static gboolean
desktop_pump(gpointer arg)
{
	gint64		now;
	guint		interval;

	now = g_get_monotonic_time();

	if (desktop_events())
		pump_active = now;

	if (pump_active != 0 &&
	    now - pump_active < DESKTOP_PUMP_LINGER * G_TIME_SPAN_MILLISECOND)
		interval = DESKTOP_PUMP_FAST;
	else
		interval = DESKTOP_PUMP;

	if (interval == pump_interval)
		return (G_SOURCE_CONTINUE);

	pump_interval = interval;
	(void)g_timeout_add(pump_interval, desktop_pump, NULL);

	return (G_SOURCE_REMOVE);
}

/*
 * Pick up the mirror events the core has for us and store each one before
 * the core acks its parcel, returns 1 if there were any.
 */
static int
desktop_events(void)
{
	int		any;
	size_t		len;
	const char	*p, *end;
	static char	buf[VIGIL_CHAT_EVENTS_MAX];

	if (run_id == 0)
		return (0);

	if ((len = vigil_chat_events(buf, sizeof(buf))) == 0)
		return (0);

	p = buf;

	if (*p != '[')
		return (0);

	p++;
	any = 0;

	while (*p == '{') {
		if ((end = desktop_skip(p)) == NULL)
			break;

		any = 1;
		desktop_mirror(p, end - p);
		desktop_unsent(p, end - p);
		desktop_queued(p, end - p);

		p = end;
		if (*p == ',')
			p++;
	}

	sodium_memzero(buf, len);

	return (any);
}

/*
 * Store every op of one mirror event from the core where it belongs, the
 * data of the event goes with its last op, and ack the parcel once all of
 * it is on disk.
 */
static void
desktop_mirror(const char *evt, size_t len)
{
	int		ret;
	u_int64_t	token;
	u_int8_t	*data;
	char		*record;
	size_t		size, need;
	char		name[STORE_NAME_MAX];
	const char	*field, *events, *end, *op, *next;

	PRECOND(evt != NULL);

	end = evt + len;

	if ((field = desktop_field(evt, end, "\"type\":\"mirror\"")) == NULL)
		return;

	if ((field = desktop_field(evt, end, "\"token\":")) == NULL)
		return;
	token = strtoull(field, NULL, 10);

	if ((field = desktop_field(evt, end, "\"size\":")) == NULL)
		return;
	size = strtoull(field, NULL, 10);

	if ((events = desktop_field(evt, end, "\"events\":[")) == NULL)
		return;

	data = NULL;

	if (size > 0) {
		if (size > VIGIL_CHAT_DATA_MAX)
			return;

		if ((data = malloc(size)) == NULL)
			fatal("malloc");

		if (vigil_chat_data_take(token, data, size) != (ssize_t)size) {
			free(data);
			return;
		}
	}

	ret = 0;
	name[0] = '\0';

	for (op = events; *op == '{'; op = next) {
		if ((next = desktop_skip(op)) == NULL || next > end)
			break;

		if (*next == ']') {
			ret = desktop_op(op, next - op, data, size,
			    name, sizeof(name));
		} else {
			ret = desktop_op(op, next - op, NULL, 0,
			    name, sizeof(name));
		}

		if (ret == -1)
			break;

		if (*next == ',')
			next++;
	}

	if (data != NULL) {
		sodium_memzero(data, size);
		free(data);
	}

	if (ret == -1)
		return;

	need = sizeof(DESKTOP_RECORD) + 32 + sizeof(name) + (end - events);

	if ((record = malloc(need)) == NULL)
		fatal("malloc");

	ret = snprintf(record, need, DESKTOP_RECORD,
	    (u_int64_t)time(NULL) * 1000, name);
	if (ret < 0 || (size_t)ret >= need)
		fatal("record header too long");

	record[ret++] = '[';
	memcpy(record + ret, events, end - events);
	len = ret + (end - events);

	desktop_pending(&pending, record, len);

	if (vigil_chat_stored(token) == -1)
		fprintf(stderr, "vigil: could not ack %" PRIu64 "\n", token);

	sodium_memzero(record, len);
	free(record);
}

/*
 * Store the given op of our phone where it belongs, as the phone keeps it:
 * ops of a chat in the log of that chat, the data of a record under the
 * name the record gives it and our state as a file per item. Puts the
 * name of an avatar we store in name. Returns -1 if we could not store it.
 */
static int
desktop_op(const char *op, size_t len, const u_int8_t *data, size_t size,
    char *name, size_t namelen)
{
	int		ret;
	const char	*end, *rec;
	char		file[STORE_NAME_MAX];
	char		kind[16], chat[STORE_CHAT_MAX], peer[STORE_PEER_MAX];

	PRECOND(op != NULL);
	PRECOND(data != NULL || size == 0);
	PRECOND(name != NULL);

	end = op + len;

	if (desktop_key(op, end, "op", kind, sizeof(kind)) == NULL)
		return (0);

	ret = 0;
	chat[0] = '\0';
	peer[0] = '\0';
	file[0] = '\0';

	(void)desktop_key(op, end, "chat", chat, sizeof(chat));
	(void)desktop_key(op, end, "peer", peer, sizeof(peer));

	if (!strcmp(kind, "batch")) {
		if ((rec = desktop_key(op, end, "ops", NULL, 0)) == NULL ||
		    rec[0] != '[' || rec[1] != '{')
			return (0);
		(void)desktop_key(rec + 1, end, "chat", chat, sizeof(chat));

		/* Android starts the ops of a chat in a resync in a batch. */
		if (desktop_key(rec + 1, end, "op", kind, sizeof(kind)) == NULL)
			kind[0] = '\0';

		if (!strcmp(kind, "chat") && store_chat_valid(chat) == 0)
			store_chat_drop(chat);

		(void)snprintf(kind, sizeof(kind), "batch");
	}

	if (!strcmp(kind, "rec") && data != NULL) {
		if ((rec = desktop_key(op, end, "rec", NULL, 0)) == NULL)
			return (0);
		(void)desktop_key(rec, end, "data", file, sizeof(file));
	}

	if (!strcmp(kind, "data"))
		(void)desktop_key(op, end, "file", file, sizeof(file));

	if (!strcmp(kind, "rec") || !strcmp(kind, "read") ||
	    !strcmp(kind, "batch") || !strcmp(kind, "data") ||
	    !strcmp(kind, "chat") || !strcmp(kind, "delete") ||
	    !strcmp(kind, "group")) {
		if (store_chat_valid(chat) == -1) {
			fprintf(stderr, "vigil: an op for a bad chat\n");
			return (0);
		}
	}

	if (!strcmp(kind, "read") && !store_chat_exists(chat))
		return (0);

	if (!strcmp(kind, "rec") && !store_chat_exists(chat) &&
	    !desktop_creates(op, end))
		return (0);

	if (file[0] != '\0' && (strncmp(file, chat, strlen(chat)) ||
	    file[strlen(chat)] != '-'))
		return (0);

	if (file[0] != '\0' && data != NULL) {
		if (store_data_put(file, data, size) == -1)
			return (-1);
	}

	if (!strcmp(kind, "rec") || !strcmp(kind, "read") ||
	    !strcmp(kind, "batch")) {
		ret = store_chat_append(chat, op, len);
	} else if (!strcmp(kind, "chat")) {
		store_chat_drop(chat);
	} else if (!strcmp(kind, "delete")) {
		store_chat_delete(chat);
	} else if (!strcmp(kind, "reset")) {
		store_reset();
	} else if (!strcmp(kind, "group")) {
		ret = desktop_state(op, len, "group-", chat);
	} else if (!strcmp(kind, "name")) {
		ret = desktop_state(op, len, "name-", peer);
	} else if (!strcmp(kind, "prefs")) {
		ret = desktop_state(op, len, "prefs", "");
	} else if (!strcmp(kind, "avatars")) {
		store_avatars(op, len);
	} else if (!strcmp(kind, "avatar")) {
		ret = desktop_face(data, size, "avatar-", peer, name, namelen);
	} else if (!strcmp(kind, "self")) {
		ret = desktop_state(op, len, "self", "");
		if (ret == 0 && desktop_key(op, end, "avatar", NULL, 0) != NULL)
			ret = desktop_face(data, size, "self-avatar", "",
			    name, namelen);
	}

	return (ret);
}

/*
 * Returns 1 if the given rec op holds a record that makes its chat, as on
 * the phone only a message, a group change or a topic does.
 */
static int
desktop_creates(const char *op, const char *end)
{
	const char	*rec;
	char		type[16];

	PRECOND(op != NULL);
	PRECOND(end != NULL);

	if ((rec = desktop_key(op, end, "rec", NULL, 0)) == NULL)
		return (0);

	if (desktop_key(rec, end, "t", type, sizeof(type)) == NULL)
		return (0);

	if (!strcmp(type, "msg") || !strcmp(type, "member") ||
	    !strcmp(type, "topic"))
		return (1);

	return (0);
}

/*
 * Keep the given op as the state item prefix || id, a group op without
 * info drops the item.
 */
static int
desktop_state(const char *op, size_t len, const char *prefix,
    const char *id)
{
	int		ret;
	const char	*info;
	char		file[STORE_NAME_MAX];

	PRECOND(op != NULL);
	PRECOND(prefix != NULL);
	PRECOND(id != NULL);

	ret = snprintf(file, sizeof(file), "%s%s", prefix, id);
	if (ret < 0 || (size_t)ret >= sizeof(file))
		return (0);

	info = desktop_key(op, op + len, "info", NULL, 0);

	if (info != NULL && !strncmp(info, "null", 4)) {
		store_data_drop(file);
		return (0);
	}

	ret = store_data_put(file, (const u_int8_t *)op, len);

	return (ret);
}

/*
 * Keep the given avatar as prefix || id and put that name in name, or
 * drop the avatar when there is no data.
 */
static int
desktop_face(const u_int8_t *data, size_t size, const char *prefix,
    const char *id, char *name, size_t namelen)
{
	int		ret;

	PRECOND(data != NULL || size == 0);
	PRECOND(prefix != NULL);
	PRECOND(id != NULL);
	PRECOND(name != NULL);

	ret = snprintf(name, namelen, "%s%s", prefix, id);
	if (ret < 0 || (size_t)ret >= namelen) {
		name[0] = '\0';
		return (0);
	}

	if (data == NULL) {
		store_data_drop(name);
		name[0] = '\0';
		return (0);
	}

	if (store_data_put(name, data, size) == -1) {
		name[0] = '\0';
		return (-1);
	}

	return (0);
}

/*
 * Hand the user interface an answer for a request the core could not
 * queue for our phone, we do not store that answer.
 */
static void
desktop_unsent(const char *evt, size_t len)
{
	int		ret;
	u_int64_t	ref;
	const char	*end, *field;
	const char	*flock, *peer, *owner, *group;
	char		chat[STORE_CHAT_MAX], record[512];

	PRECOND(evt != NULL);

	end = evt + len;

	if (desktop_field(evt, end, "\"what\":\"unsent\"") == NULL)
		return;

	if ((field = desktop_field(evt, end, "\"ref\":")) == NULL)
		return;
	ref = strtoull(field, NULL, 10);

	if ((flock = desktop_field(evt, end, "\"flock\":\"")) == NULL)
		return;

	if ((peer = desktop_field(evt, end, "\"peer\":\"")) == NULL)
		return;

	owner = desktop_field(evt, end, "\"owner\":\"");
	group = desktop_field(evt, end, "\"group\":\"");

	if (owner != NULL && group != NULL) {
		ret = snprintf(chat, sizeof(chat), "%.*s-g%.2s%.16s",
		    (int)strcspn(flock, "\""), flock, owner, group);
	} else {
		ret = snprintf(chat, sizeof(chat), "%.*s-%.2s",
		    (int)strcspn(flock, "\""), flock, peer);
	}

	if (ret < 0 || (size_t)ret >= sizeof(chat))
		return;

	ret = snprintf(record, sizeof(record), DESKTOP_UNSENT,
	    (u_int64_t)time(NULL) * 1000, chat, ref);
	if (ret < 0 || (size_t)ret >= sizeof(record))
		fatal("unsent record too long");

	desktop_pending(&notes, record, ret);
}

/*
 * Hand the user interface a send we held for our phone when the core
 * started, as a queued op with the event in it, we do not store it.
 */
static void
desktop_queued(const char *evt, size_t len)
{
	int		ret;
	size_t		need;
	char		*record;

	PRECOND(evt != NULL);

	if (desktop_field(evt, evt + len, "\"what\":\"queued\"") == NULL)
		return;

	need = sizeof(DESKTOP_QUEUED) + len;

	if ((record = malloc(need)) == NULL)
		fatal("malloc");

	ret = snprintf(record, need, DESKTOP_QUEUED, (int)len, evt);
	if (ret < 0 || (size_t)ret >= need)
		fatal("queued record too long");

	desktop_pending(&notes, record, ret);

	sodium_memzero(record, need);
	free(record);
}

/*
 * Remember the given record in held for the next poll of the user
 * interface.
 */
static void
desktop_pending(struct desktop_held *held, const char *record, size_t len)
{
	size_t		need;
	char		*grown;

	PRECOND(held != NULL);
	PRECOND(record != NULL);

	need = held->len + len + 1;

	if ((grown = malloc(need)) == NULL)
		fatal("malloc");

	if (held->len > 0) {
		memcpy(grown, held->buf, held->len);
		grown[held->len++] = ',';
		sodium_memzero(held->buf, held->len - 1);
		free(held->buf);
	}

	memcpy(grown + held->len, record, len);

	held->buf = grown;
	held->len += len;
}

/*
 * Forget the records we held in held for the next poll of the user
 * interface.
 */
static void
desktop_pending_drop(struct desktop_held *held)
{
	PRECOND(held != NULL);

	if (held->buf == NULL)
		return;

	sodium_memzero(held->buf, held->len);
	free(held->buf);

	held->buf = NULL;
	held->len = 0;
}

/*
 * Find the given key in the JSON from start to end, returns what comes
 * right after it or NULL.
 */
static const char *
desktop_field(const char *start, const char *end, const char *key)
{
	const char	*p;
	size_t		len;

	PRECOND(start != NULL);
	PRECOND(end != NULL);
	PRECOND(key != NULL);

	len = strlen(key);

	for (p = start; p + len <= end; p++) {
		if (!memcmp(p, key, len))
			return (p + len);
	}

	return (NULL);
}

/*
 * Find the given key at the top level of the JSON object at obj and return
 * where its value starts. If out is set the value has to be a string that
 * fits in out, we copy it there. Returns NULL if the key is not there.
 */
static const char *
desktop_key(const char *obj, const char *end, const char *key, char *out,
    size_t outlen)
{
	size_t		len;
	const char	*p, *val, *next;

	PRECOND(obj != NULL);
	PRECOND(end != NULL);
	PRECOND(key != NULL);

	if (obj >= end || *obj != '{')
		return (NULL);

	len = strlen(key);

	for (p = obj + 1; p < end && *p == '"'; p = next) {
		val = p + 1 + len + 2;
		if (val > end)
			return (NULL);

		if (!memcmp(p + 1, key, len) && p[1 + len] == '"' &&
		    p[2 + len] == ':')
			break;

		if ((next = desktop_string(p, end)) == NULL || *next != ':')
			return (NULL);

		if ((next = desktop_value(next + 1, end)) == NULL)
			return (NULL);

		if (*next != ',')
			return (NULL);
		next++;
	}

	if (p >= end || *p != '"')
		return (NULL);

	if (out == NULL)
		return (val);

	if (*val != '"')
		return (NULL);

	if ((next = desktop_string(val, end)) == NULL)
		return (NULL);

	if ((size_t)(next - val - 2) >= outlen)
		return (NULL);

	memcpy(out, val + 1, next - val - 2);
	out[next - val - 2] = '\0';

	return (val);
}

/*
 * Returns where the JSON string at p ends, NULL if it does not end before
 * end.
 */
static const char *
desktop_string(const char *p, const char *end)
{
	PRECOND(p != NULL);
	PRECOND(*p == '"');

	for (p++; p < end; p++) {
		if (*p == '\\')
			p++;
		else if (*p == '"')
			return (p + 1);
	}

	return (NULL);
}

/*
 * Returns where the JSON value at p ends, NULL if it does not end before
 * end.
 */
static const char *
desktop_value(const char *p, const char *end)
{
	const char	*next;

	PRECOND(p != NULL);

	if (p >= end)
		return (NULL);

	if (*p == '"')
		return (desktop_string(p, end));

	if (*p == '{' || *p == '[') {
		if ((next = desktop_skip(p)) == NULL || next > end)
			return (NULL);
		return (next);
	}

	while (p < end && *p != ',' && *p != '}' && *p != ']')
		p++;

	return (p);
}

/*
 * Skip over the JSON value at p, returns where it ends or NULL if it does
 * not end.
 */
static const char *
desktop_skip(const char *p)
{
	int		depth, quoted;

	PRECOND(p != NULL);

	depth = 0;
	quoted = 0;

	for (; *p != '\0'; p++) {
		if (quoted) {
			if (*p == '\\' && p[1] != '\0')
				p++;
			else if (*p == '"')
				quoted = 0;
			continue;
		}

		if (*p == '"') {
			quoted = 1;
		} else if (*p == '{' || *p == '[') {
			depth++;
		} else if (*p == '}' || *p == ']') {
			depth--;
			if (depth == 0)
				return (p + 1);
		}
	}

	return (NULL);
}

/*
 * Take the given key from the map as a hex number, returns -1 if it is
 * not there.
 */
static int
desktop_hex(FlValue *map, const char *key, u_int64_t *out)
{
	char		*ep;
	FlValue		*val;
	const char	*str;

	PRECOND(map != NULL);
	PRECOND(key != NULL);
	PRECOND(out != NULL);

	if ((val = fl_value_lookup_string(map, key)) == NULL)
		return (-1);

	if (fl_value_get_type(val) != FL_VALUE_TYPE_STRING)
		return (-1);

	str = fl_value_get_string(val);
	if (*str == '\0')
		return (-1);

	errno = 0;
	*out = strtoull(str, &ep, 16);

	if (*ep != '\0' || errno != 0)
		return (-1);

	return (0);
}

/*
 * Take the members in the map as the peers a group message goes to.
 */
static int
desktop_targets(FlValue *map, u_int8_t *targets)
{
	char		*ep;
	unsigned long	peer;
	size_t		idx, count;
	FlValue		*list, *item;

	PRECOND(map != NULL);
	PRECOND(targets != NULL);

	if ((list = fl_value_lookup_string(map, "members")) == NULL)
		return (-1);

	if (fl_value_get_type(list) != FL_VALUE_TYPE_LIST)
		return (-1);

	count = fl_value_get_length(list);

	for (idx = 0; idx < count; idx++) {
		item = fl_value_get_list_value(list, idx);
		if (fl_value_get_type(item) != FL_VALUE_TYPE_STRING)
			return (-1);

		peer = strtoul(fl_value_get_string(item), &ep, 16);
		if (*ep != '\0' || peer == 0 || peer >= KYRKA_PEERS_PER_FLOCK)
			return (-1);

		targets[peer / 8] |= 1 << (peer % 8);
	}

	return (0);
}

/*
 * Make the ref of an ask we make ourselves, as the user interface does.
 */
static u_int64_t
desktop_ref(void)
{
	u_int64_t	now;

	now = g_get_real_time();

	return (VIGIL_CHAT_COMPANION_REF | now);
}

/*
 * Keep the given data we send under ref in the chat the map names, under
 * the name the phone gives the data of the record, put in name.
 */
static int
desktop_keep(FlValue *args, u_int64_t ref, const u_int8_t *data, size_t len,
    char *name, size_t namelen)
{
	int		ret;
	u_int64_t	flock, peer, owner, group;

	PRECOND(args != NULL);
	PRECOND(data != NULL);
	PRECOND(name != NULL);

	if (desktop_hex(args, "flock", &flock) == -1 || flock == 0)
		return (-1);

	if (desktop_hex(args, "group", &group) == 0 && group != 0) {
		if (desktop_hex(args, "owner", &owner) == -1 ||
		    owner == 0 || owner >= KYRKA_PEERS_PER_FLOCK)
			return (-1);
		ret = snprintf(name, namelen, "%" PRIx64 "-g%02" PRIx64
		    "%016" PRIx64 "-%016" PRIx64 ".dat", flock, owner,
		    group, ref);
	} else {
		if (desktop_hex(args, "peer", &peer) == -1 ||
		    peer == 0 || peer >= KYRKA_PEERS_PER_FLOCK)
			return (-1);
		ret = snprintf(name, namelen, "%" PRIx64 "-%02" PRIx64
		    "-%016" PRIx64 ".dat", flock, peer, ref);
	}

	if (ret < 0 || (size_t)ret >= namelen)
		return (-1);

	ret = store_data_put(name, data, len);

	return (ret);
}

/*
 * Read the file at the given path, returns NULL if it cannot be read, is
 * empty or holds more than VIGIL_CHAT_DATA_MAX bytes.
 */
static u_int8_t *
desktop_read(const char *path, size_t *len)
{
	u_int8_t	*data;

	PRECOND(path != NULL);
	PRECOND(len != NULL);

	if ((data = store_read(path, VIGIL_CHAT_DATA_MAX, len)) == NULL)
		return (NULL);

	if (*len == 0) {
		free(data);
		return (NULL);
	}

	return (data);
}

/*
 * Keep the given data and have our phone send it, answer the given call
 * with the name we keep the data under, its name and its size.
 */
static void
desktop_send_kept(FlMethodCall *call, FlValue *args, u_int64_t ref, int kind,
    const char *name, const u_int8_t *data, size_t len)
{
	FlValue		*out;
	char		file[STORE_NAME_MAX];

	PRECOND(call != NULL);
	PRECOND(args != NULL);
	PRECOND(name != NULL);
	PRECOND(data != NULL);

	if (desktop_keep(args, ref, data, len, file, sizeof(file)) == -1) {
		fl_method_call_respond_error(call, "send",
		    "It could not be sent.", NULL, NULL);
		return;
	}

	if (desktop_send_to(args, ref, NULL, 0, kind, name, data, len) == -1) {
		store_data_drop(file);
		fl_method_call_respond_error(call, "send",
		    "It could not be sent.", NULL, NULL);
		return;
	}

	out = fl_value_new_map();
	fl_value_set_string_take(out, "file", fl_value_new_string(file));
	fl_value_set_string_take(out, "name", fl_value_new_string(name));
	fl_value_set_string_take(out, "size", fl_value_new_int(len));

	fl_method_call_respond_success(call, out, NULL);
	fl_value_unref(out);
}

/*
 * Keep the data the user interface hands us and have our phone send that
 * data.
 */
static void
desktop_send_data(FlMethodCall *call)
{
	u_int64_t	ref;
	int		which;
	const char	*what;
	FlValue		*args, *kind, *name, *data;

	args = fl_method_call_get_args(call);

	if (run_id == 0) {
		fl_method_call_respond_error(call, "offline",
		    "Vigil is not linked to a phone.", NULL, NULL);
		return;
	}

	if (fl_value_get_type(args) != FL_VALUE_TYPE_MAP ||
	    (kind = fl_value_lookup_string(args, "kind")) == NULL ||
	    (name = fl_value_lookup_string(args, "name")) == NULL ||
	    (data = fl_value_lookup_string(args, "data")) == NULL ||
	    fl_value_get_type(kind) != FL_VALUE_TYPE_STRING ||
	    fl_value_get_type(name) != FL_VALUE_TYPE_STRING ||
	    fl_value_get_type(data) != FL_VALUE_TYPE_UINT8_LIST ||
	    fl_value_get_length(data) == 0 ||
	    fl_value_get_length(data) > VIGIL_CHAT_DATA_MAX ||
	    strlen(fl_value_get_string(name)) > VIGIL_CHAT_DATA_NAME_MAX ||
	    desktop_hex(args, "ref", &ref) == -1 ||
	    !(ref & VIGIL_CHAT_COMPANION_REF)) {
		fl_method_call_respond_error(call, "args", "Bad data.",
		    NULL, NULL);
		return;
	}

	what = fl_value_get_string(kind);

	if (!strcmp(what, "image"))
		which = VIGIL_CHAT_DATA_IMAGE;
	else if (!strcmp(what, "voice"))
		which = VIGIL_CHAT_DATA_VOICE;
	else
		which = VIGIL_CHAT_DATA_FILE;

	desktop_send_kept(call, args, ref, which, fl_value_get_string(name),
	    fl_value_get_uint8_list(data), fl_value_get_length(data));
}

/*
 * Hand back the image on the clipboard as PNG, nothing if there is none.
 */
static void
desktop_clipboard(FlMethodCall *call)
{
	gsize		len;
	gchar		*buf;
	GdkPixbuf	*image;
	FlValue		*reply;
	GtkClipboard	*clipboard;

	clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);

	if (!gtk_clipboard_wait_is_image_available(clipboard)) {
		fl_method_call_respond_success(call, NULL, NULL);
		return;
	}

	if ((image = gtk_clipboard_wait_for_image(clipboard)) == NULL) {
		fl_method_call_respond_success(call, NULL, NULL);
		return;
	}

	if (!gdk_pixbuf_save_to_buffer(image, &buf, &len, "png", NULL, NULL)) {
		g_object_unref(image);
		fl_method_call_respond_success(call, NULL, NULL);
		return;
	}

	g_object_unref(image);

	if (len == 0 || len > VIGIL_CHAT_DATA_MAX) {
		g_free(buf);
		fl_method_call_respond_error(call, "file",
		    "That image is larger than 4 MB.", NULL, NULL);
		return;
	}

	reply = fl_value_new_uint8_list((const u_int8_t *)buf, len);
	fl_method_call_respond_success(call, reply, NULL);
	fl_value_unref(reply);

	sodium_memzero(buf, len);
	g_free(buf);
}

/*
 * Ask the platform of our phone to do what the JSON says.
 */
static void
desktop_ask(FlMethodCall *call)
{
	size_t		len;
	u_int64_t	ref;
	FlValue		*args;
	const char	*json, *id;

	args = fl_method_call_get_args(call);

	if (run_id == 0) {
		fl_method_call_respond_error(call, "offline",
		    "Vigil is not linked to a phone.", NULL, NULL);
		return;
	}

	if (fl_value_get_type(args) != FL_VALUE_TYPE_STRING) {
		fl_method_call_respond_error(call, "args", "Nothing to ask.",
		    NULL, NULL);
		return;
	}

	json = fl_value_get_string(args);
	len = strlen(json);

	if (len == 0 || len > VIGIL_CHAT_MESSAGE_MAX ||
	    (id = desktop_field(json, json + len, "\"id\":")) == NULL) {
		fl_method_call_respond_error(call, "args", "Nothing to ask.",
		    NULL, NULL);
		return;
	}

	ref = strtoull(id, NULL, 10);

	if (!(ref & VIGIL_CHAT_COMPANION_REF) ||
	    vigil_chat_companion_ask(ref, json, len, NULL, 0) == -1) {
		fl_method_call_respond_error(call, "ask",
		    "Vigil could not ask your phone.", NULL, NULL);
		return;
	}

	fl_method_call_respond_success(call, NULL, NULL);
}

/*
 * Let the user pick a picture for our phone to use as our avatar, or have
 * our phone drop our avatar when the user interface says so.
 */
static void
desktop_avatar(FlMethodCall *call)
{
	size_t		len;
	int		ret;
	u_int64_t	ref;
	u_int8_t	*data;
	char		*path;
	FlValue		*args;
	FlValue		*reply;
	int		asklen;
	char		ask[64];

	args = fl_method_call_get_args(call);

	ref = desktop_ref();

	asklen = snprintf(ask, sizeof(ask), DESKTOP_AVATAR, ref);
	if (asklen < 0 || (size_t)asklen >= sizeof(ask))
		fatal("snprintf: avatar ask");

	if (run_id == 0) {
		fl_method_call_respond_error(call, "offline",
		    "Vigil is not linked to a phone.", NULL, NULL);
		return;
	}

	if (fl_value_get_type(args) == FL_VALUE_TYPE_BOOL &&
	    !fl_value_get_bool(args)) {
		ret = vigil_chat_companion_ask(ref, ask, asklen, NULL, 0);
		reply = fl_value_new_bool(ret == 0);
		fl_method_call_respond_success(call, reply, NULL);
		fl_value_unref(reply);
		return;
	}

	if (runner_pick(window, "Pick your picture", &path) == -1) {
		reply = fl_value_new_bool(FALSE);
		fl_method_call_respond_success(call, reply, NULL);
		fl_value_unref(reply);
		return;
	}

	data = desktop_read(path, &len);
	free(path);

	if (data == NULL) {
		fl_method_call_respond_error(call, "file",
		    "That picture cannot be read, is empty or is over 4 MB.",
		    NULL, NULL);
		return;
	}

	ret = vigil_chat_companion_ask(ref, ask, asklen, data, len);

	sodium_memzero(data, len);
	free(data);

	if (ret == -1) {
		fl_method_call_respond_error(call, "ask",
		    "Vigil could not ask your phone.", NULL, NULL);
		return;
	}

	reply = fl_value_new_bool(TRUE);
	fl_method_call_respond_success(call, reply, NULL);
	fl_value_unref(reply);
}

/*
 * Tell the core if we type, to a peer or to the members of a group.
 */
static void
desktop_typing(FlMethodCall *call)
{
	FlValue		*args, *on;
	u_int8_t	targets[VIGIL_CHAT_GROUP_ROSTER];
	u_int64_t	peer, owner, group, topic, flock;

	args = fl_method_call_get_args(call);

	if (run_id == 0 || fl_value_get_type(args) != FL_VALUE_TYPE_MAP ||
	    (on = fl_value_lookup_string(args, "on")) == NULL ||
	    fl_value_get_type(on) != FL_VALUE_TYPE_BOOL ||
	    desktop_hex(args, "flock", &flock) == -1) {
		fl_method_call_respond_success(call, NULL, NULL);
		return;
	}

	topic = 0;
	(void)desktop_hex(args, "topic", &topic);

	memset(targets, 0, sizeof(targets));

	if (desktop_hex(args, "group", &group) == 0 && group != 0) {
		if (desktop_hex(args, "owner", &owner) == 0 && owner > 0 &&
		    owner < KYRKA_PEERS_PER_FLOCK &&
		    desktop_targets(args, targets) == 0) {
			if (fl_value_get_bool(on)) {
				vigil_chat_group_typing(flock, owner, group,
				    topic, targets);
			} else {
				vigil_chat_group_typing(0, 0, 0, 0, NULL);
			}
		}
	} else if (desktop_hex(args, "peer", &peer) == 0 && peer > 0 &&
	    peer < KYRKA_PEERS_PER_FLOCK) {
		vigil_chat_typing(flock, peer, topic, fl_value_get_bool(on));
	}

	fl_method_call_respond_success(call, NULL, NULL);
}

/*
 * Tell the core which chat we have open, our phone keeps that peer close.
 */
static void
desktop_open(FlMethodCall *call)
{
	FlValue		*args, *on;
	u_int64_t	peer, flock;

	args = fl_method_call_get_args(call);

	if (run_id != 0 && fl_value_get_type(args) == FL_VALUE_TYPE_MAP &&
	    (on = fl_value_lookup_string(args, "on")) != NULL &&
	    fl_value_get_type(on) == FL_VALUE_TYPE_BOOL &&
	    desktop_hex(args, "flock", &flock) == 0 &&
	    desktop_hex(args, "peer", &peer) == 0 && peer > 0 &&
	    peer < KYRKA_PEERS_PER_FLOCK)
		vigil_chat_open(flock, peer, fl_value_get_bool(on));

	fl_method_call_respond_success(call, NULL, NULL);
}

/*
 * Hand back the hash our phones use for the data we keep under each of the
 * given names, the first 8 bytes of its SHA-256 as hex, or nothing for
 * data we do not have.
 */
static void
desktop_hash(FlMethodCall *call)
{
	size_t		len;
	u_int8_t	*data;
	const char	*name;
	char		hex[17];
	size_t		idx, count;
	FlValue		*args, *out, *item;
	u_int8_t	sum[crypto_hash_sha256_BYTES];

	args = fl_method_call_get_args(call);

	if (fl_value_get_type(args) != FL_VALUE_TYPE_LIST) {
		fl_method_call_respond_error(call, "args", "No list.",
		    NULL, NULL);
		return;
	}

	out = fl_value_new_list();
	count = fl_value_get_length(args);

	for (idx = 0; idx < count; idx++) {
		item = fl_value_get_list_value(args, idx);
		data = NULL;

		if (fl_value_get_type(item) == FL_VALUE_TYPE_STRING) {
			name = fl_value_get_string(item);
			if (strlen(name) > 0 && strlen(name) < STORE_NAME_MAX)
				data = store_data_get(name, &len);
		}

		if (data == NULL) {
			fl_value_append_take(out, fl_value_new_string(""));
			continue;
		}

		crypto_hash_sha256(sum, data, len);
		sodium_memzero(data, len);
		free(data);

		if (!memcmp(sum, "\0\0\0\0\0\0\0\0", 8))
			sum[7] = 1;

		(void)snprintf(hex, sizeof(hex),
		    "%02x%02x%02x%02x%02x%02x%02x%02x", sum[0], sum[1],
		    sum[2], sum[3], sum[4], sum[5], sum[6], sum[7]);

		fl_value_append_take(out, fl_value_new_string(hex));
	}

	fl_method_call_respond_success(call, out, NULL);
	fl_value_unref(out);
}

/*
 * Wipe the data we keep under each of the given names.
 */
static void
desktop_drop(FlMethodCall *call)
{
	const char	*name;
	size_t		idx, count;
	FlValue		*args, *item;

	args = fl_method_call_get_args(call);

	if (fl_value_get_type(args) == FL_VALUE_TYPE_LIST) {
		count = fl_value_get_length(args);

		for (idx = 0; idx < count; idx++) {
			item = fl_value_get_list_value(args, idx);
			if (fl_value_get_type(item) != FL_VALUE_TYPE_STRING)
				continue;

			name = fl_value_get_string(item);
			if (strlen(name) > 0 && strlen(name) < STORE_NAME_MAX)
				store_data_drop(name);
		}
	}

	fl_method_call_respond_success(call, NULL, NULL);
}

/*
 * Ask our phone to do something with a call for us.
 */
static void
desktop_call_op(FlMethodCall *call)
{
	int		op;
	const char	*what;
	FlValue		*args, *val;
	u_int64_t	peer, flock;

	args = fl_method_call_get_args(call);

	if (run_id == 0) {
		fl_method_call_respond_error(call, "offline",
		    "Vigil is not linked to a phone.", NULL, NULL);
		return;
	}

	if (fl_value_get_type(args) != FL_VALUE_TYPE_MAP ||
	    (val = fl_value_lookup_string(args, "op")) == NULL ||
	    fl_value_get_type(val) != FL_VALUE_TYPE_STRING ||
	    desktop_hex(args, "flock", &flock) == -1 || flock == 0 ||
	    desktop_hex(args, "peer", &peer) == -1 || peer == 0 ||
	    peer >= KYRKA_PEERS_PER_FLOCK) {
		fl_method_call_respond_error(call, "args", "Bad call.",
		    NULL, NULL);
		return;
	}

	what = fl_value_get_string(val);

	if (!strcmp(what, "mute")) {
		vigil_voice_mute(1);
		fl_method_call_respond_success(call, NULL, NULL);
		return;
	}

	if (!strcmp(what, "unmute")) {
		vigil_voice_mute(0);
		fl_method_call_respond_success(call, NULL, NULL);
		return;
	}

	if (!strcmp(what, "dial")) {
		op = VIGIL_COMPANION_DIAL;
	} else if (!strcmp(what, "answer")) {
		op = VIGIL_COMPANION_ANSWER;
	} else if (!strcmp(what, "decline")) {
		op = VIGIL_COMPANION_DECLINE;
	} else if (!strcmp(what, "hangup")) {
		op = VIGIL_COMPANION_HANGUP;
	} else {
		fl_method_call_respond_error(call, "args", "Bad call.",
		    NULL, NULL);
		return;
	}

	if (op == VIGIL_COMPANION_DIAL || op == VIGIL_COMPANION_ANSWER)
		memo_stop();

	if (vigil_chat_call(op, flock, peer) == -1) {
		fl_method_call_respond_error(call, "call",
		    "Vigil could not ask your phone.", NULL, NULL);
		return;
	}

	fl_method_call_respond_success(call, NULL, NULL);
}

/*
 * Hand back the state of the call we hold for our phone.
 */
static void
desktop_voice(FlMethodCall *call)
{
	FlValue		*reply;
	char		buf[1024];

	if (vigil_voice_status(buf, sizeof(buf)) == 0)
		(void)snprintf(buf, sizeof(buf), "{}");

	reply = fl_value_new_string(buf);
	fl_method_call_respond_success(call, reply, NULL);
	fl_value_unref(reply);
}

/*
 * Start recording a voice message.
 */
static void
desktop_memo_record(FlMethodCall *call)
{
	if (memo_record() == -1) {
		fl_method_call_respond_error(call, "voice",
		    "Vigil cannot record right now.", NULL, NULL);
		return;
	}

	fl_method_call_respond_success(call, NULL, NULL);
}

/*
 * Stop recording and throw away what we have.
 */
static void
desktop_memo_cancel(FlMethodCall *call)
{
	size_t		len;
	u_int8_t	*data;

	if ((data = memo_finish(&len)) != NULL) {
		sodium_memzero(data, len);
		free(data);
	}

	fl_method_call_respond_success(call, NULL, NULL);
}

/*
 * Stop recording, keep the recording and have our phone send the
 * recording.
 */
static void
desktop_memo_send(FlMethodCall *call)
{
	struct tm	tm;
	time_t		now;
	size_t		len;
	u_int64_t	ref;
	u_int8_t	*data;
	FlValue		*args;
	char		name[64];

	args = fl_method_call_get_args(call);

	if ((data = memo_finish(&len)) == NULL) {
		fl_method_call_respond_error(call, "voice",
		    "Nothing was recorded.", NULL, NULL);
		return;
	}

	now = time(NULL);
	(void)localtime_r(&now, &tm);

	if (strftime(name, sizeof(name), "voice-%Y%m%d-%H%M%S.aac", &tm) == 0)
		fatal("failed to name a voice message");

	if (run_id == 0 || fl_value_get_type(args) != FL_VALUE_TYPE_MAP ||
	    desktop_hex(args, "ref", &ref) == -1 ||
	    !(ref & VIGIL_CHAT_COMPANION_REF)) {
		fl_method_call_respond_error(call, "voice",
		    "The voice message could not be sent.", NULL, NULL);
	} else {
		desktop_send_kept(call, args, ref, VIGIL_CHAT_DATA_VOICE,
		    name, data, len);
	}

	sodium_memzero(data, len);
	free(data);
}

/*
 * Play the voice message we keep under the given name, we answer once it
 * stopped playing.
 */
static void
desktop_memo_play(FlMethodCall *call)
{
	int64_t		at;
	int		ret;
	size_t		len;
	double		rate;
	u_int8_t	*data;
	FlValue		*args, *file, *val;

	args = fl_method_call_get_args(call);

	if (fl_value_get_type(args) != FL_VALUE_TYPE_MAP ||
	    (file = fl_value_lookup_string(args, "file")) == NULL ||
	    fl_value_get_type(file) != FL_VALUE_TYPE_STRING ||
	    strlen(fl_value_get_string(file)) >= STORE_NAME_MAX) {
		fl_method_call_respond_error(call, "args", "Bad file.",
		    NULL, NULL);
		return;
	}

	at = 0;
	rate = 1;

	if ((val = fl_value_lookup_string(args, "at")) != NULL &&
	    fl_value_get_type(val) == FL_VALUE_TYPE_INT)
		at = fl_value_get_int(val);

	if ((val = fl_value_lookup_string(args, "rate")) != NULL &&
	    fl_value_get_type(val) == FL_VALUE_TYPE_FLOAT)
		rate = fl_value_get_float(val);

	if (rate < 0.5 || rate > 3)
		rate = 1;

	if ((data = store_data_get(fl_value_get_string(file), &len)) == NULL) {
		fl_method_call_respond_error(call, "file",
		    "That voice message is no longer here.", NULL, NULL);
		return;
	}

	ret = memo_play(data, len, at, rate, call);

	sodium_memzero(data, len);
	free(data);

	if (ret == -1) {
		fl_method_call_respond_error(call, "voice",
		    "That voice message cannot be played right now.",
		    NULL, NULL);
	}
}

/*
 * Jump to the given millisecond in the voice message we play.
 */
static void
desktop_memo_seek(FlMethodCall *call)
{
	FlValue		*args;

	args = fl_method_call_get_args(call);

	if (fl_value_get_type(args) == FL_VALUE_TYPE_INT)
		memo_seek(fl_value_get_int(args));

	fl_method_call_respond_success(call, NULL, NULL);
}

/*
 * Change how fast we play the voice message.
 */
static void
desktop_memo_speed(FlMethodCall *call)
{
	FlValue		*args;

	args = fl_method_call_get_args(call);

	if (fl_value_get_type(args) == FL_VALUE_TYPE_FLOAT &&
	    fl_value_get_float(args) >= 0.5 && fl_value_get_float(args) <= 3)
		memo_speed(fl_value_get_float(args));

	fl_method_call_respond_success(call, NULL, NULL);
}

/*
 * Hand back where we are in the voice message we play and how long it
 * is in milliseconds, nothing if we play none.
 */
static void
desktop_memo_position(FlMethodCall *call)
{
	FlValue		*out;
	u_int64_t	at, len;

	if (memo_position(&at, &len) == -1) {
		fl_method_call_respond_success(call, NULL, NULL);
		return;
	}

	out = fl_value_new_list();
	fl_value_append_take(out, fl_value_new_int(at));
	fl_value_append_take(out, fl_value_new_int(len));

	fl_method_call_respond_success(call, out, NULL);
	fl_value_unref(out);
}

/*
 * Open the given link in the browser of the user.
 */
static void
desktop_url(FlMethodCall *call)
{
	const char	*url;
	FlValue		*args;
	GError		*error;

	args = fl_method_call_get_args(call);

	if (fl_value_get_type(args) != FL_VALUE_TYPE_STRING) {
		fl_method_call_respond_error(call, "args", "No link.",
		    NULL, NULL);
		return;
	}

	url = fl_value_get_string(args);

	if (strncmp(url, "https://", 8) && strncmp(url, "http://", 7)) {
		fl_method_call_respond_error(call, "args", "Not a web link.",
		    NULL, NULL);
		return;
	}

	error = NULL;

	if (!gtk_show_uri_on_window(window, url, GDK_CURRENT_TIME, &error)) {
		fl_method_call_respond_error(call, "url", error->message,
		    NULL, NULL);
		g_error_free(error);
		return;
	}

	fl_method_call_respond_success(call, NULL, NULL);
}
