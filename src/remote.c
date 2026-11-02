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

#include <ctype.h>
#include <inttypes.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

#include <sodium.h>

#include "vigil.h"
#include "wordlist.h"

#include <libkyrka/libnyfe.h>

/*
 * Joining a flock with the words from covenant, for a device that cannot
 * scan a QR code. See covenant its src/remote.c for how the words are
 * made, the first VIGIL_REMOTE_ROUTE_WORDS words are the route:
 *
 *	flock (7) || covenant (4) || covenant kek (1) || our kek (1) ||
 *	cathedral (4) || port (2) || checksum (2)
 *
 * Everything is big endian and the flock comes without its domain. The
 * route is a number in base VIGIL_WORD_COUNT, most significant word
 * first. The checksum is the first two bytes of:
 *
 *	SHA3-256(REMOTE_ROUTE_LABEL || route[0..19) || passphrase)
 *
 * The platform joins the flock at Reliquary with the cosk we make for
 * it, after which we set up the tunnel to covenant the way cephas does.
 * Covenant sends our configuration over it and we add our cathedral
 * secret and cosk to it.
 */

/* The labels cephas uses. */
#define REMOTE_ROUTE_LABEL	"CEPHAS.ROUTE.CHECKSUM"
#define REMOTE_KDF_LABEL	"CEPHAS.KDF.SHARED.SECRET"
#define REMOTE_PASS_LABEL	"CEPHAS.PBKDF.PASSPHRASE"

/* The length of a route once decoded from its words. */
#define REMOTE_ROUTE_LEN	21

/* The MTU we give libkyrka, the configuration comes in one packet. */
#define REMOTE_MTU		1400

/* The length of the passphrase, words of at most 9 letters and a space. */
#define REMOTE_PASS_LEN		(VIGIL_REMOTE_PASS_WORDS * 10 + 1)

/* The length of the words we accept, the same but for all words. */
#define REMOTE_WORDS_LEN	(VIGIL_REMOTE_WORDS * 10 + 1)

/* The largest key we hex encode, with its NUL. */
#define REMOTE_HEX_MAX		((VIGIL_COSK_LENGTH * 2) + 1)

/*
 * The messages we exchange with covenant, the same as cephas. Our ack
 * carries the SHA3-256 digest of the configuration we got.
 */
#define REMOTE_MSG_HEARTBEAT	1
#define REMOTE_MSG_KEK		2
#define REMOTE_MSG_ACK		3
#define REMOTE_MSG_CONFIG	4

struct remote_msg {
	u_int8_t	type;
	u_int8_t	data[32];
} __attribute__((packed));

/* The room for a configuration, its public part and the key lines. */
#define REMOTE_CONFIG_LEN	1344

struct remote_config {
	u_int8_t	type;
	u_int16_t	length;
	u_int8_t	data[REMOTE_CONFIG_LEN];
} __attribute__((packed));

/*
 * The state of joining remotely, from the words up until we have our
 * configuration.
 */
struct remote {
	int				fd;
	struct kyrka_cathedral_cfg	cfg;
	struct sockaddr_in		peer;
	u_int64_t			last;
	int				state;
	int				error;
	KYRKA				*kyrka;
	u_int32_t			covenant;
	struct sockaddr_in		cathedral;
	u_int8_t			digest[32];
	u_int8_t			cs[VIGIL_KEY_LENGTH];
	u_int8_t			sk[VIGIL_COSK_LENGTH];
	char				pass[REMOTE_PASS_LEN];
	u_int8_t			route[REMOTE_ROUTE_LEN];
	char				config[VIGIL_CONFIG_MAX];
};

static void	remote_lock(void);
static void	remote_free(void);
static void	remote_read(void);
static int	remote_state(void);
static void	remote_unlock(void);
static void	remote_heartbeat(void);
static int	remote_route_apply(void);
static int	remote_route_check(void);
static int	remote_identity(const char *);
static int	remote_config_check(const char *);
static void	remote_ack(const u_int8_t *, size_t);
static void	remote_secret_derive(u_int8_t *, size_t);
static int	remote_passphrase(const u_int16_t *, size_t);
static void	remote_received(const struct remote_config *);
static int	remote_words_parse(const char *, u_int16_t *, size_t);
static int	remote_route_decode(const u_int16_t *, size_t,
		    u_int8_t *, size_t);

static void	remote_event(KYRKA *, union kyrka_event *, void *);
static void	remote_heaven_ifc(struct kyrka_packet *, u_int64_t, void *);
static void	remote_purgatory_ifc(struct kyrka_packet *, u_int64_t, void *);
static void	remote_cathedral_ifc(struct kyrka_packet *, u_int64_t, void *);

/* Held while touching the remote state, it may be stopped at any time. */
static pthread_mutex_t	remote_mutex = PTHREAD_MUTEX_INITIALIZER;

/* The remote join that is in progress, if any. */
static struct remote	*remote = NULL;

/*
 * Returns the word at idx in the word list, or NULL once past its end.
 */
const char *
vigil_remote_word(u_int16_t idx)
{
	if (idx >= VIGIL_WORD_COUNT)
		return (NULL);

	return (vigil_words[idx]);
}

/*
 * Read the route from the given words, which are separated by spaces,
 * and make the cosk pair we join the flock under. Writes the flock, the
 * cathedral and the public half of the cosk as json into out.
 *
 * Returns -1 if the words are not right.
 */
int
vigil_remote_words(const char *words, char *out, size_t outlen)
{
	int			ret;
	struct vigil_buf	buf;
	char			hex[REMOTE_HEX_MAX];
	u_int8_t		pk[VIGIL_KEY_LENGTH];
	u_int16_t		all[VIGIL_REMOTE_WORDS];

	PRECOND(words != NULL);
	PRECOND(out != NULL);
	PRECOND(outlen > 0);

	ret = -1;

	remote_lock();
	remote_free();

	if ((remote = calloc(1, sizeof(*remote))) == NULL)
		fatal("calloc: failed to allocate remote");

	nyfe_zeroize_register(remote, sizeof(*remote));
	remote->fd = -1;

	nyfe_zeroize_register(all, sizeof(all));

	if (remote_words_parse(words, all, VIGIL_REMOTE_WORDS) == -1)
		goto cleanup;

	if (remote_route_decode(all, VIGIL_REMOTE_ROUTE_WORDS,
	    remote->route, sizeof(remote->route)) == -1)
		goto cleanup;

	if (remote_passphrase(all, VIGIL_REMOTE_WORDS) == -1)
		goto cleanup;

	if (remote_route_check() == -1)
		goto cleanup;

	if (remote_route_apply() == -1)
		goto cleanup;

	if (crypto_sign_keypair(pk, remote->sk) != 0)
		fatal("failed to create cosk pair");

	(void)sodium_bin2hex(hex, sizeof(hex), pk, sizeof(pk));

	vigil_buf_init(&buf, out, outlen);
	vigil_buf_appendf(&buf,
	    "{\"flock\":\"%016" PRIx64 "\",\"cathedral\":\"%s\","
	    "\"cosk\":\"%s\"}", remote->cfg.flock_src,
	    vigil_address(&remote->cathedral), hex);

	if (buf.flags & VIGIL_BUF_FULL)
		goto cleanup;

	ret = 0;

cleanup:
	nyfe_zeroize(all, sizeof(all));

	if (ret == -1)
		remote_free();

	remote_unlock();

	return (ret);
}

/*
 * We joined the flock at Reliquary, which gave us our cathedral id and
 * secret (both hex). Set up the tunnel to covenant with them.
 */
int
vigil_remote_start(const char *id, const char *secret)
{
	size_t		len;
	u_int8_t	key[VIGIL_KEY_LENGTH];

	PRECOND(id != NULL);
	PRECOND(secret != NULL);

	remote_lock();

	if (remote == NULL) {
		remote_unlock();
		return (-1);
	}

	if (remote->kyrka != NULL) {
		remote_unlock();
		return (-1);
	}

	if (remote_identity(id) == -1) {
		remote_unlock();
		return (-1);
	}

	if (strlen(secret) != sizeof(remote->cs) * 2) {
		remote_unlock();
		return (-1);
	}

	if (sodium_hex2bin(remote->cs, sizeof(remote->cs), secret,
	    strlen(secret), NULL, &len, NULL) != 0) {
		remote_unlock();
		return (-1);
	}

	if (len != sizeof(remote->cs)) {
		remote_unlock();
		return (-1);
	}

	nyfe_zeroize_register(key, sizeof(key));
	remote_secret_derive(key, sizeof(key));

	vigil_kyrka_enter();

	if ((remote->kyrka = kyrka_ctx_alloc(remote_event, NULL)) == NULL)
		fatal("failed to create remote context");

	if (kyrka_shroud_enable(remote->kyrka) == -1) {
		fatal("failed to enable shroud: %d",
		    kyrka_last_error(remote->kyrka));
	}

	if (kyrka_mtu_size(remote->kyrka, REMOTE_MTU) == -1) {
		fatal("failed to set libkyrka mtu size: %d",
		    kyrka_last_error(remote->kyrka));
	}

	if (kyrka_heaven_ifc(remote->kyrka, remote_heaven_ifc, NULL) == -1) {
		fatal("kyrka_heaven_ifc: %d",
		    kyrka_last_error(remote->kyrka));
	}

	if (kyrka_purgatory_ifc(remote->kyrka,
	    remote_purgatory_ifc, NULL) == -1) {
		fatal("kyrka_purgatory_ifc: %d",
		    kyrka_last_error(remote->kyrka));
	}

	if (kyrka_secret_load(remote->kyrka, key, sizeof(key)) == -1) {
		fatal("kyrka_secret_load: %d",
		    kyrka_last_error(remote->kyrka));
	}

	if (kyrka_cathedral_config(remote->kyrka, &remote->cfg) == -1) {
		fatal("failed to configure cathedral: %d",
		    kyrka_last_error(remote->kyrka));
	}

	if (kyrka_cathedral_secret_load(remote->kyrka,
	    remote->cs, sizeof(remote->cs)) == -1) {
		fatal("kyrka_cathedral_secret_load: %d",
		    kyrka_last_error(remote->kyrka));
	}

	if (kyrka_cathedral_cosk_load(remote->kyrka,
	    remote->sk, sizeof(remote->sk)) == -1) {
		fatal("kyrka_cathedral_cosk_load: %d",
		    kyrka_last_error(remote->kyrka));
	}

	vigil_kyrka_leave();

	nyfe_zeroize(key, sizeof(key));

	if ((remote->fd = socket(AF_INET, SOCK_DGRAM, 0)) == -1) {
		vigil_log(LOG_NOTICE, "[remote] socket: %s", errno_s);
		remote_free();
		remote_unlock();
		return (-1);
	}

	nyfe_mem_zero(remote->pass, sizeof(remote->pass));
	vigil_socket_nonblock(remote->fd);

	memcpy(&remote->peer, &remote->cathedral, sizeof(remote->peer));
	remote->state = VIGIL_REMOTE_WAITING;

	remote_unlock();

	return (0);
}

/*
 * Wait up to a second for packets from the cathedral or covenant and do
 * the book keeping for the tunnel once every second.
 *
 * Returns the state of the tunnel or -1 if it is gone or failed.
 */
int
vigil_remote_run(void)
{
	struct pollfd		pfd;
	u_int64_t		now;
	int			nfd, ret;

	remote_lock();

	if (remote == NULL) {
		remote_unlock();
		return (-1);
	}

	if (remote->kyrka == NULL) {
		remote_unlock();
		return (-1);
	}

	pfd.fd = remote->fd;
	pfd.events = POLLIN;

	ret = remote_state();
	remote_unlock();

	if ((nfd = poll(&pfd, 1, 1000)) == -1) {
		if (errno == EINTR)
			return (ret);
		vigil_log(LOG_NOTICE, "[remote] poll: %s", errno_s);
		return (-1);
	}

	remote_lock();

	if (remote == NULL) {
		remote_unlock();
		return (-1);
	}

	if (remote->kyrka == NULL) {
		remote_unlock();
		return (-1);
	}

	vigil_kyrka_enter();

	if (nfd > 0) {
		if (pfd.revents & POLLIN)
			remote_read();
	}

	now = vigil_time_ms();

	if ((now - remote->last) >= 1000) {
		remote->last = now;

		if (kyrka_cathedral_notify(remote->kyrka) == -1) {
			vigil_log(LOG_NOTICE,
			    "[remote] kyrka_cathedral_notify: %d",
			    kyrka_last_error(remote->kyrka));
			remote->error = 1;
		}

		if (kyrka_key_manage(remote->kyrka) == -1) {
			vigil_log(LOG_NOTICE, "[remote] kyrka_key_manage: %d",
			    kyrka_last_error(remote->kyrka));
			remote->error = 1;
		}

		if (remote->state == VIGIL_REMOTE_ONLINE)
			remote_heartbeat();
	}

	vigil_kyrka_leave();

	ret = remote_state();
	remote_unlock();

	return (ret);
}

/*
 * Copy the configuration we got from covenant into out once we have it,
 * our cathedral secret and cosk are part of it.
 */
int
vigil_remote_config(char *out, size_t outlen)
{
	size_t		len;

	PRECOND(out != NULL);
	PRECOND(outlen > 0);

	remote_lock();

	if (remote == NULL) {
		remote_unlock();
		return (-1);
	}

	if (remote->state != VIGIL_REMOTE_DONE) {
		remote_unlock();
		return (-1);
	}

	len = strlen(remote->config);

	if (len >= outlen) {
		remote_unlock();
		return (-1);
	}

	memcpy(out, remote->config, len + 1);
	remote_unlock();

	return (0);
}

/*
 * Stop joining remotely and forget everything about it.
 */
void
vigil_remote_stop(void)
{
	remote_lock();
	remote_free();
	remote_unlock();
}

/*
 * Take the remote lock.
 */
static void
remote_lock(void)
{
	if (pthread_mutex_lock(&remote_mutex) != 0)
		fatal("failed to take the remote lock");
}

/*
 * Release the remote lock.
 */
static void
remote_unlock(void)
{
	if (pthread_mutex_unlock(&remote_mutex) != 0)
		fatal("failed to release the remote lock");
}

/*
 * Free the remote state, the caller holds the remote lock.
 */
static void
remote_free(void)
{
	if (remote == NULL)
		return;

	if (remote->fd != -1)
		(void)close(remote->fd);

	if (remote->kyrka != NULL) {
		vigil_kyrka_enter();
		kyrka_ctx_free(remote->kyrka);
		vigil_kyrka_leave();
	}

	nyfe_zeroize(remote, sizeof(*remote));
	free(remote);

	remote = NULL;
}

/*
 * Returns the state of the remote join, or -1 if it failed. The caller
 * holds the remote lock.
 */
static int
remote_state(void)
{
	PRECOND(remote != NULL);

	if (remote->error)
		return (-1);

	return (remote->state);
}

/*
 * Parse the cathedral id Reliquary gave us when we joined the flock.
 */
static int
remote_identity(const char *id)
{
	char		*ep;
	size_t		idx;
	unsigned long	cid;

	PRECOND(id != NULL);
	PRECOND(remote != NULL);

	if (strlen(id) != 8)
		return (-1);

	for (idx = 0; idx < 8; idx++) {
		if (!isxdigit((unsigned char)id[idx]))
			return (-1);
	}

	errno = 0;
	cid = strtoul(id, &ep, 16);

	if (errno != 0)
		return (-1);

	if (*ep != '\0')
		return (-1);

	if (cid == 0)
		return (-1);

	if (cid == remote->covenant)
		return (-1);

	remote->cfg.identity = cid;

	return (0);
}

/*
 * Check that the configuration covenant sent us parses and that it is
 * for our cathedral id in the flock we joined.
 */
static int
remote_config_check(const char *config)
{
	struct vigil_config	*cfg;
	char			err[128];

	PRECOND(config != NULL);
	PRECOND(remote != NULL);

	cfg = vigil_config_parse(config, strlen(config), err, sizeof(err));
	if (cfg == NULL) {
		vigil_log(LOG_NOTICE, "[remote] config: %s", err);
		return (-1);
	}

	if (cfg->cathedral_id != remote->cfg.identity) {
		vigil_log(LOG_NOTICE, "[remote] config for another device");
		vigil_config_free(cfg);
		return (-1);
	}

	if (cfg->cathedral_flock != remote->cfg.flock_src) {
		vigil_log(LOG_NOTICE, "[remote] config for another flock");
		vigil_config_free(cfg);
		return (-1);
	}

	vigil_config_free(cfg);

	return (0);
}

/*
 * Split the words and look each of them up in the word list, there must
 * be exactly count of them. Returns -1 if a word is not in the list or
 * if the count is off.
 */
static int
remote_words_parse(const char *words, u_int16_t *out, size_t count)
{
	size_t		idx, len;
	int		ret, which;
	char		copy[REMOTE_WORDS_LEN], *word, *next;

	PRECOND(words != NULL);
	PRECOND(out != NULL);
	PRECOND(count == VIGIL_REMOTE_WORDS);

	if ((len = strlen(words)) >= sizeof(copy))
		return (-1);

	nyfe_zeroize_register(copy, sizeof(copy));
	memcpy(copy, words, len + 1);

	ret = 0;
	idx = 0;

	for (word = copy; word != NULL; word = next) {
		if ((next = strchr(word, ' ')) != NULL)
			*(next)++ = '\0';

		for (which = 0; which < VIGIL_WORD_COUNT; which++) {
			if (!strcmp(vigil_words[which], word))
				break;
		}

		if (which == VIGIL_WORD_COUNT) {
			ret = -1;
			break;
		}

		if (idx == count) {
			ret = -1;
			break;
		}

		out[idx++] = which;
	}

	nyfe_zeroize(copy, sizeof(copy));

	if (ret == -1)
		return (-1);

	if (idx != count)
		return (-1);

	return (0);
}

/*
 * Put the passphrase together from the words that follow the route, the
 * way cephas does with every word followed by a space.
 */
static int
remote_passphrase(const u_int16_t *words, size_t count)
{
	size_t			idx;
	struct vigil_buf	buf;

	PRECOND(words != NULL);
	PRECOND(count == VIGIL_REMOTE_WORDS);
	PRECOND(remote != NULL);

	vigil_buf_init(&buf, remote->pass, sizeof(remote->pass));

	for (idx = VIGIL_REMOTE_ROUTE_WORDS; idx < count; idx++)
		vigil_buf_appendf(&buf, "%s ", vigil_words[words[idx]]);

	if (buf.flags & VIGIL_BUF_FULL) {
		nyfe_mem_zero(remote->pass, sizeof(remote->pass));
		return (-1);
	}

	return (0);
}

/*
 * Decode the route from its words into route, see the top of this file.
 */
static int
remote_route_decode(const u_int16_t *words, size_t count, u_int8_t *route,
    size_t len)
{
	size_t		idx;
	u_int32_t	val;
	int		word;

	PRECOND(words != NULL);
	PRECOND(count == VIGIL_REMOTE_ROUTE_WORDS);
	PRECOND(route != NULL);
	PRECOND(len == REMOTE_ROUTE_LEN);

	memset(route, 0, len);

	for (word = 0; word < VIGIL_REMOTE_ROUTE_WORDS; word++) {
		val = words[word];
		for (idx = len; idx > 0; idx--) {
			val += route[idx - 1] * VIGIL_WORD_COUNT;
			route[idx - 1] = val & 0xff;
			val >>= 8;
		}

		if (val != 0)
			return (-1);
	}

	return (0);
}

/*
 * Take the flock, covenant, the tunnel and the cathedral from the route.
 */
static int
remote_route_apply(void)
{
	u_int16_t	port;
	u_int64_t	flock;
	u_int32_t	val, ip;

	PRECOND(remote != NULL);

	flock = 0;
	memcpy(&flock, &remote->route[0], 7);
	remote->cfg.flock_src = be64toh(flock);

	memcpy(&val, &remote->route[7], sizeof(val));
	remote->covenant = be32toh(val);

	remote->cfg.tunnel = (remote->route[12] << 8) | remote->route[11];

	memcpy(&ip, &remote->route[13], sizeof(ip));
	memcpy(&port, &remote->route[17], sizeof(port));

	if (remote->covenant == 0)
		return (-1);

	if (remote->route[11] == 0)
		return (-1);

	if (remote->route[12] == 0)
		return (-1);

	if (remote->route[11] == remote->route[12])
		return (-1);

	if (ip == 0)
		return (-1);

	if (port == 0)
		return (-1);

	remote->cathedral.sin_family = AF_INET;
	remote->cathedral.sin_port = port;
	remote->cathedral.sin_addr.s_addr = ip;

	remote->cfg.send = remote_cathedral_ifc;

	return (0);
}

/*
 * Check the checksum in the route against the passphrase.
 */
static int
remote_route_check(void)
{
	struct nyfe_sha3	ctx;
	u_int8_t		digest[32];

	PRECOND(remote != NULL);

	nyfe_zeroize_register(&ctx, sizeof(ctx));

	nyfe_sha3_init256(&ctx);
	nyfe_sha3_update(&ctx, REMOTE_ROUTE_LABEL,
	    sizeof(REMOTE_ROUTE_LABEL) - 1);
	nyfe_sha3_update(&ctx, remote->route, 19);
	nyfe_sha3_update(&ctx, remote->pass, strlen(remote->pass));
	nyfe_sha3_final(&ctx, digest, sizeof(digest));

	nyfe_zeroize(&ctx, sizeof(ctx));

	if (remote->route[19] != digest[0])
		return (-1);

	if (remote->route[20] != digest[1])
		return (-1);

	return (0);
}

/*
 * Derive the shared secret for the tunnel from the passphrase, the
 * same way cephas does in cephas_secret_derive().
 *
 * We first run the passphrase through nyfe_passphrase_kdf() which
 * produces 64-bytes of IKM, this is then run through KMAC256():
 *
 *    X = len(flock_a) || flock_a || len(flock_b) || flock_b ||
 *        len(identity_a) || identity_a || len(identity_b) || identity_b ||
 *        len(peer_a) || peer_a || len(peer_b) || peer_b
 *
 *    secret = KMAC256(ikm, "CEPHAS.KDF.SHARED.SECRET", X)
 */
static void
remote_secret_derive(u_int8_t *secret, size_t len)
{
	struct nyfe_kmac256	kdf;
	u_int64_t		flock_a, flock_b;
	u_int32_t		identity_a, identity_b;
	u_int8_t		ilen, peer_a, peer_b, tmp;
	u_int8_t		okm[NYFE_KEY_LEN], salt[NYFE_KEY_FILE_SALT_LEN];

	PRECOND(secret != NULL);
	PRECOND(len == VIGIL_KEY_LENGTH);
	PRECOND(remote != NULL);

	nyfe_zeroize_register(okm, sizeof(okm));
	nyfe_zeroize_register(&kdf, sizeof(kdf));

	nyfe_mem_zero(salt, sizeof(salt));

	nyfe_passphrase_kdf(remote->pass, strlen(remote->pass),
	    salt, sizeof(salt), okm, sizeof(okm), REMOTE_PASS_LABEL,
	    sizeof(REMOTE_PASS_LABEL) - 1);

	if (remote->cfg.identity < remote->covenant) {
		identity_a = htobe32(remote->cfg.identity);
		identity_b = htobe32(remote->covenant);
	} else {
		identity_a = htobe32(remote->covenant);
		identity_b = htobe32(remote->cfg.identity);
	}

	peer_a = (remote->cfg.tunnel >> 8) & 0xff;
	peer_b = remote->cfg.tunnel & 0xff;

	if (peer_a > peer_b) {
		tmp = peer_a;
		peer_a = peer_b;
		peer_b = tmp;
	}

	if (remote->cfg.flock_src < remote->cfg.flock_dst) {
		flock_a = htobe64(remote->cfg.flock_src);
		flock_b = htobe64(remote->cfg.flock_dst);
	} else {
		flock_a = htobe64(remote->cfg.flock_dst);
		flock_b = htobe64(remote->cfg.flock_src);
	}

	nyfe_kmac256_init(&kdf, okm, sizeof(okm),
	    REMOTE_KDF_LABEL, sizeof(REMOTE_KDF_LABEL) - 1);
	nyfe_zeroize(okm, sizeof(okm));

	ilen = sizeof(flock_a);
	nyfe_kmac256_update(&kdf, &ilen, sizeof(ilen));
	nyfe_kmac256_update(&kdf, &flock_a, sizeof(flock_a));

	nyfe_kmac256_update(&kdf, &ilen, sizeof(ilen));
	nyfe_kmac256_update(&kdf, &flock_b, sizeof(flock_b));

	ilen = sizeof(identity_a);
	nyfe_kmac256_update(&kdf, &ilen, sizeof(ilen));
	nyfe_kmac256_update(&kdf, &identity_a, sizeof(identity_a));

	nyfe_kmac256_update(&kdf, &ilen, sizeof(ilen));
	nyfe_kmac256_update(&kdf, &identity_b, sizeof(identity_b));

	ilen = sizeof(peer_a);
	nyfe_kmac256_update(&kdf, &ilen, sizeof(ilen));
	nyfe_kmac256_update(&kdf, &peer_a, sizeof(peer_a));

	nyfe_kmac256_update(&kdf, &ilen, sizeof(ilen));
	nyfe_kmac256_update(&kdf, &peer_b, sizeof(peer_b));

	nyfe_kmac256_final(&kdf, secret, len);
	nyfe_zeroize(&kdf, sizeof(kdf));
}

/*
 * Read everything that is waiting on our socket and hand it to libkyrka,
 * the caller holds both the remote and the kyrka lock.
 */
static void
remote_read(void)
{
	ssize_t			ret;
	size_t			len;
	struct kyrka_packet	pkt;
	void			*ptr;

	PRECOND(remote != NULL);

	nyfe_zeroize_register(&pkt, sizeof(pkt));

	for (;;) {
		if ((ptr = kyrka_packet_recvbuf(remote->kyrka,
		    &pkt, &len)) == NULL)
			fatal("failed to get recvbuf");

		if ((ret = recv(remote->fd, ptr, len, 0)) == -1) {
			if (errno == EINTR)
				continue;
			if (errno != EWOULDBLOCK && errno != EAGAIN) {
				vigil_log(LOG_NOTICE,
				    "[remote] recv: %s", errno_s);
				remote->error = 1;
			}
			nyfe_zeroize(&pkt, sizeof(pkt));
			return;
		}

		if (ret == 0)
			continue;

		pkt.length = ret;
		pkt.shroud = KYRKA_PACKET_SHROUD_CATHEDRAL;

		if (kyrka_purgatory_input(remote->kyrka, &pkt) == -1) {
			switch (kyrka_last_error(remote->kyrka)) {
			case KYRKA_ERROR_NO_RX_KEY:
			case KYRKA_ERROR_SHROUD_NO_KEYS:
			case KYRKA_ERROR_CATHEDRAL_CONFIG:
				break;
			default:
				vigil_log(LOG_NOTICE,
				    "[remote] purgatory input: %d",
				    kyrka_last_error(remote->kyrka));
				break;
			}
		}
	}

	nyfe_zeroize(&pkt, sizeof(pkt));
}

/*
 * Send a heartbeat to covenant.
 */
static void
remote_heartbeat(void)
{
	size_t			len;
	struct kyrka_packet	pkt;
	struct remote_msg	*msg;

	PRECOND(remote != NULL);

	if ((msg = kyrka_packet_databuf(remote->kyrka, &pkt, &len)) == NULL)
		fatal("failed to get databuf for heartbeat");

	VERIFY(len >= sizeof(*msg));

	nyfe_mem_zero(msg, sizeof(*msg));
	msg->type = REMOTE_MSG_HEARTBEAT;

	pkt.length = sizeof(*msg);
	pkt.shroud = KYRKA_PACKET_SHROUD_CATHEDRAL;

	if (kyrka_heaven_input(remote->kyrka, &pkt) == -1) {
		vigil_log(LOG_NOTICE, "[remote] heartbeat: %d",
		    kyrka_last_error(remote->kyrka));
		remote->error = 1;
	}
}

/*
 * Send a burst of acks to covenant carrying the digest of what we got.
 */
static void
remote_ack(const u_int8_t *digest, size_t digest_len)
{
	int			i;
	size_t			len;
	struct kyrka_packet	pkt;
	struct remote_msg	*msg;

	PRECOND(digest != NULL);
	PRECOND(digest_len == sizeof(msg->data));
	PRECOND(remote != NULL);

	for (i = 0; i < 10; i++) {
		if ((msg = kyrka_packet_databuf(remote->kyrka,
		    &pkt, &len)) == NULL)
			fatal("failed to get databuf for ack");

		VERIFY(len >= sizeof(*msg));

		nyfe_mem_zero(msg, sizeof(*msg));
		msg->type = REMOTE_MSG_ACK;
		memcpy(msg->data, digest, digest_len);

		pkt.length = sizeof(*msg);
		pkt.shroud = KYRKA_PACKET_SHROUD_CATHEDRAL;

		if (kyrka_heaven_input(remote->kyrka, &pkt) == -1) {
			vigil_log(LOG_NOTICE, "[remote] ack: %d",
			    kyrka_last_error(remote->kyrka));
			remote->error = 1;
			return;
		}
	}
}

/*
 * We got our configuration from covenant. We add our cathedral secret
 * and cosk to it and make sure it is a configuration for us before we
 * let covenant know we have it.
 */
static void
remote_received(const struct remote_config *msg)
{
	struct vigil_buf	buf;
	struct nyfe_sha3	ctx;
	size_t			idx, len;
	char			cs[REMOTE_HEX_MAX];
	char			sk[REMOTE_HEX_MAX];

	PRECOND(msg != NULL);
	PRECOND(remote != NULL);

	len = be16toh(msg->length);

	if (len == 0) {
		remote->error = 1;
		return;
	}

	if (len > sizeof(msg->data)) {
		remote->error = 1;
		return;
	}

	for (idx = 0; idx < len; idx++) {
		if (msg->data[idx] == '\n')
			continue;

		if (msg->data[idx] < 0x20 || msg->data[idx] > 0x7e) {
			remote->error = 1;
			return;
		}
	}

	nyfe_zeroize_register(cs, sizeof(cs));
	nyfe_zeroize_register(sk, sizeof(sk));

	(void)sodium_bin2hex(cs, sizeof(cs), remote->cs, sizeof(remote->cs));
	(void)sodium_bin2hex(sk, sizeof(sk), remote->sk, sizeof(remote->sk));

	vigil_buf_init(&buf, remote->config, sizeof(remote->config));
	vigil_buf_appendf(&buf, "%.*s", (int)len, (const char *)msg->data);

	if (msg->data[len - 1] != '\n')
		vigil_buf_appendf(&buf, "\n");

	vigil_buf_appendf(&buf, "cathedral_secret %s\n", cs);
	vigil_buf_appendf(&buf, "cathedral_cosk %s\n", sk);

	nyfe_zeroize(cs, sizeof(cs));
	nyfe_zeroize(sk, sizeof(sk));

	if (buf.flags & VIGIL_BUF_FULL) {
		nyfe_mem_zero(remote->config, sizeof(remote->config));
		remote->error = 1;
		return;
	}

	if (remote_config_check(remote->config) == -1) {
		nyfe_mem_zero(remote->config, sizeof(remote->config));
		remote->error = 1;
		return;
	}

	nyfe_zeroize_register(&ctx, sizeof(ctx));

	nyfe_sha3_init256(&ctx);
	nyfe_sha3_update(&ctx, msg->data, len);
	nyfe_sha3_final(&ctx, remote->digest, sizeof(remote->digest));

	nyfe_zeroize(&ctx, sizeof(ctx));

	remote_ack(remote->digest, sizeof(remote->digest));

	if (remote->error == 0)
		remote->state = VIGIL_REMOTE_DONE;
}

/*
 * Callback from libkyrka when a tunnel event occurred.
 */
static void
remote_event(KYRKA *ctx, union kyrka_event *evt, void *udata)
{
	PRECOND(ctx != NULL);
	PRECOND(evt != NULL);
	PRECOND(udata == NULL);
	PRECOND(remote != NULL);

	switch (evt->type) {
	case KYRKA_EVENT_KEYS_INFO:
		if (remote->state != VIGIL_REMOTE_WAITING)
			break;
		if (evt->keys.tx_spi != 0 && evt->keys.rx_spi != 0)
			remote->state = VIGIL_REMOTE_ONLINE;
		break;
	case KYRKA_EVENT_PEER_DISCOVERY:
		remote->peer.sin_port = evt->peer.port;
		remote->peer.sin_addr.s_addr = evt->peer.ip;
		break;
	default:
		break;
	}
}

/*
 * Callback from libkyrka when there is plaintext from covenant, either
 * a heartbeat or our configuration.
 */
static void
remote_heaven_ifc(struct kyrka_packet *pkt, u_int64_t magic, void *udata)
{
	const struct remote_msg		*msg;

	PRECOND(pkt != NULL);
	PRECOND(udata == NULL);
	PRECOND(remote != NULL);

	(void)magic;

	if (remote->state != VIGIL_REMOTE_ONLINE)
		return;

	if (pkt->length < sizeof(*msg)) {
		remote->error = 1;
		return;
	}

	msg = kyrka_packet_data(pkt);

	switch (msg->type) {
	case REMOTE_MSG_HEARTBEAT:
		if (pkt->length != sizeof(*msg))
			remote->error = 1;
		break;
	case REMOTE_MSG_CONFIG:
		if (pkt->length != sizeof(struct remote_config)) {
			remote->error = 1;
			break;
		}
		remote_received(kyrka_packet_data(pkt));
		break;
	default:
		remote->error = 1;
		break;
	}
}

/*
 * Callback from libkyrka when there is ciphertext for covenant.
 */
static void
remote_purgatory_ifc(struct kyrka_packet *pkt, u_int64_t magic, void *udata)
{
	size_t		len;
	void		*data;

	PRECOND(pkt != NULL);
	PRECOND(udata == NULL);
	PRECOND(remote != NULL);

	(void)magic;

	if ((data = kyrka_packet_sendbuf(remote->kyrka, pkt, &len)) == NULL)
		fatal("failed to get remote send buffer");

	if (sendto(remote->fd, data, len, 0,
	    (const struct sockaddr *)&remote->peer,
	    sizeof(remote->peer)) == -1) {
		if (errno != EAGAIN && errno != EWOULDBLOCK &&
		    !vigil_socket_unreachable(errno, 0)) {
			vigil_log(LOG_INFO, "[remote] sendto: %s", errno_s);
		}
	}
}

/*
 * Callback from libkyrka when there is ciphertext for the cathedral.
 */
static void
remote_cathedral_ifc(struct kyrka_packet *pkt, u_int64_t magic, void *udata)
{
	size_t		len;
	void		*data;

	PRECOND(pkt != NULL);
	PRECOND(udata == NULL);
	PRECOND(remote != NULL);

	(void)magic;

	if ((data = kyrka_packet_sendbuf(remote->kyrka, pkt, &len)) == NULL)
		fatal("failed to get remote cathedral send buffer");

	if (sendto(remote->fd, data, len, 0,
	    (const struct sockaddr *)&remote->cathedral,
	    sizeof(remote->cathedral)) == -1) {
		if (errno != EAGAIN && errno != EWOULDBLOCK &&
		    !vigil_socket_unreachable(errno, 0)) {
			vigil_log(LOG_INFO,
			    "[remote] sendto: %s (cathedral)", errno_s);
		}
	}
}
