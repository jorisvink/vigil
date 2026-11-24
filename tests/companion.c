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
 * companion - run a companion configuration as the desktop of a phone on
 * a regular host.
 *
 *	$ ./tests/companion [-m peer:text] [-i peer:file]
 *	    [-g owner:id:serial:members:text] [-a json] [-t peer]
 *	    [-C peer] [-A] [-H secs] [-e count] [-x secs] [-o dir]
 *	    [-D datadir] <companion>
 *
 * We print what our phone tells us and store all of it. With -m our phone
 * sends the text to the given peer, with -i the image in the given file.
 * With -g it sends the text to the given members of the given group, the
 * members are a comma separated list of peer ids. With -a we ask the
 * platform of our phone to do what the given JSON says. All of them may
 * be given more than once. With -t our phone keeps typing to the given
 * peer. Data that comes along is written to the directory given with -o.
 * With -D we keep what we stored up to in the given data directory.
 *
 * With -C our phone calls the given peer for us and with -A it answers
 * the first call it rings for, with -H we hang up after talking for that
 * many seconds. The audio is a tone (see src/audio_host.c).
 *
 * With -e we wait until our phone told us about that many messages or data
 * from its peers and with -x we give up after that many seconds.
 *
 * Exits 0 if everything we sent was delivered, a call we wanted got to
 * talking and we got all we waited for, 1 otherwise.
 */

#include <sys/types.h>

#include <inttypes.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

#include "vigil.h"

/* Where the status tells us our phone is there. */
#define COMPANION_ONLINE	"\"companion\":{\"role\":\"desktop\""

/* The delivered events for what we sent, our refs all start like this. */
#define COMPANION_DELIVERED	"\"type\":\"delivered\",\"ref\":461168601842738"

/* The milliseconds we stay around once we are done. */
#define COMPANION_LINGER	3000

/* The maximum number of things we send. */
#define COMPANION_SENDS_MAX	16

/* What we send. */
#define COMPANION_TEXT		0
#define COMPANION_IMAGE		1
#define COMPANION_ASK		2

static void	usage(void) __attribute__((noreturn));

static void	*companion_core_run(void *);
static void	companion_send(int);
static int	companion_call(const char *);
static void	companion_events(int *, int *);
static void	companion_data(u_int64_t, size_t);
static int	companion_members(const char *, u_int8_t *);
static size_t	companion_read(const char *, char *, size_t);

/* The companion configuration and the flock of our phone we talk in. */
static size_t		config_len;
static u_int64_t	flock = 0;
static char		config[VIGIL_CONFIG_MAX];

/* What we send. */
static int		send_count = 0;
static int		send_what[COMPANION_SENDS_MAX];
static u_int8_t		send_peer[COMPANION_SENDS_MAX];
static const char	*send_text[COMPANION_SENDS_MAX];
static u_int8_t		send_owner[COMPANION_SENDS_MAX];
static u_int64_t	send_group[COMPANION_SENDS_MAX];
static u_int32_t	send_serial[COMPANION_SENDS_MAX];
static u_int8_t		send_members[COMPANION_SENDS_MAX]
			    [VIGIL_CHAT_GROUP_ROSTER];

/* Where the data we get goes and where we keep what we stored up to. */
static const char	*outdir = NULL;
static const char	*datadir = NULL;

/* The call we make or answer and when we hang up. */
static u_int8_t		call_peer = 0;
static int		call_answer = 0;
static int		call_hangup = 0;

static void
usage(void)
{
	fprintf(stderr, "usage: companion [-m peer:text] [-i peer:file] "
	    "[-g owner:id:serial:members:text] [-a json] [-t peer] "
	    "[-C peer] [-A] [-H secs] [-e count] [-x secs] [-o dir] "
	    "[-D datadir] -F flock <companion>\n");
	exit(1);
}

int
main(int argc, char **argv)
{
	pthread_t	core;
	char		*p, *ep;
	long		peer, typing;
	u_int64_t	id, start, talked;
	int		ch, idx, expect, quit, sent, got, delivered, wanted;

	quit = 60;
	got = 0;
	sent = 0;
	typing = 0;
	talked = 0;
	expect = 0;
	delivered = 0;

	while ((ch = getopt(argc, argv, "AC:D:F:H:a:e:g:i:m:o:t:x:")) != -1) {
		switch (ch) {
		case 'F':
			flock = strtoull(optarg, &ep, 16);
			if (*ep != '\0' || flock == 0)
				usage();
			break;
		case 'A':
			call_answer = 1;
			break;
		case 'C':
			peer = strtol(optarg, &ep, 16);
			if (*ep != '\0' || peer <= 0 ||
			    peer >= KYRKA_PEERS_PER_FLOCK)
				usage();
			call_peer = peer;
			break;
		case 'H':
			call_hangup = atoi(optarg);
			break;
		case 'a':
			if (send_count == COMPANION_SENDS_MAX)
				usage();
			if (strlen(optarg) == 0 ||
			    strlen(optarg) > VIGIL_CHAT_MESSAGE_MAX)
				usage();
			send_text[send_count] = optarg;
			send_what[send_count] = COMPANION_ASK;
			send_count++;
			break;
		case 'e':
			expect = atoi(optarg);
			break;
		case 't':
			typing = strtol(optarg, &ep, 16);
			if (*ep != '\0' || typing <= 0 ||
			    typing >= KYRKA_PEERS_PER_FLOCK)
				usage();
			break;
		case 'g':
			if (send_count == COMPANION_SENDS_MAX)
				usage();
			idx = send_count;
			peer = strtol(optarg, &ep, 16);
			if (*ep != ':' || peer <= 0 ||
			    peer >= KYRKA_PEERS_PER_FLOCK)
				usage();
			send_owner[idx] = peer;
			send_group[idx] = strtoull(ep + 1, &ep, 16);
			if (*ep != ':' || send_group[idx] == 0)
				usage();
			send_serial[idx] = strtoul(ep + 1, &ep, 10);
			if (*ep != ':')
				usage();
			p = strchr(ep + 1, ':');
			if (p == NULL)
				usage();
			*(p++) = '\0';
			if (companion_members(ep + 1, send_members[idx]) == -1)
				usage();
			if (strlen(p) == 0 ||
			    strlen(p) > VIGIL_CHAT_GROUP_MAX)
				usage();
			send_text[idx] = p;
			send_count++;
			break;
		case 'i':
		case 'm':
			if (send_count == COMPANION_SENDS_MAX)
				usage();
			peer = strtol(optarg, &ep, 16);
			if (*ep != ':' || peer <= 0 ||
			    peer >= KYRKA_PEERS_PER_FLOCK)
				usage();
			if (strlen(ep + 1) == 0 ||
			    strlen(ep + 1) > VIGIL_CHAT_MESSAGE_MAX)
				usage();
			send_peer[send_count] = peer;
			send_text[send_count] = ep + 1;
			if (ch == 'i')
				send_what[send_count] = COMPANION_IMAGE;
			send_count++;
			break;
		case 'D':
			datadir = optarg;
			break;
		case 'o':
			outdir = optarg;
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

	if (flock == 0)
		usage();

	config_len = companion_read(argv[0], config, sizeof(config));

	wanted = 0;
	for (idx = 0; idx < send_count; idx++) {
		if (send_what[idx] == COMPANION_ASK)
			continue;

		if (send_group[idx] == 0) {
			wanted++;
			continue;
		}

		for (peer = 1; peer < KYRKA_PEERS_PER_FLOCK; peer++) {
			if (send_members[idx][peer / 8] & (1 << (peer % 8)))
				wanted++;
		}
	}

	signal(SIGPIPE, SIG_IGN);

	id = vigil_run_id();

	if (pthread_create(&core, NULL, companion_core_run, &id) != 0)
		fatal("pthread_create");

	p = calloc(1, VIGIL_STATUS_MAX);
	if (p == NULL)
		fatal("calloc");

	start = vigil_time_ms();

	while ((vigil_time_ms() - start) < (u_int64_t)quit * 1000) {
		(void)poll(NULL, 0, 200);

		if (vigil_status(p, VIGIL_STATUS_MAX) == 0)
			continue;

		if (sent == 0 && (ep = strstr(p, COMPANION_ONLINE)) != NULL &&
		    strstr(ep, "\"online\":true") != NULL) {
			if (typing != 0)
				vigil_chat_typing(flock, typing, 0, 1);
			if (call_peer != 0 &&
			    vigil_chat_call(VIGIL_COMPANION_DIAL, flock,
			    call_peer) == -1)
				fatal("vigil_chat_call");
			for (idx = 0; idx < send_count; idx++)
				companion_send(idx);
			sent = 1;
		}

		companion_events(&got, &delivered);

		if (sent && (call_peer != 0 || call_answer) && talked == 0 &&
		    companion_call(p) == 1) {
			printf("companion: talking\n");
			talked = vigil_time_ms();
		}

		if (talked != 0 && call_hangup > 0 && call_peer != 0 &&
		    vigil_time_ms() - talked >= (u_int64_t)call_hangup * 1000) {
			if (vigil_chat_call(VIGIL_COMPANION_HANGUP, flock,
			    call_peer) == -1)
				fatal("vigil_chat_call");
			call_hangup = 0;
		}

		if ((call_peer != 0 || call_answer) && call_hangup > 0)
			continue;

		if ((call_peer != 0 || call_answer) && talked == 0)
			continue;

		if ((sent || send_count == 0) && delivered >= wanted &&
		    got >= expect)
			break;
	}

	printf("companion: got %d/%d, delivered %d/%d, talked %s\n",
	    got, expect, delivered, wanted, talked ? "yes" : "no");

	if (vigil_status(p, VIGIL_STATUS_MAX) > 0 &&
	    (ep = strstr(p, COMPANION_ONLINE)) != NULL)
		printf("companion: %.*s\n", (int)strcspn(ep, "}") + 1, ep);
	fflush(stdout);

	(void)poll(NULL, 0, COMPANION_LINGER);

	vigil_stop(id);
	pthread_join(core, NULL);

	free(p);

	if ((call_peer != 0 || call_answer) && talked == 0)
		return (1);

	if (got >= expect && delivered >= wanted)
		return (0);

	return (1);
}

/*
 * Read the configuration at path into buf, returns its length.
 */
static size_t
companion_read(const char *path, char *buf, size_t len)
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
 * Run the vigil core with only our companion configuration.
 */
static void *
companion_core_run(void *arg)
{
	u_int64_t	*id;

	PRECOND(arg != NULL);

	id = arg;

	if (vigil_run(*id, -1, config, config_len, NULL, 0, datadir) == -1)
		fatal("vigil_run failed");

	return (NULL);
}

/*
 * Parse a comma separated list of peer ids into the given members.
 */
static int
companion_members(const char *list, u_int8_t *members)
{
	char		*ep;
	long		peer;

	PRECOND(list != NULL);
	PRECOND(members != NULL);

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
 * Hand the given thing to send to the core, the way a desktop platform
 * does: one request, for a group with every member in it.
 */
static void
companion_send(int idx)
{
	FILE				*fp;
	size_t				len;
	u_int64_t			ref;
	struct vigil_chat_request	req;
	u_int8_t			*data;

	PRECOND(idx >= 0 && idx < send_count);

	ref = VIGIL_CHAT_COMPANION_REF | (idx + 1);

	if (send_what[idx] == COMPANION_ASK) {
		if (vigil_chat_companion_ask(ref, send_text[idx],
		    strlen(send_text[idx]), NULL, 0) == -1)
			fatal("vigil_chat_companion_ask");
		return;
	}

	memset(&req, 0, sizeof(req));

	req.ref = ref;
	req.flock = flock;
	req.op = VIGIL_COMPANION_SEND;

	if (send_group[idx] == 0) {
		req.peer = send_peer[idx];
	} else {
		req.owner = send_owner[idx];
		req.group = send_group[idx];
		req.serial = send_serial[idx];
		memcpy(req.members, send_members[idx], sizeof(req.members));
	}

	if (send_what[idx] != COMPANION_IMAGE) {
		if (vigil_chat_companion_send(&req, send_text[idx],
		    strlen(send_text[idx])) == -1)
			fatal("vigil_chat_companion_send");
		return;
	}

	if ((data = malloc(VIGIL_CHAT_DATA_MAX)) == NULL)
		fatal("malloc");

	fp = fopen(send_text[idx], "r");
	if (fp == NULL)
		fatal("fopen(%s): %s", send_text[idx], errno_s);

	len = fread(data, 1, VIGIL_CHAT_DATA_MAX, fp);
	fclose(fp);

	if (len == 0)
		fatal("%s is empty", send_text[idx]);

	req.kind = VIGIL_CHAT_DATA_IMAGE;
	req.len = strlen("image.jpg");
	memcpy(req.name, "image.jpg", req.len);

	if (vigil_chat_companion_send(&req, data, len) == -1)
		fatal("vigil_chat_companion_send");

	free(data);
}

/*
 * Answer a call our phone rings for if we want to, returns 1 once the
 * call we hold is talking.
 */
static int
companion_call(const char *status)
{
	const char	*p;
	long		peer;
	char		*ep;
	char		voice[1024];

	PRECOND(status != NULL);

	if (call_answer == 1 &&
	    (p = strstr(status, "\"incoming\":[\"")) != NULL) {
		if (strtoull(p + 13, &ep, 16) != flock || *ep != '-')
			fatal("odd incoming call");

		peer = strtol(ep + 1, NULL, 16);
		if (peer <= 0 || peer >= KYRKA_PEERS_PER_FLOCK)
			fatal("odd incoming call");

		printf("companion: answering %02lx\n", peer);
		if (vigil_chat_call(VIGIL_COMPANION_ANSWER, flock, peer) == -1)
			fatal("vigil_chat_call");

		call_peer = peer;
		call_answer = 2;
	}

	if (strstr(status, "\"relay\":true") == NULL)
		return (0);

	if (vigil_voice_status(voice, sizeof(voice)) == 0)
		return (0);

	if (strstr(voice, "\"state\":\"talking\"") == NULL)
		return (0);

	return (1);
}

/*
 * Pick up what our phone told us, print it and store it right away.
 */
static void
companion_events(int *got, int *delivered)
{
	size_t		size;
	u_int64_t	token;
	char		*p, *ep;
	char		buf[VIGIL_CHAT_EVENTS_MAX];

	PRECOND(got != NULL);
	PRECOND(delivered != NULL);

	if (vigil_chat_events(buf, sizeof(buf)) == 0)
		return;

	if (!strcmp(buf, "[]"))
		return;

	printf("companion: %s\n", buf);

	for (p = buf; (p = strstr(p, "\"type\":\"mirror\"")) != NULL; p++) {
		if ((ep = strstr(p, "\"token\":")) == NULL)
			fatal("mirror without a token");
		token = strtoull(ep + 8, NULL, 10);

		if ((ep = strstr(p, "\"size\":")) == NULL)
			fatal("mirror without a size");
		size = strtoull(ep + 7, NULL, 10);

		if (size > 0)
			companion_data(token, size);

		if (vigil_chat_stored(token) == -1)
			fatal("failed to ack %" PRIu64, token);
	}

	for (p = buf; (p = strstr(p, "\"type\":\"message\"")) != NULL; p++)
		(*got)++;

	for (p = buf; (p = strstr(p, "\"type\":\"data\"")) != NULL; p++)
		(*got)++;

	for (p = buf; (p = strstr(p, COMPANION_DELIVERED)) != NULL; p++)
		(*delivered)++;

	fflush(stdout);
}

/*
 * Take the data that came with the given token, write it out if we can.
 */
static void
companion_data(u_int64_t token, size_t size)
{
	FILE		*fp;
	ssize_t		ret;
	u_int8_t	*data;
	char		path[1024];

	if ((data = malloc(size)) == NULL)
		fatal("malloc");

	ret = vigil_chat_data_take(token, data, size);
	if (ret == -1 || (size_t)ret != size)
		fatal("failed to take %zu bytes for %" PRIu64, size, token);

	printf("companion: took %zu bytes\n", size);

	if (outdir != NULL) {
		(void)snprintf(path, sizeof(path), "%s/%" PRIu64 ".dat",
		    outdir, token);

		if ((fp = fopen(path, "w")) == NULL)
			fatal("fopen(%s): %s", path, errno_s);

		if (fwrite(data, 1, size, fp) != size)
			fatal("fwrite(%s)", path);

		fclose(fp);
	}

	free(data);
}
