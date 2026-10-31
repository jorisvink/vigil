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
 * caller - run a liturgy on a regular host and make or take a call,
 * the audio is a tone (see src/audio_host.c).
 *
 *	$ ./tests/caller [-c peer | -a | -d | -r] [-n | -s] [-t secs] [-w secs]
 *	    [-x secs] [-b secs] [-g secs] [-G secs] [-v] [-e] [-D datadir]
 *	    <config> <gospel>
 *
 * With -c we call the given peer once it is online, with -a we answer the
 * first call we get, with -d we decline it and with -r we let it ring.
 * With -t we hang up after talking for that many seconds and with -x
 * after that many seconds no matter what, otherwise we wait for the
 * other side. With -b we act as if the network changed after that many
 * seconds. With -g the tunnel starts without the Gospel configuration and
 * we attach it to the running core after that many seconds, with -G we
 * detach it again after that many seconds.
 *
 * With -v our camera is on during the call, the video is a pattern (see
 * src/video_host.c). With -e we expect video from the other side and fail
 * if we got too little of it or any of it was not what was sent.
 *
 * With - as the tunnel configuration we only run the Gospel configuration.
 *
 * Normally the voice runs inside of the vigil core. With -n we only do
 * the signaling and with -s we only run the gospel, without the core, so
 * that two of us can play the packet tunnel provider and app on iOS.
 *
 * Exits 0 if we heard the other side, or declined, and 1 otherwise.
 */

#include <sys/types.h>
#include <sys/socket.h>

#include <inttypes.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

#include "vigil.h"

/* The seconds we give everything before we give up. */
#define CALLER_TIMEOUT		120

/* The seconds we let a call ring before we answer or decline it. */
#define CALLER_PICKUP		2

/* The playback level at which we consider we hear the tone. */
#define CALLER_LEVEL_MIN	1000

/* The frames we must have decoded to consider we saw the other side. */
#define CALLER_VIDEO_MIN	30

static void	usage(void) __attribute__((noreturn));

static void	*caller_core_run(void *);
static void	*caller_standalone_run(void *);
static void	caller_voice_start(u_int8_t, int);
static size_t	caller_config_read(const char *, char *, size_t);

/* Set by src/audio_host.c and src/video_host.c. */
extern volatile u_int32_t	host_audio_level;
extern volatile u_int64_t	host_video_ok;
extern volatile u_int64_t	host_video_bad;
extern volatile u_int64_t	host_video_keys;

/* The tunnel side and our side of the socketpair. */
static int		tun[2];

/* The tunnel and Gospel configurations. */
static size_t		gospel_len;
static size_t		config_len;
static char		gospel[VIGIL_CONFIG_MAX];
static char		config[VIGIL_CONFIG_MAX];

/* Where the core keeps its remembrances, if anywhere. */
static const char	*datadir = NULL;

/* After how many seconds we attach and detach the Gospel configuration. */
static int		attach = 0;
static int		detach = 0;

/* If we only signal or only run the voice. */
static int			standalone = 0;
static int			signal_only = 0;

/* The voice session we started. */
static u_int64_t		voice_id = 0;
static u_int8_t			voice_peer = 0;
static int			voice_mode = 0;
static volatile int		voice_done = 0;

static void
usage(void)
{
	fprintf(stderr, "usage: caller [-c peer | -a | -d | -r] [-n | -s] "
	    "[-t secs] [-w secs] [-x secs] [-b secs] [-g secs] [-G secs] "
	    "[-v] [-e] [-D datadir] "
	    "<config> <gospel>\n");
	exit(1);
}

int
main(int argc, char **argv)
{
	int		rebind;
	pthread_t	core;
	u_int64_t	id, start;
	char		summary[4096], status[1024];
	u_int8_t	pkt[KYRKA_PACKET_DATA_LEN];
	int		ch, idx, answer, decline, heard, video, expect;
	int		ring, quit, dial, talk, wait, seconds, dialed, rang;

	ring = 0;
	quit = 0;
	rebind = 0;
	dial = 0;
	rang = 0;
	talk = 0;
	wait = 3;
	heard = 0;
	answer = 0;
	dialed = 0;
	decline = 0;
	video = 0;
	expect = 0;

	while ((ch = getopt(argc, argv, "ab:c:dD:eg:G:nrst:vw:x:")) != -1) {
		switch (ch) {
		case 'e':
			expect = 1;
			break;
		case 'v':
			video = 1;
			break;
		case 'g':
			attach = atoi(optarg);
			break;
		case 'G':
			detach = atoi(optarg);
			break;
		case 'b':
			rebind = atoi(optarg);
			break;
		case 'D':
			datadir = optarg;
			break;
		case 'r':
			ring = 1;
			break;
		case 'x':
			quit = atoi(optarg);
			break;
		case 'n':
			signal_only = 1;
			break;
		case 's':
			standalone = 1;
			break;
		case 'a':
			answer = 1;
			break;
		case 'c':
			dial = strtol(optarg, NULL, 16);
			if (dial <= 0 || dial >= KYRKA_PEERS_PER_FLOCK)
				usage();
			break;
		case 'd':
			decline = 1;
			break;
		case 't':
			talk = atoi(optarg);
			break;
		case 'w':
			wait = atoi(optarg);
			break;
		default:
			usage();
		}
	}

	argc -= optind;
	argv += optind;

	if (argc != 2)
		usage();

	if (dial == 0 && answer == 0 && decline == 0 && ring == 0)
		usage();

	if (signal_only && standalone)
		usage();

	if (standalone && dial == 0)
		usage();

	gospel_len = caller_config_read(argv[1], gospel, sizeof(gospel));

	if (strcmp(argv[0], "-")) {
		config_len = caller_config_read(argv[0],
		    config, sizeof(config));

		if (vigil_config_check(config, config_len,
		    summary, sizeof(summary)) == -1)
			fatal("%s", summary);
	}

	if (vigil_config_check(gospel, gospel_len,
	    summary, sizeof(summary)) == -1)
		fatal("%s", summary);

	if (socketpair(AF_UNIX, SOCK_DGRAM, 0, tun) == -1)
		fatal("socketpair: %s", errno_s);

	vigil_video_camera(video);

	signal(SIGPIPE, SIG_IGN);
	vigil_socket_nonblock(tun[1]);

	id = vigil_run_id();

	if (!standalone &&
	    pthread_create(&core, NULL, caller_core_run, &id) != 0)
		fatal("pthread_create");

	seconds = 0;
	start = vigil_time_ms();

	while (!voice_done) {
		while (read(tun[1], pkt, sizeof(pkt)) > 0)
			;

		(void)poll(NULL, 0, 1000);
		seconds++;

		if (vigil_time_ms() - start > CALLER_TIMEOUT * 1000) {
			printf("caller: timed out\n");
			break;
		}

		if (dial && !dialed && seconds >= wait && (standalone ||
		    (vigil_call_peer(dial) & VIGIL_CALL_PEER_ONLINE))) {
			printf("caller: %02x is online\n", dial);
			if (!standalone)
				vigil_call_dial(dial);
			if (!signal_only)
				caller_voice_start(dial, VIGIL_VOICE_MODE_CALL);
			dialed = 1;
		}

		if (signal_only && dialed && seconds >= wait + talk + 30) {
			printf("caller: done signaling\n");
			break;
		}

		if (attach > 0 && seconds == attach) {
			printf("caller: attaching the gospel configuration\n");
			if (vigil_gospel(gospel, gospel_len) == -1)
				fatal("vigil_gospel");
		}

		if (detach > 0 && seconds == detach) {
			printf("caller: detaching the gospel configuration\n");
			if (vigil_gospel(NULL, 0) == -1)
				fatal("vigil_gospel");
		}

		if (rebind > 0 && seconds == rebind) {
			printf("caller: the network changed\n");
			vigil_rebind();
		}

		if (quit > 0 && seconds == quit && voice_id != 0) {
			if (standalone)
				vigil_voice_stop(voice_id);
			else
				vigil_voice_hangup();
		}

		if ((answer || decline || ring) && voice_id == 0) {
			for (idx = 1; idx < KYRKA_PEERS_PER_FLOCK; idx++) {
				if ((vigil_call_peer(idx) &
				    VIGIL_CALL_PEER_SIGNALING) ==
				    VIGIL_CALL_PEER_PRESENT)
					break;
			}

			if (idx < KYRKA_PEERS_PER_FLOCK) {
				printf("caller: %02x is calling\n", idx);
				caller_voice_start(idx, VIGIL_VOICE_MODE_RING);
				rang = seconds;
			}
		}

		if (rang != 0 && !ring && seconds == rang + CALLER_PICKUP) {
			if (answer)
				vigil_voice_answer();
			else
				vigil_voice_hangup();
		}

		if (vigil_voice_status(status, sizeof(status)) > 0 &&
		    voice_id != 0 && strstr(status, "\"calls\":1") != NULL) {
			vigil_atomic_write(&voice_done, 1);
			printf("caller: %s\n", status);
		}

		if (vigil_voice_status(status, sizeof(status)) > 0 &&
		    strstr(status, "\"running\":true") != NULL) {
			printf("caller: %s level %u\n", status,
			    vigil_atomic_read(&host_audio_level));

			if (strstr(status, "\"talking\"") != NULL &&
			    vigil_atomic_read(&host_audio_level) >=
			    CALLER_LEVEL_MIN)
				heard = 1;

			if (talk > 0 && heard && --talk == 0) {
				if (standalone)
					vigil_voice_stop(voice_id);
				else
					vigil_voice_hangup();
			}
		}

		fflush(stdout);
	}

	printf("caller: call ended, heard the other side: %s\n",
	    heard ? "yes" : "no");
	printf("caller: video decoded %" PRIu64 " (%" PRIu64 " keys), "
	    "%" PRIu64 " bad\n", vigil_atomic_read(&host_video_ok),
	    vigil_atomic_read(&host_video_keys),
	    vigil_atomic_read(&host_video_bad));
	fflush(stdout);

	if (!standalone) {
		vigil_call_hangup();
		vigil_stop(id);
		pthread_join(core, NULL);
	}

	if (signal_only)
		return (0);

	if (decline) {
		if (strstr(status, "\"end\":\"local\"") == NULL)
			return (1);
		return (0);
	}

	if (!heard)
		return (1);

	if (expect) {
		if (vigil_atomic_read(&host_video_bad) != 0)
			return (1);

		if (vigil_atomic_read(&host_video_ok) < CALLER_VIDEO_MIN)
			return (1);
	}

	return (0);
}

/*
 * Read the configuration at path into buf, returns its length.
 */
static size_t
caller_config_read(const char *path, char *buf, size_t len)
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
 * Run the vigil core on our socketpair.
 */
static void *
caller_core_run(void *arg)
{
	int		ret;
	u_int64_t	*id;

	PRECOND(arg != NULL);

	id = arg;

	if (config_len == 0) {
		ret = vigil_run(*id, -1, gospel, gospel_len, NULL, 0, datadir);
	} else if (attach > 0) {
		ret = vigil_run(*id, tun[0], config, config_len,
		    NULL, 0, datadir);
	} else {
		ret = vigil_run(*id, tun[0], config, config_len,
		    gospel, gospel_len, datadir);
	}

	if (ret == -1)
		fatal("vigil_run failed");

	return (NULL);
}

/*
 * Start a voice session with the given peer, in the vigil core or on
 * its own thread if we run without the core.
 */
static void
caller_voice_start(u_int8_t peer, int mode)
{
	pthread_t	thread;

	PRECOND(peer > 0 && peer < KYRKA_PEERS_PER_FLOCK);
	PRECOND(mode == VIGIL_VOICE_MODE_CALL ||
	    mode == VIGIL_VOICE_MODE_RING);

	voice_peer = peer;
	voice_mode = mode;
	voice_id = vigil_voice_run_id();

	if (!standalone) {
		vigil_voice_call(peer, mode);
		return;
	}

	if (pthread_create(&thread, NULL, caller_standalone_run, NULL) != 0)
		fatal("pthread_create");

	pthread_detach(thread);
}

/*
 * Run the voice session without the vigil core.
 */
static void *
caller_standalone_run(void *arg)
{
	PRECOND(arg == NULL);

	(void)vigil_voice_run(voice_id,
	    gospel, gospel_len, voice_peer, voice_mode, NULL);

	return (NULL);
}
