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
#include <sys/socket.h>

#include <arpa/inet.h>
#include <netinet/in.h>

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vigil.h"

#include <libkyrka/libnyfe.h>

/* The label for the tunnel secret. */
#define SAFETY_KDF_LABEL	"VIGIL.SAFETY.KDF.SECRET"

/* The label for the code that both peers compare. */
#define SAFETY_CODE_LABEL	"VIGIL.SAFETY.CODE"

/* The milliseconds we give the other side to type our words. */
#define SAFETY_TIMEOUT		(120 * 1000)

/* The milliseconds between sends of our share until the share is acked. */
#define SAFETY_RESEND		1000

/* The number of acks we send at once, we close our tunnel after. */
#define SAFETY_ACK_BURST	5

/* The milliseconds between key management and cathedral notifies. */
#define SAFETY_MANAGE_INTERVAL	1000
#define SAFETY_NOTIFY_INTERVAL	5000

/* The states of an exchange. */
#define SAFETY_STATE_NONE	0
#define SAFETY_STATE_CONNECTING	1
#define SAFETY_STATE_DONE	2
#define SAFETY_STATE_FAILED	3

/* What the platform asked for. */
#define SAFETY_REQUEST_NONE	0
#define SAFETY_REQUEST_START	1
#define SAFETY_REQUEST_STOP	2

/* The messages over the tunnel. */
#define SAFETY_MSG_SHARE	1
#define SAFETY_MSG_ACK		2

/* The length of the share of each side. */
#define SAFETY_SHARE_LEN	(VIGIL_ADDITIONAL_INPUT_LEN / 2)

struct safety_msg {
	u_int8_t	type;
	u_int8_t	data[SAFETY_SHARE_LEN];
} __attribute__((packed));

/*
 * The state of the exchange for the platform, under safety_lock.
 */
struct safety_shared {
	int		state;
	int		fresh;
	int		request;
	u_int8_t	peer;
	u_int8_t	own[VIGIL_SAFETY_HALF_LEN];
	u_int8_t	theirs[VIGIL_SAFETY_HALF_LEN];
	u_int8_t	input[VIGIL_ADDITIONAL_INPUT_LEN];
};

/*
 * The tunnel to the other side, only touched from the chat update.
 */
struct safety_tunnel {
	struct vigil_io		io;
	KYRKA			*ctx;
	u_int8_t		peer;
	int			got;
	int			done;
	int			first;
	int			acked;
	int			ack_due;
	u_int64_t		online;
	u_int64_t		expire;
	u_int64_t		next_send;
	u_int64_t		next_manage;
	u_int64_t		next_notify;
	struct vigil_cathedral	cathedral;
	u_int8_t		own[SAFETY_SHARE_LEN];
	u_int8_t		theirs[SAFETY_SHARE_LEN];
};

static void	safety_ack(void);
static int	safety_send(int);
static void	safety_done(void);
static void	safety_check(void);
static void	safety_read(void *);
static void	safety_state(u_int8_t, int);
static void	safety_output(struct kyrka_packet *);
static void	safety_secret(u_int8_t, const u_int8_t *, const u_int8_t *,
		    u_int8_t *, size_t);

static void	safety_tunnel_free(void);
static void	safety_tunnel_manage(void);
static void	safety_cathedral_check(void);
static void	safety_tunnel_new(u_int8_t, const u_int8_t *,
		    const u_int8_t *);

static void	safety_event(KYRKA *, union kyrka_event *, void *);
static void	safety_heaven_input(struct kyrka_packet *, u_int64_t, void *);
static void	safety_purgatory_input(struct kyrka_packet *, u_int64_t,
		    void *);
static void	safety_cathedral_send(struct kyrka_packet *, u_int64_t,
		    void *);

static struct safety_shared	shared;
static struct safety_tunnel	*tunnel = NULL;
static pthread_mutex_t		safety_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * Write the words for the given half into out, separated by spaces.
 * The words are a number in base VIGIL_WORD_COUNT, most significant
 * word first.
 */
int
vigil_safety_words(const u_int8_t *half, size_t len, char *out,
    size_t outlen)
{
	int			idx;
	struct vigil_buf	buf;
	u_int64_t		val;
	u_int16_t		words[VIGIL_SAFETY_WORDS];

	PRECOND(half != NULL);
	PRECOND(len == VIGIL_SAFETY_HALF_LEN);
	PRECOND(out != NULL);
	PRECOND(outlen > 0);

	memcpy(&val, half, sizeof(val));
	val = be64toh(val);

	for (idx = VIGIL_SAFETY_WORDS - 1; idx >= 0; idx--) {
		words[idx] = val % VIGIL_WORD_COUNT;
		val /= VIGIL_WORD_COUNT;
	}

	vigil_buf_init(&buf, out, outlen);

	for (idx = 0; idx < VIGIL_SAFETY_WORDS; idx++) {
		vigil_buf_appendf(&buf, "%s%s", idx == 0 ? "" : " ",
		    vigil_remote_word(words[idx]));
	}

	nyfe_mem_zero(words, sizeof(words));
	nyfe_mem_zero(&val, sizeof(val));

	if (buf.flags & VIGIL_BUF_FULL) {
		nyfe_mem_zero(out, outlen);
		return (-1);
	}

	return (0);
}

/*
 * Read a half from the given words, which are separated by spaces.
 * Returns -1 if the words are not right.
 */
int
vigil_safety_half(const char *words, u_int8_t *half, size_t len)
{
	int		idx, ret;
	u_int64_t	val;
	u_int16_t	all[VIGIL_SAFETY_WORDS];

	PRECOND(words != NULL);
	PRECOND(half != NULL);
	PRECOND(len == VIGIL_SAFETY_HALF_LEN);

	if (vigil_remote_words_parse(words, all, VIGIL_SAFETY_WORDS) == -1) {
		nyfe_mem_zero(all, sizeof(all));
		return (-1);
	}

	ret = 0;
	val = 0;

	for (idx = 0; idx < VIGIL_SAFETY_WORDS; idx++) {
		if (val > (UINT64_MAX - all[idx]) / VIGIL_WORD_COUNT) {
			ret = -1;
			break;
		}

		val = val * VIGIL_WORD_COUNT + all[idx];
	}

	if (ret == 0) {
		val = htobe64(val);
		memcpy(half, &val, sizeof(val));
	}

	nyfe_mem_zero(all, sizeof(all));
	nyfe_mem_zero(&val, sizeof(val));

	return (ret);
}

/*
 * Write the code for the given additional input into out as 16 hex
 * digits in groups of 4. The code is the first 64 bits of a KMAC256
 * over the input with an all zero key, both peers show the same code
 * for the same input.
 */
int
vigil_safety_code(const u_int8_t *input, size_t len, char *out, size_t outlen)
{
	struct nyfe_kmac256	kdf;
	struct vigil_buf	buf;
	int			idx;
	u_int8_t		key[VIGIL_KEY_LENGTH];
	u_int8_t		code[VIGIL_SAFETY_HALF_LEN];

	PRECOND(input != NULL);
	PRECOND(len == VIGIL_ADDITIONAL_INPUT_LEN);
	PRECOND(out != NULL);
	PRECOND(outlen > 0);

	nyfe_zeroize_register(&kdf, sizeof(kdf));
	nyfe_mem_zero(key, sizeof(key));

	nyfe_kmac256_init(&kdf, key, sizeof(key),
	    SAFETY_CODE_LABEL, sizeof(SAFETY_CODE_LABEL) - 1);
	nyfe_kmac256_update(&kdf, input, len);
	nyfe_kmac256_final(&kdf, code, sizeof(code));
	nyfe_zeroize(&kdf, sizeof(kdf));

	vigil_buf_init(&buf, out, outlen);

	for (idx = 0; idx < (int)sizeof(code); idx++) {
		vigil_buf_appendf(&buf, "%s%02x",
		    idx > 0 && idx % 2 == 0 ? " " : "", code[idx]);
	}

	nyfe_mem_zero(code, sizeof(code));

	if (buf.flags & VIGIL_BUF_FULL) {
		nyfe_mem_zero(out, outlen);
		return (-1);
	}

	return (0);
}

/*
 * Start an exchange with the given peer from our half and theirs, any
 * exchange that is going on is stopped. Returns -1 if both are the same.
 */
int
vigil_safety_start(u_int8_t peer, const u_int8_t *own, const u_int8_t *theirs,
    size_t len)
{
	PRECOND(peer > 0 && peer < KYRKA_PEERS_PER_FLOCK);
	PRECOND(own != NULL);
	PRECOND(theirs != NULL);
	PRECOND(len == VIGIL_SAFETY_HALF_LEN);

	if (nyfe_mem_cmp(own, theirs, len) == 0)
		return (-1);

	vigil_lock(&safety_lock);

	nyfe_mem_zero(&shared, sizeof(shared));

	shared.peer = peer;
	shared.state = SAFETY_STATE_CONNECTING;
	shared.request = SAFETY_REQUEST_START;

	memcpy(shared.own, own, len);
	memcpy(shared.theirs, theirs, len);

	vigil_unlock(&safety_lock);

	return (0);
}

/*
 * Copy the input the exchange with the given peer gave us into out, once.
 * Returns -1 if there is none.
 */
int
vigil_safety_get(u_int8_t peer, u_int8_t *out, size_t len)
{
	int		ret;

	PRECOND(out != NULL);
	PRECOND(len == VIGIL_ADDITIONAL_INPUT_LEN);

	ret = -1;
	vigil_lock(&safety_lock);

	if (shared.state == SAFETY_STATE_DONE &&
	    shared.peer == peer && shared.fresh) {
		memcpy(out, shared.input, len);
		nyfe_mem_zero(shared.input, sizeof(shared.input));
		shared.fresh = 0;
		ret = 0;
	}

	vigil_unlock(&safety_lock);

	return (ret);
}

/*
 * Stop the exchange and wipe all state of the exchange.
 */
void
vigil_safety_stop(void)
{
	vigil_lock(&safety_lock);
	nyfe_mem_zero(&shared, sizeof(shared));
	shared.request = SAFETY_REQUEST_STOP;
	vigil_unlock(&safety_lock);
}

/*
 * Run what the platform asked for and manage our tunnel, called from
 * the chat update.
 */
void
vigil_safety_update(void)
{
	int		request;
	u_int8_t	peer;
	u_int8_t	own[VIGIL_SAFETY_HALF_LEN];
	u_int8_t	theirs[VIGIL_SAFETY_HALF_LEN];

	PRECOND(vigil->gospel != NULL);

	vigil_lock(&safety_lock);

	peer = shared.peer;
	request = shared.request;
	shared.request = SAFETY_REQUEST_NONE;

	if (request == SAFETY_REQUEST_START) {
		memcpy(own, shared.own, sizeof(own));
		memcpy(theirs, shared.theirs, sizeof(theirs));
		nyfe_mem_zero(shared.own, sizeof(shared.own));
		nyfe_mem_zero(shared.theirs, sizeof(shared.theirs));
	}

	vigil_unlock(&safety_lock);

	if (request != SAFETY_REQUEST_NONE)
		safety_tunnel_free();

	if (request == SAFETY_REQUEST_START) {
		safety_tunnel_new(peer, own, theirs);
		nyfe_mem_zero(own, sizeof(own));
		nyfe_mem_zero(theirs, sizeof(theirs));
	}

	if (tunnel != NULL)
		safety_tunnel_manage();
}

/*
 * Give our tunnel a new socket, the network changed.
 */
void
vigil_safety_rebind(void)
{
	int		fd;

	if (tunnel == NULL)
		return;

	fd = vigil_socket_udp();
	if (fd == -1)
		return;

	vigil_io_unschedule(&tunnel->io);
	vigil_socket_close(tunnel->io.fd);

	tunnel->io.fd = fd;
	tunnel->next_notify = 0;

	vigil_io_schedule(&tunnel->io);
}

/*
 * The chats stop, so does an exchange that is going on.
 */
void
vigil_safety_cleanup(void)
{
	u_int8_t	peer;

	if (tunnel == NULL)
		return;

	peer = tunnel->peer;
	safety_tunnel_free();

	vigil_lock(&safety_lock);

	if (shared.peer == peer && shared.state == SAFETY_STATE_CONNECTING)
		shared.state = SAFETY_STATE_FAILED;

	vigil_unlock(&safety_lock);
}

/*
 * Wipe all we hold, the caller holds the kyrka lock for good.
 */
void
vigil_safety_erase(void)
{
	nyfe_mem_zero(&shared, sizeof(shared));

	if (tunnel != NULL) {
		nyfe_mem_zero(tunnel->own, sizeof(tunnel->own));
		nyfe_mem_zero(tunnel->theirs, sizeof(tunnel->theirs));
	}
}

/*
 * Add the state of the exchange to the given status, if there is one.
 */
void
vigil_safety_status(struct vigil_buf *buf)
{
	int		state;
	u_int8_t	peer;
	const char	*name;

	PRECOND(buf != NULL);

	vigil_lock(&safety_lock);
	peer = shared.peer;
	state = shared.state;
	vigil_unlock(&safety_lock);

	switch (state) {
	case SAFETY_STATE_CONNECTING:
		name = "connecting";
		break;
	case SAFETY_STATE_DONE:
		name = "done";
		break;
	case SAFETY_STATE_FAILED:
		name = "failed";
		break;
	default:
		return;
	}

	vigil_buf_appendf(buf,
	    ",\"safety\":{\"peer\":\"%02x\",\"state\":\"%s\"}", peer, name);
}

/*
 * Set up the tunnel to the given peer from both halves.
 */
static void
safety_tunnel_new(u_int8_t peer, const u_int8_t *own, const u_int8_t *theirs)
{
	struct kyrka_cathedral_cfg	cfg;
	u_int8_t			local;
	struct vigil_config		*gospel;
	u_int8_t			secret[VIGIL_KEY_LENGTH];

	PRECOND(peer > 0 && peer < KYRKA_PEERS_PER_FLOCK);
	PRECOND(own != NULL);
	PRECOND(theirs != NULL);
	PRECOND(tunnel == NULL);

	gospel = vigil->gospel;
	local = gospel->spi & 0xff;

	if (peer == local) {
		safety_state(peer, SAFETY_STATE_FAILED);
		return;
	}

	if ((tunnel = calloc(1, sizeof(*tunnel))) == NULL)
		fatal("calloc: failed to allocate safety tunnel");

	tunnel->io.fd = -1;
	tunnel->peer = peer;
	tunnel->first = local < peer;
	tunnel->io.handle = safety_read;
	tunnel->expire = vigil->now + SAFETY_TIMEOUT;

	if (vigil_call_cathedral_pick(&tunnel->cathedral) == -1) {
		vigil_log(LOG_NOTICE, "[safety] %02x no cathedral", peer);
		safety_tunnel_free();
		safety_state(peer, SAFETY_STATE_FAILED);
		return;
	}

	tunnel->io.fd = vigil_socket_udp();
	if (tunnel->io.fd == -1) {
		safety_tunnel_free();
		safety_state(peer, SAFETY_STATE_FAILED);
		return;
	}

	nyfe_random_bytes(tunnel->own, sizeof(tunnel->own));

	tunnel->ctx = kyrka_ctx_alloc(safety_event, tunnel);
	if (tunnel->ctx == NULL)
		fatal("failed to create safety context");

	nyfe_zeroize_register(secret, sizeof(secret));
	safety_secret(peer, own, theirs, secret, sizeof(secret));

	if (kyrka_secret_load(tunnel->ctx, secret, sizeof(secret)) == -1)
		fatal("kyrka_secret_load: %d", kyrka_last_error(tunnel->ctx));

	nyfe_zeroize(secret, sizeof(secret));

	memset(&cfg, 0, sizeof(cfg));

	cfg.udata = tunnel;
	cfg.send = safety_cathedral_send;
	cfg.identity = gospel->cathedral_id;
	cfg.tunnel = (local << 8) | peer;
	cfg.flock_src = gospel->cathedral_flock | VIGIL_SAFETY_FLOCK_DOMAIN;
	cfg.flock_dst = cfg.flock_src;

	if (gospel->flags & VIGIL_CONFIG_FLAG_COMMIXTION)
		cfg.hops = VIGIL_COMMIXTION_HOPS;

	vigil_kyrka_cathedral(tunnel->ctx, &cfg, gospel, 0, gospel->mtu);

	if (kyrka_heaven_ifc(tunnel->ctx, safety_heaven_input, tunnel) == -1)
		fatal("failed to set heaven interface");

	if (kyrka_purgatory_ifc(tunnel->ctx,
	    safety_purgatory_input, tunnel) == -1)
		fatal("failed to set purgatory interface");

	vigil_io_schedule(&tunnel->io);

	vigil_log(LOG_INFO, "[safety] %02x tunnel via %s", peer,
	    vigil_address(&tunnel->cathedral.addr));
}

/*
 * Tear down our tunnel, if we have one.
 */
static void
safety_tunnel_free(void)
{
	if (tunnel == NULL)
		return;

	if (tunnel->io.fd != -1) {
		vigil_io_unschedule(&tunnel->io);
		vigil_socket_close(tunnel->io.fd);
	}

	if (tunnel->ctx != NULL)
		kyrka_ctx_free(tunnel->ctx);

	vigil_log(LOG_INFO, "[safety] %02x tunnel closed", tunnel->peer);

	nyfe_mem_zero(tunnel, sizeof(*tunnel));
	free(tunnel);

	tunnel = NULL;
}

/*
 * Manage keys, keep the cathedral informed and send our share until the
 * share is acked. Once we expire we are done or the exchange failed.
 */
static void
safety_tunnel_manage(void)
{
	u_int8_t	peer;

	PRECOND(tunnel != NULL);

	if (vigil->now >= tunnel->expire) {
		peer = tunnel->peer;

		if (tunnel->done == 0) {
			vigil_log(LOG_INFO, "[safety] %02x no tunnel came up "
			    "in time, the words may not match", peer);
		}

		safety_tunnel_free();

		vigil_lock(&safety_lock);

		if (shared.peer == peer &&
		    shared.state == SAFETY_STATE_CONNECTING)
			shared.state = SAFETY_STATE_FAILED;

		vigil_unlock(&safety_lock);
		return;
	}

	safety_cathedral_check();

	if (vigil->now >= tunnel->next_notify) {
		tunnel->next_notify = vigil->now + SAFETY_NOTIFY_INTERVAL;

		if (kyrka_cathedral_notify(tunnel->ctx) == -1) {
			vigil_log(LOG_NOTICE, "[safety] cathedral notify: %d",
			    kyrka_last_error(tunnel->ctx));
		}
	}

	if (vigil->now >= tunnel->next_manage) {
		tunnel->next_manage = vigil->now + SAFETY_MANAGE_INTERVAL;

		if (kyrka_key_manage(tunnel->ctx) == -1) {
			vigil_log(LOG_NOTICE, "[safety] key manage: %d",
			    kyrka_last_error(tunnel->ctx));
		}
	}

	if (tunnel->online == 0)
		return;

	if (tunnel->ack_due)
		safety_ack();

	if (tunnel->acked == 0 && vigil->now >= tunnel->next_send) {
		tunnel->next_send = vigil->now + SAFETY_RESEND;
		(void)safety_send(SAFETY_MSG_SHARE);
	}

	safety_check();
}

/*
 * Move our tunnel to another cathedral if our current cathedral stopped
 * answering us.
 */
static void
safety_cathedral_check(void)
{
	PRECOND(tunnel != NULL);

	if ((vigil->now - tunnel->cathedral.last) <= tunnel->cathedral.timeout)
		return;

	if (vigil_call_cathedral_pick(&tunnel->cathedral) == -1) {
		tunnel->cathedral.last = vigil->now;
		return;
	}

	vigil_log(LOG_NOTICE, "[safety] %02x switching to cathedral %s",
	    tunnel->peer, vigil_address(&tunnel->cathedral.addr));

	tunnel->next_notify = 0;
}

/*
 * Ack the share of the other side, a burst of them since we close our
 * tunnel once we are done. If our keys are not up yet we try again on
 * the next update.
 */
static void
safety_ack(void)
{
	int		idx;

	PRECOND(tunnel != NULL);

	for (idx = 0; idx < SAFETY_ACK_BURST; idx++) {
		if (safety_send(SAFETY_MSG_ACK) == -1)
			return;
	}

	tunnel->ack_due = 0;
}

/*
 * We are done once we have the share of the other side, the other side
 * acked our share and our acks for the share of the other side went out.
 */
static void
safety_check(void)
{
	PRECOND(tunnel != NULL);

	if (tunnel->done)
		return;

	if (tunnel->got && tunnel->acked && tunnel->ack_due == 0)
		safety_done();
}

/*
 * Send the given message to the other side, a share carries ours.
 * Returns -1 if libkyrka refused the message.
 */
static int
safety_send(int type)
{
	int			ret;
	size_t			len;
	struct kyrka_packet	pkt;
	struct safety_msg	*msg;

	PRECOND(type == SAFETY_MSG_SHARE || type == SAFETY_MSG_ACK);
	PRECOND(tunnel != NULL);

	if ((msg = kyrka_packet_databuf(tunnel->ctx, &pkt, &len)) == NULL)
		fatal("failed to get databuf for safety message");

	VERIFY(len >= sizeof(*msg));

	nyfe_mem_zero(msg, sizeof(*msg));
	msg->type = type;

	if (type == SAFETY_MSG_SHARE)
		memcpy(msg->data, tunnel->own, sizeof(tunnel->own));

	pkt.length = sizeof(*msg);
	pkt.shroud = KYRKA_PACKET_SHROUD_CATHEDRAL;

	ret = kyrka_heaven_input(tunnel->ctx, &pkt);
	nyfe_mem_zero(&pkt, sizeof(pkt));

	return (ret);
}

/*
 * We have the share of the other side and the other side acked our share.
 * Put the input together for the platform, wipe both shares and close
 * the tunnel.
 */
static void
safety_done(void)
{
	u_int8_t	*low, *high;

	PRECOND(tunnel != NULL);
	PRECOND(tunnel->got && tunnel->acked && tunnel->ack_due == 0);

	tunnel->done = 1;

	if (tunnel->first) {
		low = tunnel->own;
		high = tunnel->theirs;
	} else {
		low = tunnel->theirs;
		high = tunnel->own;
	}

	vigil_lock(&safety_lock);

	if (shared.peer == tunnel->peer &&
	    shared.state != SAFETY_STATE_NONE) {
		shared.fresh = 1;
		shared.state = SAFETY_STATE_DONE;
		memcpy(shared.input, low, SAFETY_SHARE_LEN);
		memcpy(shared.input + SAFETY_SHARE_LEN, high, SAFETY_SHARE_LEN);
	}

	vigil_unlock(&safety_lock);

	nyfe_mem_zero(tunnel->own, sizeof(tunnel->own));
	nyfe_mem_zero(tunnel->theirs, sizeof(tunnel->theirs));

	tunnel->expire = vigil->now;
	vigil_chat_safety(tunnel->peer);
}

/*
 * Set the state of the exchange with the given peer.
 */
static void
safety_state(u_int8_t peer, int state)
{
	vigil_lock(&safety_lock);

	if (shared.peer == peer)
		shared.state = state;

	vigil_unlock(&safety_lock);
}

/*
 * Derive the secret for our tunnel to the given peer from both halves,
 * see the top of this file.
 */
static void
safety_secret(u_int8_t peer, const u_int8_t *own, const u_int8_t *theirs,
    u_int8_t *secret, size_t len)
{
	struct nyfe_kmac256	kdf;
	u_int64_t		flock;
	u_int8_t		ilen, low, high;
	u_int8_t		key[VIGIL_SAFETY_HALF_LEN * 2];

	PRECOND(own != NULL);
	PRECOND(theirs != NULL);
	PRECOND(secret != NULL);
	PRECOND(len == VIGIL_KEY_LENGTH);

	nyfe_zeroize_register(key, sizeof(key));
	nyfe_zeroize_register(&kdf, sizeof(kdf));

	low = vigil->gospel->spi & 0xff;
	high = peer;

	if (low < high) {
		memcpy(key, own, VIGIL_SAFETY_HALF_LEN);
		memcpy(key + VIGIL_SAFETY_HALF_LEN,
		    theirs, VIGIL_SAFETY_HALF_LEN);
	} else {
		high = low;
		low = peer;
		memcpy(key, theirs, VIGIL_SAFETY_HALF_LEN);
		memcpy(key + VIGIL_SAFETY_HALF_LEN,
		    own, VIGIL_SAFETY_HALF_LEN);
	}

	flock = htobe64(vigil->gospel->cathedral_flock);

	nyfe_kmac256_init(&kdf, key, sizeof(key),
	    SAFETY_KDF_LABEL, sizeof(SAFETY_KDF_LABEL) - 1);
	nyfe_zeroize(key, sizeof(key));

	ilen = sizeof(flock);
	nyfe_kmac256_update(&kdf, &ilen, sizeof(ilen));
	nyfe_kmac256_update(&kdf, &flock, sizeof(flock));

	ilen = sizeof(low);
	nyfe_kmac256_update(&kdf, &ilen, sizeof(ilen));
	nyfe_kmac256_update(&kdf, &low, sizeof(low));

	nyfe_kmac256_update(&kdf, &ilen, sizeof(ilen));
	nyfe_kmac256_update(&kdf, &high, sizeof(high));

	nyfe_kmac256_final(&kdf, secret, len);
	nyfe_zeroize(&kdf, sizeof(kdf));
}

/*
 * Callback for when an event occurred on the socket of our tunnel,
 * we push what we read into the libkyrka context of the tunnel.
 */
static void
safety_read(void *udata)
{
	int			idx;
	size_t			len;
	ssize_t			ret;
	struct kyrka_packet	pkt;
	u_int8_t		*ptr;
	struct sockaddr_in	from;
	socklen_t		fromlen;

	PRECOND(udata == tunnel);

	if (!(tunnel->io.flags & VIGIL_IO_READABLE))
		return;

	tunnel->io.flags &= ~VIGIL_IO_READABLE;

	for (idx = 0; idx < VIGIL_IO_BATCH; idx++) {
		ptr = kyrka_packet_recvbuf(tunnel->ctx, &pkt, &len);
		if (ptr == NULL)
			fatal("failed to get safety receive buffer");

		fromlen = sizeof(from);

		ret = recvfrom(tunnel->io.fd, ptr, len, 0,
		    (struct sockaddr *)&from, &fromlen);
		if (ret == -1) {
			if (errno == EINTR)
				continue;
			if (errno != EWOULDBLOCK && errno != EAGAIN)
				vigil_log(LOG_NOTICE, "[safety] read: %s",
				    errno_s);
			break;
		}

		if (ret == 0)
			continue;

		if (!vigil_inet_match(&from, &tunnel->cathedral.addr))
			continue;

		tunnel->cathedral.last = vigil->now;
		tunnel->cathedral.timeout = VIGIL_CATHEDRAL_TIMEOUT;

		vigil->gospel_rx += ret;
		pkt.length = ret;
		pkt.shroud = KYRKA_PACKET_SHROUD_CATHEDRAL;

		if (kyrka_purgatory_input(tunnel->ctx, &pkt) == -1 &&
		    kyrka_last_error(tunnel->ctx) != KYRKA_ERROR_NO_RX_KEY) {
			vigil_log(LOG_NOTICE, "[safety] purgatory input: %d",
			    kyrka_last_error(tunnel->ctx));
		}
	}

	nyfe_mem_zero(&pkt, sizeof(pkt));
}

/*
 * Callback from libkyrka when an event occurred on our tunnel.
 */
static void
safety_event(KYRKA *ctx, union kyrka_event *evt, void *udata)
{
	PRECOND(ctx != NULL);
	PRECOND(evt != NULL);
	PRECOND(udata == tunnel);

	switch (evt->type) {
	case KYRKA_EVENT_KEYS_INFO:
		tunnel->next_notify = 0;

		if (evt->keys.tx_spi == 0)
			break;

		if (tunnel->online != 0)
			break;

		vigil_log(LOG_INFO, "[safety] %02x online", tunnel->peer);
		tunnel->online = vigil->now;
		tunnel->next_send = 0;
		break;
	case KYRKA_EVENT_EXCHANGE_INFO:
		vigil_log(LOG_INFO, "[safety] exchange: %s",
		    evt->exchange.reason);
		break;
	case KYRKA_EVENT_LOGMSG:
		vigil_log(LOG_INFO, "[safety] libkyrka: %s", evt->logmsg.log);
		break;
	default:
		break;
	}
}

/*
 * Callback from libkyrka with a decrypted message from the other side,
 * we ack every share we get.
 */
static void
safety_heaven_input(struct kyrka_packet *pkt, u_int64_t seq, void *udata)
{
	struct safety_msg	*msg;

	PRECOND(pkt != NULL);
	PRECOND(udata == tunnel);

	if (pkt->type != KYRKA_PACKET_TYPE_IP)
		return;

	if (pkt->length != sizeof(*msg))
		return;

	msg = kyrka_packet_data(pkt);

	switch (msg->type) {
	case SAFETY_MSG_SHARE:
		if (tunnel->got == 0) {
			vigil_log(LOG_INFO, "[safety] %02x got its share",
			    tunnel->peer);
			memcpy(tunnel->theirs, msg->data,
			    sizeof(tunnel->theirs));
			tunnel->got = 1;
		}

		tunnel->ack_due = 1;
		safety_ack();
		safety_check();
		break;
	case SAFETY_MSG_ACK:
		if (tunnel->acked)
			break;

		vigil_log(LOG_INFO, "[safety] %02x acked our share",
		    tunnel->peer);
		tunnel->acked = 1;
		safety_check();
		break;
	default:
		break;
	}

	nyfe_mem_zero(msg, sizeof(*msg));
}

/*
 * Callback from libkyrka with ciphertext for the other side.
 */
static void
safety_purgatory_input(struct kyrka_packet *pkt, u_int64_t seq, void *udata)
{
	PRECOND(pkt != NULL);
	PRECOND(udata == tunnel);

	safety_output(pkt);
}

/*
 * Callback from libkyrka with data for the cathedral.
 */
static void
safety_cathedral_send(struct kyrka_packet *pkt, u_int64_t magic, void *udata)
{
	PRECOND(pkt != NULL);
	PRECOND(udata == tunnel);

	safety_output(pkt);
}

/*
 * Send the given packet to the cathedral, all packets go via the
 * cathedral.
 */
static void
safety_output(struct kyrka_packet *pkt)
{
	size_t		len;
	u_int8_t	*ptr;

	PRECOND(pkt != NULL);
	PRECOND(tunnel != NULL);

	ptr = kyrka_packet_sendbuf(tunnel->ctx, pkt, &len);
	if (ptr == NULL)
		fatal("failed to get safety send buffer");

	if (sendto(tunnel->io.fd, ptr, len, 0,
	    (const struct sockaddr *)&tunnel->cathedral.addr,
	    sizeof(tunnel->cathedral.addr)) == -1) {
		if (errno != EAGAIN && errno != EWOULDBLOCK &&
		    !vigil_socket_unreachable(errno, 1))
			vigil_log(LOG_NOTICE, "[safety] sendto: %s", errno_s);
		return;
	}

	vigil->gospel_tx += len;
}
