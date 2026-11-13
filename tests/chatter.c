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
 *	$ ./tests/chatter [-n nick] [-m peer:text] [-g id:name:members]
 *	    [-j owner:id[:members]] [-G text] [-L] [-K peer]
 *	    [-i peer:file] [-I file] [-f peer:file] [-F file] [-o dir]
 *	    [-t peer] [-T] [-O peer] [-e count] [-x secs] [-l count]
 *	    [-r author:hash] [-c topic:name] [-C peer:topic] [-P file]
 *	    [-R peer:hash] [-D datadir] <gospel>
 *
 * With -m we send the text to the given peer, -m may be given more
 * than once. With -g we own the group with the given id and name, its
 * members are a comma separated list of peer ids, and send its roster to
 * them. With -j we are in the group with the given owner and id and learn
 * its members from its roster, or take them from the list given. With -G
 * we send the text to every other member of our group, -G may be given
 * more than once. With -L we leave
 * our group, or end it if we own it, once our messages to it went out.
 * With -K we remove the given peer from the group we own.
 * With -t we keep typing to the given peer and with -T to our group.
 * With -O we keep the chat with the given peer open.
 * With -r every -m and -G is a reply to the message with the given hash
 * written by the given author, 0 is us.
 * With -c we set up the given topic with every peer we send to and all of
 * our messages and data go in it, in a group only its owner sets it up.
 * With -C we remove the given topic with the given peer, -C may be given
 * more than once.
 * With -R we retract the data with the given hash we sent to the given
 * peer, in our group and topic if we have them, -R may be given more
 * than once.
 * With -i we send the image in the given file to the given peer and with
 * -I to our group, -f and -F do the same for any file. All of them may be
 * given more than once. Data we get is written to the directory given
 * with -o, avatars of our peers too.
 * With -P the image in the given file is our avatar.
 * With -e we wait until we got that many messages and with -x we give
 * up after that many seconds. With -l we act as if we failed to store
 * the first count messages we got.
 *
 * Exits 0 if all our messages were delivered, to every member for those
 * in our group, and we got all the messages we waited for, 1 otherwise.
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

/* Where the status tells us the chat session and our traffic. */
#define CHATTER_SESSION		"\"chat\":{\"session\":"
#define CHATTER_GOSPEL		"\"gospel\":{"

/* The milliseconds we stay around once we are done. */
#define CHATTER_LINGER		3000

/* The maximum number of messages we send. */
#define CHATTER_MESSAGES_MAX	16

/* The color of the topic we set up. */
#define CHATTER_TOPIC_COLOR	0x3366ff

static void	usage(void) __attribute__((noreturn));

static void	*chatter_core_run(void *);
static void	chatter_group_send(int *);
static void	chatter_avatar(const char *);
static void	chatter_data_take(const char *);
static int	chatter_avatar_take(const char *);
static void	chatter_group_event(const char *);
static void	chatter_events_handle(int *, int *);
static int	chatter_members_parse(const char *);
static void	chatter_roster_send(u_int8_t, int *);
static size_t	chatter_config_read(const char *, char *, size_t);
static void	chatter_data_send(u_int8_t, u_int64_t, int, const char *, int);

static const char	*chatter_field(const char *, const char *,
			    const char *);

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

/* Our group, its members, the messages we send to it and what we do after. */
static u_int8_t		local = 0;
static u_int8_t		owner = 0;
static u_int64_t	group = 0;
static int		group_ended = 0;
static int		group_known = 0;
static int		group_leave = 0;
static u_int8_t		group_kick = 0;
static u_int32_t	group_serial = 0;
static int		group_count = 0;
static int		group_rosters = 0;
static const char	*group_name = "";
static u_int8_t		members[VIGIL_CHAT_GROUP_ROSTER];
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

/* How many copies of messages to our group we expect to be delivered. */
static int		group_wanted = 0;

/* Who we keep a chat open with. */
static long		open_peer = 0;

/* The message our messages reply to, if any. */
static long		reply_author = 0;
static u_int64_t	reply_hash = 0;

/* The topic our messages go in and the topics we remove. */
static u_int64_t	topic = 0;
static const char	*topic_name = NULL;
static int		remove_count = 0;
static u_int8_t		remove_peer[CHATTER_MESSAGES_MAX];
static u_int64_t	remove_topic[CHATTER_MESSAGES_MAX];

/* The data we retract. */
static int		retract_count = 0;
static u_int8_t		retract_peer[CHATTER_MESSAGES_MAX];
static u_int64_t	retract_hash[CHATTER_MESSAGES_MAX];

static void
usage(void)
{
	fprintf(stderr, "usage: chatter [-n nick] [-m peer:text] "
	    "[-g id:name:members] [-j owner:id[:members]] [-G text] [-L] "
	    "[-K peer] "
	    "[-i peer:file] [-I file] [-f peer:file] [-F file] "
	    "[-a peer:file] [-A file] "
	    "[-o dir] [-t peer] [-T] [-O peer] [-e count] [-x secs] "
	    "[-l count] [-r author:hash] [-c topic:name] [-C peer:topic] "
	    "[-P file] [-R peer:hash] [-D datadir] <gospel>\n");
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
	int		got, sent, gsent, gwanted, wanted;

	quit = 60;
	got = 0;
	sent = 0;
	gsent = 0;
	expect = 0;
	session = 0;
	delivered = 0;

	while ((ch = getopt(argc, argv,
	    "a:A:c:C:D:e:f:F:g:G:i:I:j:K:l:Lm:n:o:O:P:r:R:t:Tx:")) != -1) {
		switch (ch) {
		case 'c':
			topic = strtoull(optarg, &ep, 16);
			if (*ep != ':' || topic == 0)
				usage();
			topic_name = ep + 1;
			if (strlen(topic_name) == 0 ||
			    strlen(topic_name) > VIGIL_CHAT_TOPIC_NAME_MAX)
				usage();
			break;
		case 'C':
			if (remove_count == CHATTER_MESSAGES_MAX)
				usage();
			peer = strtol(optarg, &ep, 16);
			if (*ep != ':')
				usage();
			if (peer <= 0 || peer >= KYRKA_PEERS_PER_FLOCK)
				usage();
			remove_peer[remove_count] = peer;
			remove_topic[remove_count] = strtoull(ep + 1, &ep, 16);
			if (*ep != '\0' || remove_topic[remove_count] == 0)
				usage();
			remove_count++;
			break;
		case 'P':
			chatter_avatar(optarg);
			break;
		case 'R':
			if (retract_count == CHATTER_MESSAGES_MAX)
				usage();
			peer = strtol(optarg, &ep, 16);
			if (*ep != ':')
				usage();
			if (peer <= 0 || peer >= KYRKA_PEERS_PER_FLOCK)
				usage();
			retract_peer[retract_count] = peer;
			retract_hash[retract_count] = strtoull(ep + 1, &ep, 16);
			if (*ep != '\0' || retract_hash[retract_count] == 0)
				usage();
			retract_count++;
			break;
		case 'D':
			datadir = optarg;
			break;
		case 'e':
			expect = atoi(optarg);
			break;
		case 'g':
			group = strtoull(optarg, &ep, 16);
			if (*ep != ':' || group == 0)
				usage();
			group_name = ep + 1;
			ep = strchr(group_name, ':');
			if (ep == NULL)
				usage();
			*(ep++) = '\0';
			if (strlen(group_name) > VIGIL_CHAT_GROUP_NAME_MAX)
				usage();
			if (chatter_members_parse(ep) == -1)
				usage();
			group_known = 1;
			break;
		case 'j':
			peer = strtol(optarg, &ep, 16);
			if (*ep != ':')
				usage();
			if (peer <= 0 || peer >= KYRKA_PEERS_PER_FLOCK)
				usage();
			owner = peer;
			group = strtoull(ep + 1, &ep, 16);
			if (group == 0)
				usage();
			if (*ep == ':') {
				if (chatter_members_parse(ep + 1) == -1)
					usage();
				group_known = 1;
			} else if (*ep != '\0') {
				usage();
			}
			break;
		case 'G':
			if (group_count == CHATTER_MESSAGES_MAX)
				usage();
			if (strlen(optarg) == 0 ||
			    strlen(optarg) > VIGIL_CHAT_GROUP_REPLY_MAX)
				usage();
			group_text[group_count++] = optarg;
			break;
		case 'K':
			peer = strtol(optarg, &ep, 16);
			if (*ep != '\0')
				usage();
			if (peer <= 0 || peer >= KYRKA_PEERS_PER_FLOCK)
				usage();
			group_kick = peer;
			break;
		case 'L':
			group_leave = 1;
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
		case 'r':
			reply_author = strtol(optarg, &ep, 16);
			if (*ep != ':')
				usage();
			if (reply_author < 0 ||
			    reply_author >= KYRKA_PEERS_PER_FLOCK)
				usage();
			reply_hash = strtoull(ep + 1, &ep, 16);
			if (*ep != '\0' || reply_hash == 0)
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

	if (group_leave && group == 0)
		usage();

	if (group_kick != 0 && group == 0)
		usage();

	if (group_kick != 0 && !group_known)
		usage();

	for (idx = 0; idx < message_count; idx++) {
		if (topic != 0 &&
		    strlen(message_text[idx]) > VIGIL_CHAT_TOPIC_MAX)
			usage();
	}

	for (idx = 0; idx < group_count; idx++) {
		if (topic != 0 &&
		    strlen(group_text[idx]) > VIGIL_CHAT_GROUP_TOPIC_MAX)
			usage();
	}

	wanted = message_count + data_count + remove_count + retract_count;
	gwanted = group_count + group_data_count + group_leave;

	if (group_kick != 0)
		gwanted++;

	if (topic != 0)
		wanted += message_count + data_count;

	if (typing_group && group == 0)
		usage();

	gospel_len = chatter_config_read(argv[0], gospel, sizeof(gospel));

	if (vigil_config_check(gospel, gospel_len,
	    summary, sizeof(summary)) == -1)
		fatal("%s", summary);

	if ((ep = strstr(summary, "\"spi\":\"")) == NULL)
		fatal("no spi in %s", summary);

	local = strtoul(ep + 7, NULL, 16) & 0xff;

	if (group_known && owner == 0)
		owner = local;

	if (group_known)
		members[local / 8] |= 1 << (local % 8);

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
					if (topic != 0 &&
					    vigil_chat_topic(message_peer[idx],
					    900 + idx, topic, CHATTER_TOPIC_COLOR,
					    topic_name, strlen(topic_name)) == -1)
						fatal("vigil_chat_topic");

					if (topic != 0 &&
					    vigil_chat_topic_send(message_peer[idx],
					    idx + 1, topic, reply_author, reply_hash,
					    message_text[idx],
					    strlen(message_text[idx])) == -1)
						fatal("vigil_chat_topic_send");

					if (topic != 0)
						continue;

					if (reply_hash != 0 &&
					    vigil_chat_reply(message_peer[idx],
					    idx + 1, reply_author, reply_hash,
					    message_text[idx],
					    strlen(message_text[idx])) == -1)
						fatal("vigil_chat_reply");

					if (reply_hash == 0 &&
					    vigil_chat_send(message_peer[idx],
					    idx + 1, message_text[idx],
					    strlen(message_text[idx])) == -1)
						fatal("vigil_chat_send");
				}

				for (idx = 0; idx < remove_count; idx++) {
					if (vigil_chat_topic(remove_peer[idx],
					    980 + idx, remove_topic[idx], 0,
					    NULL, 0) == -1)
						fatal("vigil_chat_topic");
				}

				for (idx = 0; idx < retract_count; idx++) {
					if (group != 0 &&
					    vigil_chat_group_retract(
					    retract_peer[idx], 960 + idx, owner,
					    group, group_serial, topic,
					    retract_hash[idx]) == -1)
						fatal("vigil_chat_group_retract");

					if (group == 0 &&
					    vigil_chat_retract(retract_peer[idx],
					    960 + idx, topic,
					    retract_hash[idx]) == -1)
						fatal("vigil_chat_retract");
				}

				for (idx = 0; idx < data_count; idx++) {
					if (topic != 0 &&
					    vigil_chat_topic(data_peer[idx],
					    950 + idx, topic, CHATTER_TOPIC_COLOR,
					    topic_name, strlen(topic_name)) == -1)
						fatal("vigil_chat_topic");

					chatter_data_send(data_peer[idx],
					    500 + idx, data_kind[idx],
					    data_path[idx], 0);
				}

				if (group_known && owner == local)
					chatter_roster_send(0, &wanted);

				if (typing_peer != 0)
					vigil_chat_typing(typing_peer,
					    topic, 1);

				if (open_peer != 0)
					vigil_chat_open(open_peer, 1);

				if (typing_group)
					vigil_chat_group_typing(owner, group,
					    topic, members);

				sent = 1;
				gsent = 0;
			}

			if (gsent == 0 && gwanted > 0 && group_known &&
			    !group_ended) {
				chatter_group_send(&wanted);
				gsent = 1;
			}
		}

		chatter_events_handle(&got, &delivered);

		if (group_rosters > 0 && owner == local) {
			wanted += group_rosters;
			group_rosters = 0;
		}

		if (sent && delivered >= wanted &&
		    (gwanted == 0 || gsent) && got >= expect)
			break;
	}

	printf("chatter: got %d/%d, delivered %d/%d, group copies %d\n",
	    got, expect, delivered, wanted, group_wanted);

	if (vigil_status(p, VIGIL_STATUS_MAX) > 0 &&
	    (ep = strstr(p, CHATTER_GOSPEL)) != NULL)
		printf("chatter: %.*s\n", (int)strcspn(ep, "}") + 1, ep);
	fflush(stdout);

	/* Give our acks a few pulses to go out. */
	(void)poll(NULL, 0, CHATTER_LINGER);

	vigil_stop(id);
	pthread_join(core, NULL);

	free(p);

	if (got >= expect && delivered >= wanted && (gwanted == 0 || gsent))
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

	if (vigil_run(*id, -1, gospel, gospel_len, NULL, 0, datadir) == -1)
		fatal("vigil_run failed");

	return (NULL);
}

/*
 * Pick up the chat events, we store every message we get right away
 * unless we still have to lose some.
 */
static void
chatter_events_handle(int *got, int *delivered)
{
	u_int64_t	token;
	char		*p;
	char		buf[VIGIL_CHAT_EVENTS_MAX];

	PRECOND(got != NULL);
	PRECOND(delivered != NULL);

	if (vigil_chat_events(buf, sizeof(buf)) == 0)
		return;

	if (strcmp(buf, "[]"))
		printf("chatter: %s\n", buf);

	for (p = buf; (p = strstr(p, "{\"peer\":")) != NULL; p++)
		chatter_group_event(p);

	for (p = buf; (p = strstr(p, "\"token\":")) != NULL; p++) {
		if (chatter_avatar_take(p) != -1)
			continue;

		if (lose > 0) {
			printf("chatter: losing that one\n");
			lose--;
			continue;
		}

		token = strtoull(p + 8, NULL, 10);
		chatter_data_take(p);

		if (vigil_chat_stored(token) == -1)
			fatal("failed to ack data %" PRIu64, token);
		(*got)++;
	}

	for (p = buf; (p = strstr(p, "\"delivered\"")) != NULL; p++)
		(*delivered)++;

	fflush(stdout);
}

/*
 * Parse a comma separated list of peer ids into our members.
 */
static int
chatter_members_parse(const char *list)
{
	long		peer;
	char		*ep;

	PRECOND(list != NULL);

	memset(members, 0, sizeof(members));

	while (*list != '\0') {
		peer = strtol(list, &ep, 16);
		if (ep == list)
			return (-1);

		if (peer <= 0 || peer >= KYRKA_PEERS_PER_FLOCK)
			return (-1);

		members[peer / 8] |= 1 << (peer % 8);

		if (*ep == ',')
			ep++;
		else if (*ep != '\0')
			return (-1);

		list = ep;
	}

	return (0);
}

/*
 * Send the roster of the group we own to every member, and to the given
 * peer too if it is not 0.
 */
static void
chatter_roster_send(u_int8_t also, int *wanted)
{
	int			idx, state;
	static u_int64_t	ref = 2000;

	PRECOND(wanted != NULL);

	if (group_ended)
		state = VIGIL_CHAT_GROUP_ENDED;
	else
		state = VIGIL_CHAT_GROUP_OPEN;

	group_serial++;

	for (idx = 1; idx < KYRKA_PEERS_PER_FLOCK; idx++) {
		if (idx == local)
			continue;

		if (idx != also && !(members[idx / 8] & (1 << (idx % 8))))
			continue;

		if (vigil_chat_group_roster(idx, ref, group, group_serial,
		    state, members, group_name, strlen(group_name)) == -1)
			fatal("vigil_chat_group_roster");

		ref++;
		(*wanted)++;
	}
}

/*
 * Send our messages and data to every other member of our group, then
 * leave or end it or remove a member if we were asked to.
 */
static void
chatter_group_send(int *wanted)
{
	int		idx, peer;

	PRECOND(wanted != NULL);

	for (peer = 1; peer < KYRKA_PEERS_PER_FLOCK; peer++) {
		if (peer == local)
			continue;

		if (!(members[peer / 8] & (1 << (peer % 8))))
			continue;

		if (topic != 0 && owner == local) {
			if (vigil_chat_group_topic(peer, 1800, group,
			    group_serial, topic, CHATTER_TOPIC_COLOR,
			    topic_name, strlen(topic_name)) == -1)
				fatal("vigil_chat_group_topic");
			(*wanted)++;
		}

		for (idx = 0; idx < group_count; idx++) {
			if (vigil_chat_group_send(peer, 1000 + idx, owner,
			    group, group_serial, topic, reply_author,
			    reply_hash, group_text[idx],
			    strlen(group_text[idx])) == -1)
				fatal("vigil_chat_group_send");
			(*wanted)++;
			group_wanted++;
		}

		for (idx = 0; idx < group_data_count; idx++) {
			chatter_data_send(peer, 1500 + idx,
			    group_data_kind[idx], group_data_path[idx], 1);
			(*wanted)++;
			group_wanted++;
		}
	}

	if (group_kick != 0) {
		members[group_kick / 8] &= ~(1 << (group_kick % 8));
		chatter_roster_send(group_kick, wanted);
	}

	if (!group_leave)
		return;

	if (owner == local) {
		group_ended = 1;
		chatter_roster_send(0, wanted);
		return;
	}

	if (vigil_chat_group_leave(owner, 1900, group) == -1)
		fatal("vigil_chat_group_leave");

	(*wanted)++;
}

/*
 * Take the roster of our group or a member leaving it from the event at p.
 */
static void
chatter_group_event(const char *p)
{
	long		peer;
	u_int64_t	id;
	char		*ep;
	const char	*end, *field;

	PRECOND(p != NULL);

	end = strchr(p, '}');
	if (end == NULL)
		return;

	field = chatter_field(p, end, "\"group\":\"");
	if (field == NULL)
		return;

	id = strtoull(field + 9, NULL, 16);
	if (id != group)
		return;

	peer = strtol(p + 9, NULL, 16);

	if (chatter_field(p, end, "\"type\":\"leave\"") != NULL) {
		if (owner != local)
			return;

		printf("chatter: %02lx left our group\n", peer);
		members[peer / 8] &= ~(1 << (peer % 8));
		chatter_roster_send(0, &group_rosters);
		return;
	}

	if (chatter_field(p, end, "\"type\":\"roster\"") == NULL)
		return;

	if (peer != owner)
		return;

	field = chatter_field(p, end, "\"members\":[");
	if (field == NULL)
		return;

	memset(members, 0, sizeof(members));
	field += strlen("\"members\":[");

	while (*field == '"') {
		peer = strtol(field + 1, &ep, 16);
		members[peer / 8] |= 1 << (peer % 8);

		field = ep + 1;
		if (*field != ',')
			break;

		field++;
	}

	group_known = 1;

	field = chatter_field(p, end, "\"serial\":");
	if (field != NULL)
		group_serial = strtoul(field + 9, NULL, 10);

	if (chatter_field(p, end, "\"ended\":true") != NULL) {
		printf("chatter: our group ended\n");
		group_ended = 1;
	}

	if (!(members[local / 8] & (1 << (local % 8)))) {
		printf("chatter: we are no longer in our group\n");
		group_ended = 1;
	}
}

/*
 * Returns where name is in the event at p that ends at end, NULL if the
 * event does not have it.
 */
static const char *
chatter_field(const char *p, const char *end, const char *name)
{
	const char	*field;

	PRECOND(p != NULL);
	PRECOND(end != NULL);
	PRECOND(name != NULL);

	field = strstr(p, name);
	if (field == NULL)
		return (NULL);

	if (field > end)
		return (NULL);

	return (field);
}

/*
 * Send the file at path as the given kind of data to the given peer, in
 * our group if ingroup is set. It goes under the last part of its path.
 */
static void
chatter_data_send(u_int8_t peer, u_int64_t ref, int kind, const char *path,
    int ingroup)
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

	if (ingroup) {
		ret = vigil_chat_group_data_send(peer, ref, owner, group,
		    group_serial, topic, kind, name, data, st.st_size);
	} else if (topic != 0) {
		ret = vigil_chat_topic_data_send(peer, ref, topic, kind, name,
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

/*
 * Make the image in the file at path our avatar.
 */
static void
chatter_avatar(const char *path)
{
	FILE		*fp;
	size_t		len;
	u_int8_t	data[VIGIL_CHAT_AVATAR_MAX + 1];

	PRECOND(path != NULL);

	fp = fopen(path, "r");
	if (fp == NULL)
		fatal("fopen(%s): %s", path, errno_s);

	len = fread(data, 1, sizeof(data), fp);
	fclose(fp);

	if (len == 0 || len > VIGIL_CHAT_AVATAR_MAX)
		fatal("%s is not an avatar we can send", path);

	vigil_chat_avatar(data, len);
}

/*
 * If the event with the token at p is an avatar, take it and write it
 * to our output directory if we have one. Returns -1 if it is not one.
 */
static int
chatter_avatar_take(const char *p)
{
	FILE		*fp;
	ssize_t		len;
	u_int64_t	token;
	char		path[1024];
	const char	*start, *end, *type;
	u_int8_t	data[VIGIL_CHAT_AVATAR_MAX];

	PRECOND(p != NULL);

	end = strchr(p, '}');
	if (end == NULL)
		return (-1);

	for (start = p; *start != '{'; start--)
		;

	type = strstr(start, "\"type\":\"avatar\"");
	if (type == NULL)
		return (-1);

	if (type > end)
		return (-1);

	token = strtoull(p + 8, NULL, 10);
	if (token == 0) {
		printf("chatter: %.2s has no avatar\n", start + 9);
		return (0);
	}

	len = vigil_chat_data_take(token, data, sizeof(data));
	if (len == -1)
		fatal("avatar %" PRIu64 " is gone", token);

	printf("chatter: took avatar of %.2s, %zd bytes\n", start + 9, len);

	if (outdir != NULL) {
		(void)snprintf(path, sizeof(path), "%s/%.2s.avatar",
		    outdir, start + 9);

		fp = fopen(path, "w");
		if (fp == NULL)
			fatal("fopen(%s): %s", path, errno_s);

		if (fwrite(data, 1, len, fp) != (size_t)len)
			fatal("failed to write %s", path);

		fclose(fp);
	}

	if (vigil_chat_stored(token) == -1)
		fatal("failed to ack avatar %" PRIu64, token);

	return (0);
}
