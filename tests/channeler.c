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
 * channeler - join a voice channel on a regular host, talk with a tone
 * (see src/audio_host.c) and check which tones we hear.
 *
 *	$ ./tests/channeler [-s] [-H hz] [-t start:secs] [-m start:secs]
 *	    [-l start:secs] [-c peer:start:secs] [-e hz]... [-n peers]
 *	    [-R peer,...] [-x secs] [-D datadir] <gospel> <owner:group>
 *
 * We turn on the voice of the group with the given owner and id (hex),
 * everyone is in its roster unless -R lists who is.
 *
 * With -H we talk with a tone of that frequency, -t talks from that
 * second on for that many seconds. With -m we mute the channel for that
 * long and with -l we leave it for that long and join again. With -c we
 * call the given peer for that long, the channel must stay quiet then.
 *
 * We log when we start talking and when we first heard each tone, to
 * measure how long it takes for our first words to reach the others.
 *
 * Normally the channel runs inside of the vigil core. With -s it runs on
 * its own thread without the core, like the iOS app runs it.
 *
 * With -e we expect to hear a tone of that frequency, given more than
 * once we expect to hear all of them in the same second. With -n we
 * expect that many peers online at some point.
 *
 * Exits 0 if we heard what we expected, heard nothing while muted and
 * saw enough peers, and 1 otherwise.
 */

#include <sys/types.h>

#include <inttypes.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

#include "vigil.h"

/* The most tones we look for. */
#define CHANNELER_TONES		4

/* The level at which we consider we hear a tone. */
#define CHANNELER_LEVEL_MIN	1000

static void	usage(void) __attribute__((noreturn));

static void	*channeler_core_run(void *);
static void	*channeler_standalone_run(void *);
static void	channeler_window(int *, int *);
static void	channeler_tap(const int16_t *, size_t);
static void	channeler_roster(char *, u_int8_t *);
static void	channeler_span(const char *, int *, int *);
static size_t	channeler_config_read(const char *, char *, size_t);

/* Set by src/audio_host.c. */
extern volatile u_int32_t	host_audio_hz;
extern void			(*host_audio_tap)(const int16_t *, size_t);

/* The status of our channel, too large for the stack. */
static char			status[VIGIL_CHANNEL_STATUS_MAX];

/* Our Gospel configuration. */
static size_t			gospel_len;
static char			gospel[VIGIL_CONFIG_MAX];

/* Where the core keeps its remembrances. */
static const char		*datadir = NULL;

/* The id of the run without the core. */
static u_int64_t		alone_id = 0;

/* The tones we look for and the loudest we heard each in this second. */
static int			tones = 0;
static u_int32_t		hz[CHANNELER_TONES];
static volatile u_int32_t	levels[CHANNELER_TONES];
static volatile u_int64_t	first[CHANNELER_TONES];

static void
usage(void)
{
	fprintf(stderr, "usage: channeler [-s] [-H hz] [-t start:secs] "
	    "[-m start:secs] [-l start:secs] [-c peer:start:secs] [-e hz]... "
	    "[-n peers] [-R peer,...] [-x secs] [-D datadir] "
	    "<gospel> <owner:group>\n");
	exit(1);
}

int
main(int argc, char **argv)
{
	char		*ep;
	pthread_t	core;
	long		owner;
	u_int64_t	group;
	u_int64_t	id, flock;
	int		standalone;
	char		summary[4096];
	int		call_at, call_for, call_peer;
	u_int8_t	roster[VIGIL_CHAT_GROUP_ROSTER];
	int		ch, idx, seconds, quit, peers, online, most;
	int		leave_at, leave_for, loud, wrong, muted, gone;
	int		talk_at, talk_for, mute_at, mute_for, all, heard;

	quit = 30;
	peers = 0;
	most = 0;
	heard = 0;
	wrong = 0;
	talk_at = 0;
	talk_for = 0;
	mute_at = 0;
	mute_for = 0;
	leave_at = 0;
	leave_for = 0;
	call_at = 0;
	call_for = 0;
	call_peer = 0;
	standalone = 0;
	memset(roster, 0xff, sizeof(roster));

	while ((ch = getopt(argc, argv, "c:D:e:H:l:m:n:R:st:x:")) != -1) {
		switch (ch) {
		case 'c':
			call_peer = strtol(optarg, &ep, 16);
			if (*ep != ':' || call_peer <= 0 ||
			    call_peer >= KYRKA_PEERS_PER_FLOCK)
				usage();
			channeler_span(ep + 1, &call_at, &call_for);
			break;
		case 'D':
			datadir = optarg;
			break;
		case 'e':
			if (tones == CHANNELER_TONES)
				usage();
			hz[tones++] = atoi(optarg);
			break;
		case 'H':
			vigil_atomic_write(&host_audio_hz, atoi(optarg));
			break;
		case 'l':
			channeler_span(optarg, &leave_at, &leave_for);
			break;
		case 'm':
			channeler_span(optarg, &mute_at, &mute_for);
			break;
		case 'n':
			peers = atoi(optarg);
			break;
		case 'R':
			channeler_roster(optarg, roster);
			break;
		case 's':
			standalone = 1;
			break;
		case 't':
			channeler_span(optarg, &talk_at, &talk_for);
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

	if (argc != 2)
		usage();

	owner = strtol(argv[1], &ep, 16);
	if (*ep != ':' || owner <= 0 || owner >= KYRKA_PEERS_PER_FLOCK)
		usage();

	group = strtoull(ep + 1, &ep, 16);
	if (*ep != '\0' || group == 0)
		usage();

	if (standalone && (leave_for > 0 || call_for > 0))
		usage();

	gospel_len = channeler_config_read(argv[0], gospel, sizeof(gospel));

	if (vigil_config_check(gospel, gospel_len,
	    summary, sizeof(summary)) == -1)
		fatal("%s", summary);

	if ((ep = strstr(summary, "\"cathedral_flock\":\"")) == NULL)
		fatal("no flock in %s", summary);

	flock = strtoull(ep + 19, NULL, 16);

	signal(SIGPIPE, SIG_IGN);
	host_audio_tap = channeler_tap;

	id = vigil_run_id();

	if (standalone) {
		alone_id = vigil_channel_run_id();
		vigil_channel_join(flock, owner, group, roster);

		if (pthread_create(&core, NULL,
		    channeler_standalone_run, NULL) != 0)
			fatal("pthread_create");
	} else {
		if (pthread_create(&core, NULL, channeler_core_run, &id) != 0)
			fatal("pthread_create");

		vigil_channel_join(flock, owner, group, roster);
	}

	for (seconds = 1; seconds <= quit; seconds++) {
		(void)poll(NULL, 0, 1000);

		if (talk_for > 0 && seconds == talk_at) {
			printf("channeler: talking at %" PRIu64 "\n",
			    vigil_time_ms());
			vigil_channel_talk(1);
		}

		if (talk_for > 0 && seconds == talk_at + talk_for)
			vigil_channel_talk(0);

		if (mute_for > 0 && seconds == mute_at)
			vigil_channel_mute(1);

		if (mute_for > 0 && seconds == mute_at + mute_for)
			vigil_channel_mute(0);

		if (leave_for > 0 && seconds == leave_at)
			vigil_channel_leave();

		if (leave_for > 0 && seconds == leave_at + leave_for)
			vigil_channel_join(flock, owner, group, roster);

		if (call_for > 0 && seconds == call_at) {
			vigil_call_dial(flock, call_peer);
			vigil_voice_call(flock, call_peer,
			    VIGIL_VOICE_MODE_CALL);
		}

		if (call_for > 0 && seconds == call_at + call_for) {
			vigil_call_hangup();
			vigil_voice_hangup();
		}

		if (mute_for > 0 && seconds > mute_at &&
		    seconds <= mute_at + mute_for)
			muted = 1;
		else
			muted = 0;

		if (leave_for > 0 && seconds > leave_at &&
		    seconds <= leave_at + leave_for)
			gone = 1;
		else if (call_for > 0 && seconds > call_at &&
		    seconds <= call_at + call_for)
			gone = 1;
		else
			gone = 0;

		channeler_window(&loud, &all);

		if ((muted || gone) && loud) {
			printf("channeler: heard a tone while %s\n",
			    muted ? "muted" : "away");
			wrong = 1;
		}

		if (!muted && !gone && all)
			heard = 1;

		if (vigil_channel_status(status, sizeof(status)) == 0)
			fatal("channel status does not fit");

		online = 0;
		ep = status;

		while ((ep = strstr(ep, "\"online\":true")) != NULL) {
			online++;
			ep++;
		}

		if (online > most)
			most = online;

		printf("channeler: %d %s", seconds, status);
		for (idx = 0; idx < tones; idx++)
			printf(" %u=%u", hz[idx], levels[idx]);
		printf("\n");

		for (idx = 0; idx < tones; idx++)
			vigil_atomic_write(&levels[idx], 0);

		fflush(stdout);
	}

	if (standalone) {
		vigil_channel_stop(alone_id);
	} else {
		vigil_channel_leave();
		(void)poll(NULL, 0, 500);
		vigil_stop(id);
	}

	pthread_join(core, NULL);

	for (idx = 0; idx < tones; idx++) {
		printf("channeler: first heard %u at %" PRIu64 "\n",
		    hz[idx], vigil_atomic_read(&first[idx]));
	}

	printf("channeler: heard %s, most peers online %d, %s\n",
	    heard ? "all tones" : "not all tones", most,
	    wrong ? "heard something we should not have" : "nothing wrong");

	if (wrong)
		return (1);

	if (tones > 0 && !heard)
		return (1);

	if (most < peers)
		return (1);

	return (0);
}

/*
 * Parse a list of peers into the given roster.
 */
static void
channeler_roster(char *arg, u_int8_t *roster)
{
	char		*ep;
	long		peer;

	PRECOND(arg != NULL);
	PRECOND(roster != NULL);

	memset(roster, 0, VIGIL_CHAT_GROUP_ROSTER);

	for (;;) {
		peer = strtol(arg, &ep, 16);
		if (ep == arg || peer <= 0 || peer >= KYRKA_PEERS_PER_FLOCK)
			usage();

		roster[peer / 8] |= 1 << (peer % 8);

		if (*ep == '\0')
			break;

		if (*ep != ',')
			usage();

		arg = ep + 1;
	}
}

/*
 * Parse a start:secs span.
 */
static void
channeler_span(const char *arg, int *start, int *secs)
{
	char		*ep;

	PRECOND(arg != NULL);
	PRECOND(start != NULL);
	PRECOND(secs != NULL);

	*start = strtol(arg, &ep, 10);
	if (*ep != ':' || *start <= 0)
		usage();

	*secs = strtol(ep + 1, &ep, 10);
	if (*ep != '\0' || *secs <= 0)
		usage();
}

/*
 * Tell if we heard any tone and if we heard all of them this second.
 */
static void
channeler_window(int *loud, int *all)
{
	int		idx;

	PRECOND(loud != NULL);
	PRECOND(all != NULL);

	*loud = 0;
	*all = tones > 0;

	for (idx = 0; idx < tones; idx++) {
		if (vigil_atomic_read(&levels[idx]) >= CHANNELER_LEVEL_MIN)
			*loud = 1;
		else
			*all = 0;
	}
}

/*
 * Called with every frame we play, a goertzel filter gives us the
 * level of each tone we look for.
 */
static void
channeler_tap(const int16_t *pcm, size_t samples)
{
	size_t		idx;
	int		tone;
	u_int32_t	level;
	double		coeff, s0, s1, s2, power;

	PRECOND(pcm != NULL);
	PRECOND(samples > 0);

	for (tone = 0; tone < tones; tone++) {
		coeff = 2 * cos(2 * M_PI * hz[tone] / VIGIL_VOICE_SAMPLE_RATE);

		s1 = 0;
		s2 = 0;

		for (idx = 0; idx < samples; idx++) {
			s0 = pcm[idx] + coeff * s1 - s2;
			s2 = s1;
			s1 = s0;
		}

		power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
		level = 2 * sqrt(power) / samples;

		if (level > vigil_atomic_read(&levels[tone]))
			vigil_atomic_write(&levels[tone], level);

		if (level >= CHANNELER_LEVEL_MIN &&
		    vigil_atomic_read(&first[tone]) == 0)
			vigil_atomic_write(&first[tone], vigil_time_ms());
	}
}

/*
 * Read the configuration at path into buf, returns its length.
 */
static size_t
channeler_config_read(const char *path, char *buf, size_t len)
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
channeler_core_run(void *arg)
{
	u_int64_t	*id;

	PRECOND(arg != NULL);

	id = arg;

	if (vigil_run(*id, -1, gospel, gospel_len, NULL, 0, datadir) == -1)
		fatal("vigil_run failed");

	return (NULL);
}

/*
 * Run our channel without the vigil core.
 */
static void *
channeler_standalone_run(void *arg)
{
	PRECOND(arg == NULL);

	if (vigil_channel_run(alone_id, gospel, gospel_len, NULL) == -1)
		fatal("vigil_channel_run failed");

	return (NULL);
}
