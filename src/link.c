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

#include <pthread.h>
#include <stdio.h>

#include "vigil.h"

#include <libkyrka/libnyfe.h>

/*
 * Our side of the link, the buffers are only set while we are
 * inside of vigil_link_input() or vigil_link_output().
 */
struct link {
	KYRKA		*ctx;
	int		ready;

	u_int8_t	*out;
	size_t		outlen;
	size_t		outoff;
	int		overflow;

	u_int8_t	*msg;
	size_t		msglen;
	size_t		total;
	size_t		got;
};

static void	link_lock(void);
static void	link_unlock(void);

static void	link_event(KYRKA *, union kyrka_event *, void *);
static void	link_plaintext(struct kyrka_packet *, u_int64_t, void *);
static void	link_ciphertext(struct kyrka_packet *, u_int64_t, void *);

static struct link		link;
static pthread_mutex_t		lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * Set up our side of the link with the given secret, the other side
 * must load the same one.
 */
int
vigil_link_open(const u_int8_t *secret, size_t len)
{
	PRECOND(secret != NULL);

	if (len != VIGIL_LINK_SECRET_LEN)
		return (-1);

	vigil_link_close();

	link_lock();
	vigil_kyrka_enter();

	if ((link.ctx = kyrka_ctx_alloc(link_event, NULL)) == NULL)
		fatal("failed to create link context");

	if (kyrka_heaven_ifc(link.ctx, link_plaintext, NULL) == -1)
		fatal("kyrka_heaven_ifc: %d", kyrka_last_error(link.ctx));

	if (kyrka_purgatory_ifc(link.ctx, link_ciphertext, NULL) == -1)
		fatal("kyrka_purgatory_ifc: %d", kyrka_last_error(link.ctx));

	if (kyrka_secret_load(link.ctx, secret, len) == -1)
		fatal("kyrka_secret_load: %d", kyrka_last_error(link.ctx));

	vigil_kyrka_leave();
	link_unlock();

	return (0);
}

/*
 * Tear down our side of the link.
 */
void
vigil_link_close(void)
{
	link_lock();

	if (link.ctx != NULL) {
		vigil_kyrka_enter();
		kyrka_ctx_free(link.ctx);
		vigil_kyrka_leave();
	}

	nyfe_mem_zero(&link, sizeof(link));

	link_unlock();
}

/*
 * Returns 1 if we have keys in both directions.
 */
int
vigil_link_ready(void)
{
	int		ready;

	link_lock();
	ready = link.ready;
	link_unlock();

	return (ready);
}

/*
 * Decrypt the batch of packets the other side gave us, the message it
 * carried ends up in msg. Returns its length or 0 if there was none.
 */
ssize_t
vigil_link_input(const u_int8_t *in, size_t inlen, u_int8_t *msg, size_t msglen)
{
	struct kyrka_packet	pkt;
	ssize_t			ret;
	u_int8_t		*ptr;
	u_int16_t		plen;
	size_t			off, len, maxlen;

	PRECOND(in != NULL || inlen == 0);
	PRECOND(msg != NULL);

	link_lock();

	if (link.ctx == NULL) {
		link_unlock();
		return (-1);
	}

	link.msg = msg;
	link.msglen = msglen;
	link.total = 0;
	link.got = 0;

	vigil_kyrka_enter();

	for (off = 0; inlen - off > sizeof(plen); off += len) {
		memcpy(&plen, &in[off], sizeof(plen));
		len = be16toh(plen);
		off += sizeof(plen);

		if (len > inlen - off)
			break;

		ptr = kyrka_packet_recvbuf(link.ctx, &pkt, &maxlen);
		if (ptr == NULL)
			fatal("failed to get link receive buffer");

		if (len > maxlen)
			continue;

		memcpy(ptr, &in[off], len);
		pkt.length = len;
		pkt.shroud = KYRKA_PACKET_SHROUD_PEER;

		if (kyrka_purgatory_input(link.ctx, &pkt) == -1) {
			vigil_log(LOG_NOTICE,
			    "[link] kyrka_purgatory_input: %d",
			    kyrka_last_error(link.ctx));
		}
	}

	vigil_kyrka_leave();

	if (link.total != 0 && link.got == link.total)
		ret = link.total;
	else
		ret = 0;

	link.msg = NULL;
	link.msglen = 0;

	nyfe_mem_zero(&pkt, sizeof(pkt));
	link_unlock();

	return (ret);
}

/*
 * Let the link manage its keys and encrypt msg if one is given, the
 * packets for the other side end up in out. Returns the length of that
 * batch or -1 if msg could not be sent.
 */
ssize_t
vigil_link_output(const u_int8_t *msg, size_t msglen, u_int8_t *out,
    size_t outlen)
{
	struct kyrka_packet		pkt;
	ssize_t				ret;
	struct vigil_link_fragment	*frag;
	size_t				off, len, maxlen;

	PRECOND(msg != NULL || msglen == 0);
	PRECOND(out != NULL);

	if (msglen > VIGIL_LINK_MESSAGE_MAX)
		return (-1);

	link_lock();

	if (link.ctx == NULL) {
		link_unlock();
		return (-1);
	}

	if (msglen > 0 && link.ready == 0) {
		link_unlock();
		return (-1);
	}

	link.out = out;
	link.outlen = outlen;
	link.outoff = 0;
	link.overflow = 0;

	vigil_kyrka_enter();

	if (kyrka_key_manage(link.ctx) == -1) {
		vigil_log(LOG_NOTICE, "[link]: kyrka_key_manage %d",
		    kyrka_last_error(link.ctx));
	}

	for (off = 0; off < msglen; off += len) {
		len = msglen - off;
		if (len > VIGIL_LINK_FRAGMENT_DATA)
			len = VIGIL_LINK_FRAGMENT_DATA;

		frag = kyrka_packet_databuf(link.ctx, &pkt, &maxlen);
		if (frag == NULL)
			fatal("failed to get link data buffer");

		if (sizeof(*frag) > maxlen)
			fatal("link fragment does not fit a packet");

		memset(frag, 0, sizeof(*frag));

		frag->total = htobe32(msglen);
		frag->offset = htobe32(off);
		memcpy(frag->data, &msg[off], len);

		pkt.length = sizeof(*frag);
		pkt.shroud = KYRKA_PACKET_SHROUD_PEER;

		if (kyrka_heaven_input(link.ctx, &pkt) == -1) {
			if (kyrka_last_error(link.ctx) == KYRKA_ERROR_NO_TX_KEY)
				link.ready = 0;
			link.overflow = 1;
			break;
		}
	}

	vigil_kyrka_leave();

	if (link.overflow)
		ret = -1;
	else
		ret = link.outoff;

	link.out = NULL;
	link.outlen = 0;

	nyfe_mem_zero(&pkt, sizeof(pkt));
	link_unlock();

	return (ret);
}

/*
 * Callback from libkyrka when an event occurred on the link.
 */
static void
link_event(KYRKA *ctx, union kyrka_event *evt, void *udata)
{
	PRECOND(ctx != NULL);
	PRECOND(evt != NULL);

	switch (evt->type) {
	case KYRKA_EVENT_KEYS_INFO:
		if (evt->keys.tx_spi != 0 && evt->keys.rx_spi != 0)
			link.ready = 1;
		break;
	case KYRKA_EVENT_KEYS_ERASED:
		link.ready = 0;
		break;
	default:
		break;
	}
}

/*
 * Callback from libkyrka with a decrypted fragment, the fragments of
 * a message arrive in order in the same batch.
 */
static void
link_plaintext(struct kyrka_packet *pkt, u_int64_t seq, void *udata)
{
	struct vigil_link_fragment	*frag;
	size_t				len, total, off;

	PRECOND(pkt != NULL);

	if (link.msg == NULL)
		return;

	if (pkt->type != KYRKA_PACKET_TYPE_IP)
		return;

	if (pkt->length != sizeof(*frag))
		return;

	frag = kyrka_packet_data(pkt);

	total = be32toh(frag->total);
	off = be32toh(frag->offset);

	if (off == 0) {
		link.total = total;
		link.got = 0;
	}

	if (total != link.total)
		return;

	if (off != link.got)
		return;

	if (total > link.msglen) {
		link.total = 0;
		return;
	}

	if (off >= total) {
		link.total = 0;
		return;
	}

	len = total - off;
	if (len > VIGIL_LINK_FRAGMENT_DATA)
		len = VIGIL_LINK_FRAGMENT_DATA;

	memcpy(&link.msg[off], frag->data, len);
	link.got += len;
}

/*
 * Callback from libkyrka with ciphertext for the other side, we add
 * it to the batch we are building. Anything outside of a batch is
 * dropped, offers are sent again on the next key management.
 */
static void
link_ciphertext(struct kyrka_packet *pkt, u_int64_t seq, void *udata)
{
	size_t		len;
	u_int16_t	plen;
	u_int8_t	*ptr;

	PRECOND(pkt != NULL);

	if (link.out == NULL)
		return;

	if ((ptr = kyrka_packet_sendbuf(link.ctx, pkt, &len)) == NULL)
		fatal("failed to get link send buffer");

	if (len + sizeof(plen) > link.outlen - link.outoff) {
		link.overflow = 1;
		return;
	}

	plen = htobe16(len);
	memcpy(&link.out[link.outoff], &plen, sizeof(plen));
	memcpy(&link.out[link.outoff + sizeof(plen)], ptr, len);

	link.outoff += len + sizeof(plen);
}

/*
 * Take the lock that protects the link.
 */
static void
link_lock(void)
{
	if (pthread_mutex_lock(&lock) != 0)
		fatal("failed to take the link lock");
}

/*
 * Release the lock that protects the link.
 */
static void
link_unlock(void)
{
	if (pthread_mutex_unlock(&lock) != 0)
		fatal("failed to release the link lock");
}
