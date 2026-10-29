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
 * chatter - run a Gospel configuration on a regular host and chat.
 *
 *	$ ./tests/chatter [-n nick] [-m peer:text] [-g group] [-G text]
 *	    [-i peer:file] [-I file] [-f peer:file] [-F file] [-o dir]
 *	    [-t peer] [-T] [-O peer] [-e count] [-x secs] [-l count]
 *	    [-D datadir] <gospel>
 *
 * With -m we send the text to the given peer, -m may be given more
 * than once. With -g we join the given group and with -G we send the
 * text to it once a member is online, -G may be given more than once.
 * With -t we keep typing to the given peer and with -T to our group.
 * With -O we keep the chat with the given peer open.
 * With -i we send the image in the given file to the given peer and with
 * -I to our group, -f and -F do the same for any file. All of them may be
 * given more than once. Data we get is written to the directory given
 * with -o.
 * With -e we wait until we got that many messages and with -x we give
 * up after that many seconds. With -l we act as if we failed to store
 * the first count messages we got.
 *
 * Exits 0 if all our messages were delivered, all our group messages
 * were acked by every member and we got all the messages we waited for,
 * 1 otherwise.
 */

#include <sys/types.h>
#include <sys/stat.h>

#include <inttypes.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

#include "vigil.h"

/* Where the status tells us the chat session, our group and our traffic. */
#define CHATTER_SESSION		"\"chat\":{\"session\":"
#define CHATTER_GROUP		"\"group\":{"
#define CHATTER_GOSPEL		"\"gospel\":{"

/* What the tally of a message to our group looks like, and when it is done. */
#define CHATTER_GROUP_ACKS	"\"group\":\"%04lx\",\"type\":\"acks\""
#define CHATTER_GROUP_DONE	",\"pending\":0}"

/* The milliseconds we stay around once we are done. */
#define CHATTER_LINGER		3000

/* The maximum number of messages we send. */
#define CHATTER_MESSAGES_MAX	16

static void	usage(void) __attribute__((noreturn));

static void	*chatter_core_run(void *);
static void	chatter_data_take(const char *);
static void	chatter_events_handle(int *, int *, int *);
static size_t	chatter_config_read(const char *, char *, size_t);
static void	chatter_data_send(u_int8_t, u_int64_t, int, const char *);

/* The Gospel configuration. */
static size_t		gospel_len;
static char		gospel[VIGIL_CONFIG_MAX];

/* The number of messages we do not store. */
static int		lose = 0;

/* Where the core keeps its remembrances, if anywhere. */
static const char	*datadir = NULL;

/* The messages we send. */
static int		message_count = 0;
static u_int8_t		message_peer[CHATTER_MESSAGES_MAX];
static const char	*message_text[CHATTER_MESSAGES_MAX];

/* The group we join and the messages we send to it. */
static long		group = 0;
static int		group_count = 0;
static const char	*group_text[CHATTER_MESSAGES_MAX];

/* The data we send to peers and to our group, where the data we get goes. */
static int		data_count = 0;
static int		data_kind[CHATTER_MESSAGES_MAX];
static u_int8_t		data_peer[CHATTER_MESSAGES_MAX];
static const char	*data_path[CHATTER_MESSAGES_MAX];
static int		group_data_count = 0;
static int		group_data_kind[CHATTER_MESSAGES_MAX];
static const char	*group_data_path[CHATTER_MESSAGES_MAX];
static const char	*outdir = NULL;

/* Who we keep typing to. */
static long		typing_peer = 0;
static int		typing_group = 0;

/* Who we keep a chat open with. */
static long		open_peer = 0;

static void
usage(void)
{
	fprintf(stderr, "usage: chatter [-n nick] [-m peer:text] [-g group] "
	    "[-G text] [-i peer:file] [-I file] [-f peer:file] [-F file] "
	    "[-a peer:file] [-A file] "
	    "[-o dir] [-t peer] [-T] [-O peer] [-e count] [-x secs] "
	    "[-l count] [-D datadir] <gospel>\n");
	exit(1);
}

int
main(int argc, char **argv)
{
	pthread_t	core;
	char		*p, *ep;
	u_int64_t	id, start;
	char		summary[4096];
	long		peer, session;
	int		ch, idx, expect, quit, delivered;
	int		got, sent, gsent, gdone, wanted, gwanted;

	quit = 60;
	got = 0;
	sent = 0;
	gsent = 0;
	gdone = 0;
	expect = 0;
	session = 0;
	delivered = 0;

	while ((ch = getopt(argc, argv,
	    "a:A:D:e:f:F:g:G:i:I:l:m:n:o:O:t:Tx:")) != -1) {
		switch (ch) {
		case 'D':
			datadir = optarg;
			break;
		case 'e':
			expect = atoi(optarg);
			break;
		case 'g':
			group = strtol(optarg, &ep, 16);
			if (*ep != '\0')
				usage();
			if (group <= 0 || group > 0xffff)
				usage();
			break;
		case 'G':
			if (group_count == CHATTER_MESSAGES_MAX)
				usage();
			if (strlen(optarg) == 0 ||
			    strlen(optarg) > VIGIL_CHAT_MESSAGE_MAX)
				usage();
			group_text[group_count++] = optarg;
			break;
		case 'm':
			if (message_count == CHATTER_MESSAGES_MAX)
				usage();
			peer = strtol(optarg, &ep, 16);
			if (*ep != ':')
				usage();
			if (peer <= 0 || peer >= KYRKA_PEERS_PER_FLOCK)
				usage();
			if (strlen(ep + 1) == 0 ||
			    strlen(ep + 1) > VIGIL_CHAT_MESSAGE_MAX)
				usage();
			message_peer[message_count] = peer;
			message_text[message_count] = ep + 1;
			message_count++;
			break;
		case 'n':
			if (vigil_chat_nick(optarg) == -1)
				usage();
			break;
		case 'a':
		case 'f':
		case 'i':
			if (data_count == CHATTER_MESSAGES_MAX)
				usage();
			peer = strtol(optarg, &ep, 16);
			if (*ep != ':')
				usage();
			if (peer <= 0 || peer >= KYRKA_PEERS_PER_FLOCK)
				usage();
			data_peer[data_count] = peer;
			data_path[data_count] = ep + 1;
			if (ch == 'i')
				data_kind[data_count] = VIGIL_CHAT_DATA_IMAGE;
			else if (ch == 'a')
				data_kind[data_count] = VIGIL_CHAT_DATA_VOICE;
			else
				data_kind[data_count] = VIGIL_CHAT_DATA_FILE;
			data_count++;
			break;
		case 'A':
		case 'F':
		case 'I':
			if (group_data_count == CHATTER_MESSAGES_MAX)
				usage();
			idx = group_data_count++;
			group_data_path[idx] = optarg;
			if (ch == 'I')
				group_data_kind[idx] = VIGIL_CHAT_DATA_IMAGE;
			else if (ch == 'A')
				group_data_kind[idx] = VIGIL_CHAT_DATA_VOICE;
			else
				group_data_kind[idx] = VIGIL_CHAT_DATA_FILE;
			break;
		case 'l':
			lose = atoi(optarg);
			break;
		case 'o':
			outdir = optarg;
			break;
		case 't':
			typing_peer = strtol(optarg, &ep, 16);
			if (*ep != '\0')
				usage();
			if (typing_peer <= 0 ||
			    typing_peer >= KYRKA_PEERS_PER_FLOCK)
				usage();
			break;
		case 'T':
			typing_group = 1;
			break;
		case 'O':
			open_peer = strtol(optarg, &ep, 16);
			if (*ep != '\0')
				usage();
			if (open_peer <= 0 ||
			    open_peer >= KYRKA_PEERS_PER_FLOCK)
				usage();
			break;
		case 'x':
			quit = atoi(optarg);
			break;
		default:
			usage();
		}
	}

	argc -= optind;
	argv += optind;

	if (argc != 1)
		usage();

	if (group_count > 0 && group == 0)
		usage();

	if (group_data_count > 0 && group == 0)
		usage();

	wanted = message_count + data_count;
	gwanted = group_count + group_data_count;

	if (typing_group && group == 0)
		usage();

	gospel_len = chatter_config_read(argv[0], gospel, sizeof(gospel));

	if (vigil_config_check(gospel, gospel_len,
	    summary, sizeof(summary)) == -1)
		fatal("%s", summary);

	signal(SIGPIPE, SIG_IGN);

	id = vigil_run_id();

	if (pthread_create(&core, NULL, chatter_core_run, &id) != 0)
		fatal("pthread_create");

	p = calloc(1, VIGIL_STATUS_MAX);
	if (p == NULL)
		fatal("calloc");

	start = vigil_time_ms();

	while ((vigil_time_ms() - start) < (u_int64_t)quit * 1000) {
		(void)poll(NULL, 0, 500);

		if (vigil_status(p, VIGIL_STATUS_MAX) > 0 &&
		    (ep = strstr(p, CHATTER_SESSION)) != NULL) {
			ep += strlen(CHATTER_SESSION);

			if (strtol(ep, NULL, 10) != session) {
				session = strtol(ep, NULL, 10);

				for (idx = 0; idx < message_count; idx++) {
					if (vigil_chat_send(message_peer[idx],
					    idx + 1, message_text[idx],
					    strlen(message_text[idx])) == -1)
						fatal("vigil_chat_send");
				}

				for (idx = 0; idx < data_count; idx++) {
					chatter_data_send(data_peer[idx],
					    500 + idx, data_kind[idx],
					    data_path[idx]);
				}

				if (group != 0)
					vigil_chat_group(group);

				if (typing_peer != 0)
					vigil_chat_typing(typing_peer, 1);

				if (open_peer != 0)
					vigil_chat_open(open_peer, 1);

				if (typing_group)
					vigil_chat_group_typing(1);

				sent = 1;
				gsent = 0;
			}

			if (gsent == 0 && gwanted > 0 &&
			    (ep = strstr(p, CHATTER_GROUP)) != NULL &&
			    strstr(ep, "\"online\":true") != NULL) {
				for (idx = 0; idx < group_count; idx++) {
					if (vigil_chat_group_send(1000 + idx,
					    group_text[idx],
					    strlen(group_text[idx])) == -1)
						fatal("vigil_chat_group_send");
				}

				for (idx = 0; idx < group_data_count; idx++) {
					chatter_data_send(0, 1500 + idx,
					    group_data_kind[idx],
					    group_data_path[idx]);
				}

				gsent = 1;
			}
		}

		chatter_events_handle(&got, &delivered, &gdone);

		if (sent && delivered >= wanted &&
		    gdone >= gwanted && got >= expect)
			break;
	}

	printf("chatter: got %d/%d, delivered %d/%d, group acked %d/%d\n",
	    got, expect, delivered, wanted, gdone, gwanted);

	if (vigil_status(p, VIGIL_STATUS_MAX) > 0 &&
	    (ep = strstr(p, CHATTER_GOSPEL)) != NULL)
		printf("chatter: %.*s\n", (int)strcspn(ep, "}") + 1, ep);
	fflush(stdout);

	/* Give our acks a few pulses to go out. */
	(void)poll(NULL, 0, CHATTER_LINGER);

	vigil_stop(id);
	pthread_join(core, NULL);

	free(p);

	if (got >= expect && delivered >= wanted && gdone >= gwanted)
		return (0);

	return (1);
}

/*
 * Read the configuration at path into buf, returns its length.
 */
static size_t
chatter_config_read(const char *path, char *buf, size_t len)
{
	FILE		*fp;
	size_t		ret;

	PRECOND(path != NULL);
	PRECOND(buf != NULL);
	PRECOND(len > 0);

	fp = fopen(path, "r");
	if (fp == NULL)
		fatal("fopen(%s): %s", path, errno_s);

	ret = fread(buf, 1, len - 1, fp);
	fclose(fp);

	return (ret);
}

/*
 * Run the vigil core with only our Gospel configuration.
 */
static void *
chatter_core_run(void *arg)
{
	u_int64_t	*id;

	PRECOND(arg != NULL);

	id = arg;

	(void)vigil_run(*id, -1, gospel, gospel_len, NULL, 0, datadir);

	return (NULL);
}

/*
 * Pick up the chat events, we store every message we get right away
 * unless we still have to lose some.
 */
static void
chatter_events_handle(int *got, int *delivered, int *gdone)
{
	u_int64_t	token;
	char		*p, *ep;
	char		acks[64];
	char		buf[VIGIL_CHAT_EVENTS_MAX];

	PRECOND(got != NULL);
	PRECOND(gdone != NULL);
	PRECOND(delivered != NULL);

	if (vigil_chat_events(buf, sizeof(buf)) == 0)
		return;

	if (strcmp(buf, "[]"))
		printf("chatter: %s\n", buf);

	for (p = buf; (p = strstr(p, "\"token\":")) != NULL; p++) {
		if (lose > 0) {
			printf("chatter: losing that one\n");
			lose--;
			continue;
		}

		token = strtoull(p + 8, NULL, 10);
		chatter_data_take(p);
		vigil_chat_stored(token);
		(*got)++;
	}

	for (p = buf; (p = strstr(p, "\"delivered\"")) != NULL; p++)
		(*delivered)++;

	(void)snprintf(acks, sizeof(acks), CHATTER_GROUP_ACKS, group);

	for (p = buf; (p = strstr(p, acks)) != NULL; p++) {
		ep = strchr(p, '}');
		if (ep == NULL)
			break;

		if (!strncmp(ep - strlen(CHATTER_GROUP_DONE) + 1,
		    CHATTER_GROUP_DONE, strlen(CHATTER_GROUP_DONE)))
			(*gdone)++;
	}

	fflush(stdout);
}

/*
 * Send the file at path as the given kind of data to the given peer, or
 * to our group if the peer is 0. It goes under the last part of its path.
 */
static void
chatter_data_send(u_int8_t peer, u_int64_t ref, int kind, const char *path)
{
	struct stat	st;
	FILE		*fp;
	int		ret;
	u_int8_t	*data;
	const char	*name;

	PRECOND(path != NULL);

	name = strrchr(path, '/');
	if (name != NULL)
		name++;
	else
		name = path;

	if (stat(path, &st) == -1)
		fatal("stat(%s): %s", path, errno_s);

	if (st.st_size <= 0 || st.st_size > VIGIL_CHAT_DATA_MAX)
		fatal("%s is not data we can send", path);

	data = malloc(st.st_size);
	if (data == NULL)
		fatal("malloc");

	fp = fopen(path, "r");
	if (fp == NULL)
		fatal("fopen(%s): %s", path, errno_s);

	if (fread(data, 1, st.st_size, fp) != (size_t)st.st_size)
		fatal("failed to read %s", path);

	fclose(fp);

	if (peer == 0) {
		ret = vigil_chat_group_data_send(ref, kind, name,
		    data, st.st_size);
	} else {
		ret = vigil_chat_data_send(peer, ref, kind, name,
		    data, st.st_size);
	}

	if (ret == -1)
		fatal("failed to send %s", path);

	free(data);
}

/*
 * If the event with the token at p is data, take it and write it to our
 * output directory if we have one.
 */
static void
chatter_data_take(const char *p)
{
	FILE		*fp;
	ssize_t		len;
	u_int64_t	token;
	u_int8_t	*data;
	char		path[1024];
	const char	*start, *end, *type;

	PRECOND(p != NULL);

	end = strchr(p, '}');
	if (end == NULL)
		return;

	for (start = p; *start != '{'; start--)
		;

	type = strstr(start, "\"type\":\"data\"");
	if (type == NULL)
		return;

	if (type > end)
		return;

	token = strtoull(p + 8, NULL, 10);

	data = malloc(VIGIL_CHAT_DATA_MAX);
	if (data == NULL)
		fatal("malloc");

	len = vigil_chat_data_take(token, data, VIGIL_CHAT_DATA_MAX);
	if (len == -1)
		fatal("data %" PRIu64 " is gone", token);

	printf("chatter: took data %" PRIu64 " of %zd bytes\n", token, len);

	if (outdir != NULL) {
		(void)snprintf(path, sizeof(path), "%s/%" PRIu64 ".dat",
		    outdir, token);

		fp = fopen(path, "w");
		if (fp == NULL)
			fatal("fopen(%s): %s", path, errno_s);

		if (fwrite(data, 1, len, fp) != (size_t)len)
			fatal("failed to write %s", path);

		fclose(fp);
	}

	free(data);
}
