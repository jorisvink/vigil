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
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

#include "vigil.h"

#include <libkyrka/libnyfe.h>

/*
 * The configuration parser state.
 */
struct config_parser {
	struct vigil_config	*cfg;
	int			version;

	char			*err;
	size_t			errlen;
};

/*
 * A configuration edit.
 */
struct config_edit {
	char			*out;
	size_t			outlen;
	size_t			offset;

	char			*err;
	size_t			errlen;
};

static int	config_validate(struct config_parser *);
static int	config_error(struct config_parser *, const char *, ...)
		    __attribute__((format (printf, 2, 3)));

static int	config_validate_mtu(struct config_parser *);
static int	config_validate_tier6(struct config_parser *);
static int	config_validate_cathedral(struct config_parser *);

static int	config_parse_dns(struct config_parser *, char *);
static int	config_parse_kek(struct config_parser *, char *);
static int	config_parse_mtu(struct config_parser *, char *);
static int	config_parse_p2p(struct config_parser *, char *);
static int	config_parse_spi(struct config_parser *, char *);
static int	config_parse_mode(struct config_parser *, char *);
static int	config_parse_peer(struct config_parser *, char *);
static int	config_parse_route(struct config_parser *, char *);
static int	config_parse_tier6(struct config_parser *, char *);
static int	config_parse_shroud(struct config_parser *, char *);
static int	config_parse_tunnel(struct config_parser *, char *);
static int	config_parse_secret(struct config_parser *, char *);
static int	config_parse_instance(struct config_parser *, char *);
static int	config_parse_cathedral(struct config_parser *, char *);
static int	config_parse_block_ipv6(struct config_parser *, char *);
static int	config_parse_cathedral_id(struct config_parser *, char *);
static int	config_parse_liturgy_group(struct config_parser *, char *);
static int	config_parse_cathedral_cosk(struct config_parser *, char *);
static int	config_parse_liturgy_prefix(struct config_parser *, char *);
static int	config_parse_cathedral_flock(struct config_parser *, char *);
static int	config_parse_cathedral_secret(struct config_parser *, char *);
static int	config_parse_number(const char *, int, u_int64_t, u_int64_t *);
static int	config_parse_cathedral_nat_port(struct config_parser *, char *);
static int	config_parse_cathedral_flock_dst(struct config_parser *,
		    char *);
static int	config_parse_liturgy_discoverable(struct config_parser *,
		    char *);
static int	config_parse_cathedral_remembrance(struct config_parser *,
		    char *);
static int	config_parse_yesno(struct config_parser *, const char *, char *,
		    u_int32_t, int);
static int	config_parse_host(struct config_parser *, const char *, char *,
		    char *, size_t, u_int16_t *);
static int	config_parse_hex(struct config_parser *, const char *, char *,
		    u_int8_t *, size_t, u_int32_t);
static int	config_parse_ip_mask(struct config_parser *, const char *,
		    char *, struct in_addr *, u_int8_t *);

static int	config_edit_option(const char *);
static int	config_edit_copy(struct config_edit *, const char *, size_t,
		    int);
static int	config_edit_append(struct config_edit *, const char *,
		    const char *);

static void	config_json_bool(struct vigil_buf *, const char *, int);

static struct {
	const char	*name;
	int		(*parse)(struct config_parser *, char *);
} opts[] = {
	{ "dns",			config_parse_dns },
	{ "kek",			config_parse_kek },
	{ "mtu",			config_parse_mtu },
	{ "p2p",			config_parse_p2p },
	{ "spi",			config_parse_spi },
	{ "mode",			config_parse_mode },
	{ "peer",			config_parse_peer },
	{ "route",			config_parse_route },
	{ "tier6",			config_parse_tier6 },
	{ "shroud",			config_parse_shroud },
	{ "tunnel",			config_parse_tunnel },
	{ "secret",			config_parse_secret },
	{ "instance",			config_parse_instance },
	{ "cathedral",			config_parse_cathedral },
	{ "block_ipv6",			config_parse_block_ipv6 },
	{ "liturgy_group",		config_parse_liturgy_group },
	{ "liturgy_prefix",		config_parse_liturgy_prefix },
	{ "liturgy_discoverable",	config_parse_liturgy_discoverable },
	{ "cathedral_id",		config_parse_cathedral_id },
	{ "cathedral_cosk",		config_parse_cathedral_cosk },
	{ "cathedral_flock",		config_parse_cathedral_flock },
	{ "cathedral_secret",		config_parse_cathedral_secret },
	{ "cathedral_nat_port",		config_parse_cathedral_nat_port },
	{ "cathedral_flock_dst",	config_parse_cathedral_flock_dst },
	{ "cathedral_remembrance",	config_parse_cathedral_remembrance },
	{ NULL, NULL },
};

/*
 * Parse the given configuration, returning a new vigil_config
 * on success or NULL on failure.
 */
struct vigil_config *
vigil_config_parse(const char *data, size_t datalen, char *err, size_t errlen)
{
	struct config_parser	cp;
	size_t			len;
	u_int64_t		version;
	int			idx, lineno;
	char			*buf, *line, *next, *opt;

	PRECOND(data != NULL);
	PRECOND(err != NULL);
	PRECOND(errlen > 0);

	err[0] = '\0';

	memset(&cp, 0, sizeof(cp));
	cp.err = err;
	cp.errlen = errlen;

	if (datalen == 0 || datalen > VIGIL_CONFIG_MAX) {
		(void)config_error(&cp, "configuration has an invalid size");
		return (NULL);
	}

	if ((cp.cfg = calloc(1, sizeof(*cp.cfg))) == NULL)
		fatal("calloc: failed to allocate configuration");

	if ((buf = calloc(1, datalen + 1)) == NULL)
		fatal("calloc: failed to allocate configuration buffer");

	memcpy(buf, data, datalen);

	cp.cfg->mode = VIGIL_MODE_TUNNEL;
	cp.cfg->liturgy_group = VIGIL_LITURGY_GROUP_DEFAULT;

	cp.cfg->flags = VIGIL_CONFIG_FLAG_P2P |
	    VIGIL_CONFIG_FLAG_BLOCK_IPV6 |
	    VIGIL_CONFIG_FLAG_REMEMBRANCE;

	lineno = 0;

	for (line = buf; line != NULL; line = next) {
		lineno++;

		next = strchr(line, '\n');
		if (next != NULL)
			*(next)++ = '\0';

		line[strcspn(line, "\r")] = '\0';

		while (isspace((unsigned char)*line))
			line++;

		len = strlen(line);

		for (idx = len - 1; idx >= 0; idx--) {
			if (!isspace((unsigned char)line[idx]))
				break;
			line[idx] = '\0';
		}

		if (line[0] == '#' || line[0] == '\0')
			continue;

		opt = strchr(line, ' ');
		if (opt == NULL && cp.version == 0) {
			(void)config_error(&cp, "not a vigil configuration");
			goto cleanup;
		}

		if (opt == NULL) {
			(void)config_error(&cp,
			    "line %d: option '%s' is missing a value",
			    lineno, line);
			goto cleanup;
		}

		*(opt)++ = '\0';

		while (isspace((unsigned char)*opt))
			opt++;

		if (cp.version == 0) {
			if (strcmp(line, "vigil") ||
			    config_parse_number(opt, 10, INT_MAX,
			    &version) == -1 || version == 0) {
				(void)config_error(&cp,
				    "not a vigil configuration");
				goto cleanup;
			}

			cp.version = version;

			if (cp.version != VIGIL_CONFIG_VERSION) {
				(void)config_error(&cp,
				    "unsupported configuration version %d",
				    cp.version);
				goto cleanup;
			}

			continue;
		}

		for (idx = 0; opts[idx].name != NULL; idx++) {
			if (!strcmp(opts[idx].name, line))
				break;
		}

		if (opts[idx].name == NULL) {
			(void)config_error(&cp,
			    "line %d: unknown option '%s'", lineno, line);
			goto cleanup;
		}

		if (opts[idx].parse(&cp, opt) == -1)
			goto cleanup;
	}

	if (cp.version == 0) {
		(void)config_error(&cp, "not a vigil configuration");
		goto cleanup;
	}

	if (config_validate(&cp) == -1)
		goto cleanup;

	nyfe_mem_zero(buf, datalen);
	free(buf);

	return (cp.cfg);

cleanup:
	nyfe_mem_zero(buf, datalen);
	free(buf);

	vigil_config_free(cp.cfg);

	return (NULL);
}

/*
 * Parse the given configuration and write either its json summary
 * or a json object containing the parse error into out.
 *
 * Returns 0 if the configuration is valid, -1 otherwise.
 */
int
vigil_config_check(const char *data, size_t len, char *out, size_t outlen)
{
	const char		*p;
	struct vigil_config	*cfg;
	char			err[128];

	PRECOND(data != NULL);
	PRECOND(out != NULL);
	PRECOND(outlen > 0);

	cfg = vigil_config_parse(data, len, err, sizeof(err));
	if (cfg == NULL) {
		for (p = err; *p != '\0'; p++) {
			if (*p == '"' || *p == '\\' ||
			    (unsigned char)*p < 0x20 ||
			    (unsigned char)*p > 0x7e)
				break;
		}

		if (*p != '\0') {
			(void)snprintf(err, sizeof(err),
			    "invalid configuration");
		}

		(void)snprintf(out, outlen, "{\"error\":\"%s\"}", err);
		return (-1);
	}

	if (vigil_config_summary(cfg, out, outlen) == -1) {
		(void)snprintf(out, outlen,
		    "{\"error\":\"summary too large\"}");
		vigil_config_free(cfg);
		return (-1);
	}

	vigil_config_free(cfg);

	return (0);
}

/*
 * Replace the dns, instance, p2p, route and tier6 options in the given
 * configuration with those in edits, the result is validated and written
 * into out. Options not given in edits go back to their defaults, the
 * instance has none and must always be given.
 *
 * The result holds key material, the caller must wipe it.
 *
 * Returns 0 on success or -1 on failure, with the reason in err.
 */
int
vigil_config_edit(const char *data, size_t datalen, const char *edits,
    size_t editslen, char *out, size_t outlen, char *err, size_t errlen)
{
	struct config_edit	ce;
	struct vigil_config	*cfg;

	PRECOND(data != NULL);
	PRECOND(edits != NULL);
	PRECOND(out != NULL);
	PRECOND(outlen > 0);
	PRECOND(err != NULL);
	PRECOND(errlen > 0);

	err[0] = '\0';
	out[0] = '\0';

	memset(&ce, 0, sizeof(ce));
	ce.out = out;
	ce.outlen = outlen;
	ce.err = err;
	ce.errlen = errlen;

	if (config_edit_copy(&ce, data, datalen, 0) == -1)
		goto cleanup;

	if (config_edit_copy(&ce, edits, editslen, 1) == -1)
		goto cleanup;

	cfg = vigil_config_parse(out, ce.offset, err, errlen);
	if (cfg == NULL)
		goto cleanup;

	vigil_config_free(cfg);

	return (0);

cleanup:
	nyfe_mem_zero(out, outlen);

	return (-1);
}

/*
 * Cleanup and free a configuration.
 */
void
vigil_config_free(struct vigil_config *cfg)
{
	if (cfg == NULL)
		return;

	nyfe_mem_zero(cfg, sizeof(*cfg));
	free(cfg);
}

/*
 * Returns a human readable name for the mode of the configuration.
 */
const char *
vigil_config_mode(const struct vigil_config *cfg)
{
	const char	*mode;

	PRECOND(cfg != NULL);

	switch (cfg->mode) {
	case VIGIL_MODE_TUNNEL:
		mode = "tunnel";
		break;
	case VIGIL_MODE_CATHEDRAL:
		mode = "cathedral";
		break;
	case VIGIL_MODE_LITURGY:
		mode = "liturgy";
		break;
	case VIGIL_MODE_GOSPEL:
		mode = "gospel";
		break;
	default:
		mode = "unknown";
		break;
	}

	return (mode);
}

/*
 * Write a json summary of the given configuration into buf, ignoring
 * the key material.
 *
 * Returns 0 on success or -1 if the buffer was too small.
 */
int
vigil_config_summary(const struct vigil_config *cfg, char *buf, size_t len)
{
	struct in_addr		ip;
	int			idx;
	struct vigil_buf	out;
	char			str[INET_ADDRSTRLEN];

	PRECOND(cfg != NULL);
	PRECOND(buf != NULL);
	PRECOND(len > 0);

	vigil_buf_init(&out, buf, len);

	vigil_buf_appendf(&out, "{\"version\":%d,", VIGIL_CONFIG_VERSION);
	vigil_buf_appendf(&out, "\"instance\":\"%s\",", cfg->instance);
	vigil_buf_appendf(&out, "\"mode\":\"%s\",", vigil_config_mode(cfg));
	vigil_buf_appendf(&out, "\"mtu\":%u,", cfg->mtu);
	config_json_bool(&out, "mtu_discovery",
	    cfg->flags & VIGIL_CONFIG_FLAG_MTU_DISCOVERY);

	if (cfg->mode == VIGIL_MODE_LITURGY &&
	    !(cfg->flags & VIGIL_CONFIG_FLAG_TIER6)) {
		vigil_inet_liturgy(&ip, &cfg->liturgy_prefix,
		    cfg->spi & 0xff, cfg->spi & 0xff);
		(void)inet_ntop(AF_INET, &ip, str, sizeof(str));
		vigil_buf_appendf(&out, "\"address\":\"%s/16\",", str);
	} else if (cfg->mode != VIGIL_MODE_GOSPEL) {
		(void)inet_ntop(AF_INET, &cfg->ip, str, sizeof(str));
		vigil_buf_appendf(&out, "\"address\":\"%s/%u\",",
		    str, cfg->tun_prefix);
	}

	vigil_buf_appendf(&out, "\"routes\":[");

	for (idx = 0; idx < cfg->route_count; idx++) {
		(void)inet_ntop(AF_INET, &cfg->routes[idx].net,
		    str, sizeof(str));
		vigil_buf_appendf(&out, "%s\"%s/%u\"",
		    idx > 0 ? "," : "", str, cfg->routes[idx].prefix);
	}

	vigil_buf_appendf(&out, "],");
	vigil_buf_appendf(&out, "\"dns\":[");

	for (idx = 0; idx < cfg->dns_count; idx++) {
		(void)inet_ntop(AF_INET, &cfg->dns[idx], str, sizeof(str));
		vigil_buf_appendf(&out, "%s\"%s\"", idx > 0 ? "," : "", str);
	}

	vigil_buf_appendf(&out, "],");

	if (cfg->mode == VIGIL_MODE_TUNNEL) {
		vigil_buf_appendf(&out, "\"peer\":\"%s:%u\",",
		    cfg->peer_host, cfg->peer_port);
	} else {
		vigil_buf_appendf(&out, "\"cathedral\":\"%s:%u\",",
		    cfg->cathedral_host, cfg->cathedral_port);
		vigil_buf_appendf(&out, "\"cathedral_nat_port\":%u,",
		    cfg->cathedral_nat_port);
		vigil_buf_appendf(&out, "\"cathedral_id\":\"%08x\",",
		    cfg->cathedral_id);
		vigil_buf_appendf(&out,
		    "\"cathedral_flock\":\"%016" PRIx64 "\",",
		    cfg->cathedral_flock);
		vigil_buf_appendf(&out, "\"spi\":\"%04x\",", cfg->spi);
		config_json_bool(&out, "p2p",
		    cfg->flags & VIGIL_CONFIG_FLAG_P2P);
		config_json_bool(&out, "remembrance",
		    cfg->flags & VIGIL_CONFIG_FLAG_REMEMBRANCE);
	}

	if (cfg->mode == VIGIL_MODE_LITURGY) {
		if (!(cfg->flags & VIGIL_CONFIG_FLAG_TIER6)) {
			(void)inet_ntop(AF_INET, &cfg->liturgy_prefix,
			    str, sizeof(str));
			vigil_buf_appendf(&out,
			    "\"liturgy_prefix\":\"%s\",", str);
		}

		config_json_bool(&out, "tier6",
		    cfg->flags & VIGIL_CONFIG_FLAG_TIER6);
		vigil_buf_appendf(&out, "\"liturgy_group\":\"%04x\",",
		    cfg->liturgy_group);
		config_json_bool(&out, "liturgy_discoverable",
		    !(cfg->flags & VIGIL_CONFIG_FLAG_HIDDEN));
	}

	if (cfg->mode == VIGIL_MODE_GOSPEL) {
		vigil_buf_appendf(&out, "\"liturgy_group\":\"%04x\",",
		    cfg->liturgy_group);
	}

	config_json_bool(&out, "block_ipv6",
	    cfg->flags & VIGIL_CONFIG_FLAG_BLOCK_IPV6);
	vigil_buf_appendf(&out, "\"shroud\":%s}",
	    (cfg->flags & VIGIL_CONFIG_FLAG_SHROUD) ? "true" : "false");

	if (out.flags & VIGIL_BUF_FULL)
		return (-1);

	return (0);
}

/*
 * Validate a parsed configuration, reject invalid ones.
 */
static int
config_validate(struct config_parser *cp)
{
	int			ret;
	struct vigil_config	*cfg;

	PRECOND(cp != NULL);
	PRECOND(cp->cfg != NULL);

	cfg = cp->cfg;

	if (cfg->instance[0] == '\0')
		return (config_error(cp, "no instance was configured"));

	if (cfg->mode == VIGIL_MODE_TUNNEL &&
	    cfg->cathedral_host[0] != '\0')
		cfg->mode = VIGIL_MODE_CATHEDRAL;

	switch (cfg->mode) {
	case VIGIL_MODE_TUNNEL:
		if (cfg->peer_host[0] == '\0')
			return (config_error(cp, "no peer was configured"));

		if (!(cfg->keys & VIGIL_CONFIG_KEY_SECRET))
			return (config_error(cp, "no secret was configured"));

		if (cfg->keys & ~VIGIL_CONFIG_KEY_SECRET) {
			ret = config_error(cp,
			    "cathedral keys given without a cathedral");
			return (ret);
		}

		if (cfg->ip.s_addr == 0)
			return (config_error(cp, "no tunnel was configured"));
		break;
	case VIGIL_MODE_CATHEDRAL:
		if (cfg->peer_host[0] != '\0') {
			ret = config_error(cp,
			    "peer cannot be combined with a cathedral");
			return (ret);
		}

		if (config_validate_cathedral(cp) == -1)
			return (-1);

		if ((cfg->spi >> 8) == 0 || (cfg->spi & 0xff) == 0 ||
		    (cfg->spi >> 8) == (cfg->spi & 0xff))
			return (config_error(cp, "spi must be <src><dst>"));

		if (cfg->ip.s_addr == 0)
			return (config_error(cp, "no tunnel was configured"));
		break;
	case VIGIL_MODE_LITURGY:
		if (cfg->peer_host[0] != '\0') {
			ret = config_error(cp, "peer is not used in liturgy");
			return (ret);
		}

		if (cfg->flags & VIGIL_CONFIG_FLAG_TIER6)
			return (config_validate_tier6(cp));

		if (cfg->ip.s_addr != 0) {
			ret = config_error(cp, "tunnel is not used in "
			    "liturgy, see liturgy_prefix");
			return (ret);
		}

		if (cfg->route_count != 0) {
			ret = config_error(cp, "route is not used in "
			    "liturgy, see liturgy_prefix");
			return (ret);
		}

		if (config_validate_cathedral(cp) == -1)
			return (-1);

		if (cfg->spi == 0 || cfg->spi >= 0xff)
			return (config_error(cp, "spi must be between 01-fe"));

		if (cfg->liturgy_prefix.s_addr == 0) {
			ret = config_error(cp,
			    "no liturgy_prefix was configured");
			return (ret);
		}

		cfg->route_count = 1;
		cfg->routes[0].prefix = 16;
		cfg->routes[0].net = cfg->liturgy_prefix;
		break;
	case VIGIL_MODE_GOSPEL:
		if (cfg->peer_host[0] != '\0') {
			ret = config_error(cp, "peer is not used for gospel");
			return (ret);
		}

		if (cfg->ip.s_addr != 0) {
			ret = config_error(cp, "tunnel is not used for gospel");
			return (ret);
		}

		if (cfg->route_count != 0) {
			ret = config_error(cp, "route is not used for gospel");
			return (ret);
		}

		if (cfg->dns_count != 0)
			return (config_error(cp, "dns is not used for gospel"));

		if (cfg->liturgy_prefix.s_addr != 0) {
			ret = config_error(cp,
			    "liturgy_prefix is not used for gospel");
			return (ret);
		}

		if (cfg->flags & VIGIL_CONFIG_FLAG_TIER6) {
			ret = config_error(cp, "tier6 is not used for gospel");
			return (ret);
		}

		if (!(cfg->flags & VIGIL_CONFIG_FLAG_REMEMBRANCE)) {
			ret = config_error(cp,
			    "cathedral_remembrance is always on for gospel");
			return (ret);
		}

		if (config_validate_cathedral(cp) == -1)
			return (-1);

		if (cfg->spi == 0 || cfg->spi >= 0xff)
			return (config_error(cp, "spi must be between 01-fe"));

		if (cfg->liturgy_group == USHRT_MAX) {
			ret = config_error(cp,
			    "liturgy_group must be below ffff for gospel");
			return (ret);
		}
		break;
	default:
		return (config_error(cp, "unknown mode %d", cfg->mode));
	}

	return (config_validate_mtu(cp));
}

/*
 * Validate a tier6 configuration and reject if needed.
 */
static int
config_validate_tier6(struct config_parser *cp)
{
	struct vigil_config	*cfg;
	u_int32_t		mask;

	PRECOND(cp != NULL);

	cfg = cp->cfg;

	if (cfg->route_count != 0)
		return (config_error(cp, "route is not used with tier6"));

	if (cfg->flags & VIGIL_CONFIG_FLAG_MTU_DISCOVERY)
		return (config_error(cp, "mtu auto is not used with tier6"));

	if (config_validate_cathedral(cp) == -1)
		return (-1);

	if (cfg->spi == 0 || cfg->spi >= 0xff)
		return (config_error(cp, "spi must be between 01-fe"));

	if (cfg->ip.s_addr == 0) {
		cfg->tun_prefix = 24;
		vigil_inet_tier6(&cfg->ip,
		    cfg->cathedral_flock, cfg->spi & 0xff);
	}

	if (cfg->tun_prefix == 0 || cfg->tun_prefix > 30)
		return (config_error(cp, "tunnel mask must be between 1-30"));

	mask = 0xffffffff << (32 - cfg->tun_prefix);

	cfg->route_count = 1;
	cfg->routes[0].prefix = cfg->tun_prefix;

	cfg->routes[0].net.s_addr = ntohl(cfg->ip.s_addr);
	cfg->routes[0].net.s_addr &= mask;
	cfg->routes[0].net.s_addr = htonl(cfg->routes[0].net.s_addr);

	return (config_validate_mtu(cp));
}

/*
 * Check that all required options for talking to a cathedral are present.
 */
static int
config_validate_cathedral(struct config_parser *cp)
{
	int			ret;
	struct vigil_config	*cfg;

	PRECOND(cp != NULL);

	cfg = cp->cfg;

	if (cfg->cathedral_host[0] == '\0')
		return (config_error(cp, "no cathedral was configured"));

	if (cfg->keys & VIGIL_CONFIG_KEY_SECRET) {
		ret = config_error(cp,
		    "secret cannot be combined with a cathedral");
		return (ret);
	}

	if (cfg->cathedral_id == 0)
		return (config_error(cp, "no cathedral_id was configured"));

	if (cfg->cathedral_flock == 0)
		return (config_error(cp, "no cathedral_flock was configured"));

	if (!(cfg->keys & VIGIL_CONFIG_KEY_KEK))
		return (config_error(cp, "no kek was configured"));

	if (!(cfg->keys & VIGIL_CONFIG_KEY_CS))
		return (config_error(cp, "no cathedral_secret was configured"));

	if (!(cfg->keys & VIGIL_CONFIG_KEY_COSK))
		return (config_error(cp, "no cathedral_cosk was configured"));

	if (cfg->cathedral_nat_port == 0) {
		if (cfg->cathedral_port == USHRT_MAX)
			return (config_error(cp, "cannot derive nat port"));

		cfg->cathedral_nat_port = cfg->cathedral_port + 1;
	}

	return (0);
}

/*
 * Validate the configured MTU.
 */
static int
config_validate_mtu(struct config_parser *cp)
{
	int			ret;
	struct vigil_config	*cfg;
	u_int16_t		limit;

	PRECOND(cp != NULL);

	cfg = cp->cfg;

	if (cfg->flags & VIGIL_CONFIG_FLAG_SHROUD)
		limit = VIGIL_MTU_SHROUD;
	else
		limit = VIGIL_MTU_DEFAULT;

	if (cfg->mtu == 0) {
		cfg->mtu = limit;
		return (0);
	}

	if (cfg->mtu > limit) {
		ret = config_error(cp, "mtu may not be higher than %u", limit);
		return (ret);
	}

	if (cfg->mtu < KYRKA_MTU_MINIMUM) {
		ret = config_error(cp,
		    "mtu may not be lower than %d", KYRKA_MTU_MINIMUM);
		return (ret);
	}

	return (0);
}

/*
 * Parse the instance option.
 */
static int
config_parse_instance(struct config_parser *cp, char *opt)
{
	int		ret;
	size_t		idx, len;

	PRECOND(cp != NULL);
	PRECOND(opt != NULL);

	if (cp->cfg->instance[0] != '\0')
		return (config_error(cp, "instance already specified"));

	len = strlen(opt);
	if (len == 0 || len >= sizeof(cp->cfg->instance)) {
		ret = config_error(cp, "instance name has an invalid length");
		return (ret);
	}

	for (idx = 0; idx < len; idx++) {
		if (!isalnum((unsigned char)opt[idx]) && opt[idx] != '-' &&
		    opt[idx] != '_' && opt[idx] != '.' && opt[idx] != ' ')
			return (config_error(cp, "instance name is invalid"));
	}

	memcpy(cp->cfg->instance, opt, len + 1);

	return (0);
}

/*
 * Parse the mode configuration option.
 */
static int
config_parse_mode(struct config_parser *cp, char *opt)
{
	int		ret;

	PRECOND(cp != NULL);
	PRECOND(opt != NULL);

	if (!strcmp(opt, "tunnel")) {
		cp->cfg->mode = VIGIL_MODE_TUNNEL;
	} else if (!strcmp(opt, "liturgy")) {
		cp->cfg->mode = VIGIL_MODE_LITURGY;
	} else if (!strcmp(opt, "gospel") || !strcmp(opt, "voice")) {
		cp->cfg->mode = VIGIL_MODE_GOSPEL;
	} else {
		ret = config_error(cp,
		    "invalid mode '%s', accepted: tunnel|liturgy|gospel", opt);
		return (ret);
	}

	return (0);
}

/*
 * Parse the tunnel configuration option.
 */
static int
config_parse_tunnel(struct config_parser *cp, char *opt)
{
	char		*p;
	int		ret;
	u_int64_t	mtu;

	PRECOND(cp != NULL);
	PRECOND(opt != NULL);

	if (cp->cfg->ip.s_addr != 0)
		return (config_error(cp, "tunnel already specified"));

	p = strchr(opt, ' ');
	if (p != NULL) {
		*(p)++ = '\0';

		while (isspace((unsigned char)*p))
			p++;

		if (!strcmp(p, "auto")) {
			cp->cfg->flags |= VIGIL_CONFIG_FLAG_MTU_DISCOVERY;
		} else if (config_parse_number(p, 10, USHRT_MAX, &mtu) == 0) {
			cp->cfg->mtu = mtu;
		} else {
			ret = config_error(cp, "tunnel <ip/mask> [mtu|auto]");
			return (ret);
		}
	}

	if (config_parse_ip_mask(cp, "tunnel",
	    opt, &cp->cfg->ip, &cp->cfg->tun_prefix) == -1)
		return (-1);

	if (cp->cfg->ip.s_addr == 0)
		return (config_error(cp, "tunnel ip may not be 0.0.0.0"));

	return (0);
}

/*
 * Parse the mtu configuration option, auto turns on MTU discovery
 * using graces, starting from the largest MTU we support.
 */
static int
config_parse_mtu(struct config_parser *cp, char *opt)
{
	u_int64_t	mtu;

	PRECOND(cp != NULL);
	PRECOND(opt != NULL);

	if (!strcmp(opt, "auto")) {
		cp->cfg->mtu = 0;
		cp->cfg->flags |= VIGIL_CONFIG_FLAG_MTU_DISCOVERY;
		return (0);
	}

	if (config_parse_number(opt, 10, USHRT_MAX, &mtu) == -1)
		return (config_error(cp, "mtu <int|auto>"));

	cp->cfg->mtu = mtu;

	return (0);
}

/*
 * Parse the route configuration option.
 */
static int
config_parse_route(struct config_parser *cp, char *opt)
{
	struct vigil_route	*rt;

	PRECOND(cp != NULL);
	PRECOND(opt != NULL);

	if (cp->cfg->route_count >= VIGIL_ROUTES_MAX)
		return (config_error(cp, "too many routes"));

	rt = &cp->cfg->routes[cp->cfg->route_count];

	if (config_parse_ip_mask(cp, "route", opt, &rt->net, &rt->prefix) == -1)
		return (-1);

	if (rt->prefix == 0)
		rt->net.s_addr = 0;
	else
		rt->net.s_addr &= htonl(0xffffffff << (32 - rt->prefix));

	cp->cfg->route_count++;

	return (0);
}

/*
 * Parse the dns configuration option.
 */
static int
config_parse_dns(struct config_parser *cp, char *opt)
{
	PRECOND(cp != NULL);
	PRECOND(opt != NULL);

	if (cp->cfg->dns_count >= VIGIL_DNS_MAX)
		return (config_error(cp, "too many dns servers"));

	if (inet_pton(AF_INET, opt,
	    &cp->cfg->dns[cp->cfg->dns_count]) != 1)
		return (config_error(cp, "dns <ipv4 address>"));

	cp->cfg->dns_count++;

	return (0);
}

/*
 * Parse the peer configuration option.
 */
static int
config_parse_peer(struct config_parser *cp, char *opt)
{
	int		ret;

	PRECOND(cp != NULL);
	PRECOND(opt != NULL);

	ret = config_parse_host(cp, "peer", opt, cp->cfg->peer_host,
	    sizeof(cp->cfg->peer_host), &cp->cfg->peer_port);

	return (ret);
}

/*
 * Parse the secret configuration option.
 */
static int
config_parse_secret(struct config_parser *cp, char *opt)
{
	int		ret;

	PRECOND(cp != NULL);
	PRECOND(opt != NULL);

	ret = config_parse_hex(cp, "secret", opt, cp->cfg->secret,
	    sizeof(cp->cfg->secret), VIGIL_CONFIG_KEY_SECRET);

	return (ret);
}

/*
 * Parse the spi configuration option.
 */
static int
config_parse_spi(struct config_parser *cp, char *opt)
{
	u_int64_t	spi;

	PRECOND(cp != NULL);
	PRECOND(opt != NULL);

	if (config_parse_number(opt, 16, USHRT_MAX, &spi) == -1)
		return (config_error(cp, "spi <16-bit hex value>"));

	cp->cfg->spi = spi;

	return (0);
}

/*
 * Parse the kek configuration option.
 */
static int
config_parse_kek(struct config_parser *cp, char *opt)
{
	int		ret;

	PRECOND(cp != NULL);
	PRECOND(opt != NULL);

	ret = config_parse_hex(cp, "kek", opt, cp->cfg->kek,
	    sizeof(cp->cfg->kek), VIGIL_CONFIG_KEY_KEK);

	return (ret);
}

/*
 * Parse the shroud configuration option.
 */
static int
config_parse_shroud(struct config_parser *cp, char *opt)
{
	int		ret;

	PRECOND(cp != NULL);
	PRECOND(opt != NULL);

	ret = config_parse_yesno(cp, "shroud", opt,
	    VIGIL_CONFIG_FLAG_SHROUD, 0);

	return (ret);
}

/*
 * Parse the p2p configuration option.
 */
static int
config_parse_p2p(struct config_parser *cp, char *opt)
{
	PRECOND(cp != NULL);
	PRECOND(opt != NULL);

	return (config_parse_yesno(cp, "p2p", opt, VIGIL_CONFIG_FLAG_P2P, 0));
}

/*
 * Parse the tier6 configuration option.
 */
static int
config_parse_tier6(struct config_parser *cp, char *opt)
{
	int		ret;

	PRECOND(cp != NULL);
	PRECOND(opt != NULL);

	ret = config_parse_yesno(cp, "tier6", opt, VIGIL_CONFIG_FLAG_TIER6, 0);

	return (ret);
}

/*
 * Parse the block_ipv6 configuration option.
 */
static int
config_parse_block_ipv6(struct config_parser *cp, char *opt)
{
	int		ret;

	PRECOND(cp != NULL);
	PRECOND(opt != NULL);

	ret = config_parse_yesno(cp, "block_ipv6", opt,
	    VIGIL_CONFIG_FLAG_BLOCK_IPV6, 0);

	return (ret);
}

/*
 * Parse the cathedral configuration option.
 */
static int
config_parse_cathedral(struct config_parser *cp, char *opt)
{
	int		ret;

	PRECOND(cp != NULL);
	PRECOND(opt != NULL);

	ret = config_parse_host(cp, "cathedral", opt,
	    cp->cfg->cathedral_host, sizeof(cp->cfg->cathedral_host),
	    &cp->cfg->cathedral_port);

	return (ret);
}

/*
 * Parse the cathedral_id configuration option.
 */
static int
config_parse_cathedral_id(struct config_parser *cp, char *opt)
{
	u_int64_t	id;

	PRECOND(cp != NULL);
	PRECOND(opt != NULL);

	if (config_parse_number(opt, 16, UINT_MAX, &id) == -1)
		return (config_error(cp, "cathedral_id <32-bit hex value>"));

	cp->cfg->cathedral_id = id;

	return (0);
}

/*
 * Parse the cathedral_secret configuration option.
 */
static int
config_parse_cathedral_secret(struct config_parser *cp, char *opt)
{
	int		ret;

	PRECOND(cp != NULL);
	PRECOND(opt != NULL);

	ret = config_parse_hex(cp, "cathedral_secret", opt,
	    cp->cfg->cathedral_secret, sizeof(cp->cfg->cathedral_secret),
	    VIGIL_CONFIG_KEY_CS);

	return (ret);
}

/*
 * Parse the cathedral_cosk configuration option.
 */
static int
config_parse_cathedral_cosk(struct config_parser *cp, char *opt)
{
	int		ret;

	PRECOND(cp != NULL);
	PRECOND(opt != NULL);

	ret = config_parse_hex(cp, "cathedral_cosk", opt,
	    cp->cfg->cathedral_cosk, sizeof(cp->cfg->cathedral_cosk),
	    VIGIL_CONFIG_KEY_COSK);

	return (ret);
}

/*
 * Parse the cathedral_flock configuration option.
 */
static int
config_parse_cathedral_flock(struct config_parser *cp, char *opt)
{
	PRECOND(cp != NULL);
	PRECOND(opt != NULL);

	if (config_parse_number(opt, 16, UINT64_MAX,
	    &cp->cfg->cathedral_flock) == -1)
		return (config_error(cp, "cathedral_flock <64-bit hex value>"));

	return (0);
}

/*
 * Parse the cathedral_flock_dst configuration option.
 */
static int
config_parse_cathedral_flock_dst(struct config_parser *cp, char *opt)
{
	int		ret;

	PRECOND(cp != NULL);
	PRECOND(opt != NULL);

	if (config_parse_number(opt, 16, UINT64_MAX,
	    &cp->cfg->cathedral_flock_dst) == -1) {
		ret = config_error(cp,
		    "cathedral_flock_dst <64-bit hex value>");
		return (ret);
	}

	return (0);
}

/*
 * Parse the cathedral_nat_port configuration option.
 */
static int
config_parse_cathedral_nat_port(struct config_parser *cp, char *opt)
{
	u_int64_t	port;

	PRECOND(cp != NULL);
	PRECOND(opt != NULL);

	if (config_parse_number(opt, 10, USHRT_MAX, &port) == -1 || port == 0)
		return (config_error(cp, "cathedral_nat_port <port>"));

	cp->cfg->cathedral_nat_port = port;

	return (0);
}

/*
 * Parse the cathedral_remembrance configuration option.
 */
static int
config_parse_cathedral_remembrance(struct config_parser *cp, char *opt)
{
	int		ret;

	PRECOND(cp != NULL);
	PRECOND(opt != NULL);

	ret = config_parse_yesno(cp, "cathedral_remembrance", opt,
	    VIGIL_CONFIG_FLAG_REMEMBRANCE, 0);

	return (ret);
}

/*
 * Parse the liturgy_group configuration option.
 */
static int
config_parse_liturgy_group(struct config_parser *cp, char *opt)
{
	u_int64_t	group;

	PRECOND(cp != NULL);
	PRECOND(opt != NULL);

	if (config_parse_number(opt, 16, USHRT_MAX, &group) == -1)
		return (config_error(cp, "liturgy_group <16-bit hex value>"));

	cp->cfg->liturgy_group = group;

	return (0);
}

/*
 * Parse the liturgy_prefix configuration option, just like sanctum
 * we require this to be in the form of x.x.0.0.
 */
static int
config_parse_liturgy_prefix(struct config_parser *cp, char *opt)
{
	u_int32_t	prefix;

	PRECOND(cp != NULL);
	PRECOND(opt != NULL);

	if (inet_pton(AF_INET, opt, &cp->cfg->liturgy_prefix) != 1)
		return (config_error(cp, "liturgy_prefix <ipv4 address>"));

	prefix = ntohl(cp->cfg->liturgy_prefix.s_addr);

	if (prefix & 0x0000ffff)
		return (config_error(cp, "liturgy_prefix should be x.x.0.0"));

	return (0);
}

/*
 * Parse the liturgy_discoverable configuration option.
 */
static int
config_parse_liturgy_discoverable(struct config_parser *cp, char *opt)
{
	int		ret;

	PRECOND(cp != NULL);
	PRECOND(opt != NULL);

	ret = config_parse_yesno(cp, "liturgy_discoverable", opt,
	    VIGIL_CONFIG_FLAG_HIDDEN, 1);

	return (ret);
}

/*
 * Helper to parse a yes|no option and set or clear the given flag.
 * If invert is 1 the flag is set on "no" instead of on "yes".
 */
static int
config_parse_yesno(struct config_parser *cp, const char *name,
    char *opt, u_int32_t flag, int invert)
{
	int		on, ret;

	PRECOND(cp != NULL);
	PRECOND(name != NULL);
	PRECOND(opt != NULL);
	PRECOND(invert == 0 || invert == 1);

	if (!strcmp(opt, "yes")) {
		on = 1;
	} else if (!strcmp(opt, "no")) {
		on = 0;
	} else {
		ret = config_error(cp,
		    "invalid %s option '%s', accepted: yes|no", name, opt);
		return (ret);
	}

	if (invert)
		on = !on;

	if (on)
		cp->cfg->flags |= flag;
	else
		cp->cfg->flags &= ~flag;

	return (0);
}

/*
 * Helper to parse a hex encoded key of exactly len bytes into out.
 */
static int
config_parse_hex(struct config_parser *cp, const char *name, char *opt,
    u_int8_t *out, size_t len, u_int32_t key)
{
	size_t		idx;
	int		hi, lo, ret;

	PRECOND(cp != NULL);
	PRECOND(name != NULL);
	PRECOND(opt != NULL);
	PRECOND(out != NULL);
	PRECOND(len > 0);

	if (cp->cfg->keys & key)
		return (config_error(cp, "%s already specified", name));

	if (strlen(opt) != len * 2) {
		ret = config_error(cp,
		    "%s must be %zu hex characters", name, len * 2);
		return (ret);
	}

	for (idx = 0; idx < len; idx++) {
		if (!isxdigit((unsigned char)opt[idx * 2]) ||
		    !isxdigit((unsigned char)opt[(idx * 2) + 1])) {
			nyfe_mem_zero(out, len);
			return (config_error(cp, "%s is not valid hex", name));
		}

		hi = tolower((unsigned char)opt[idx * 2]);
		lo = tolower((unsigned char)opt[(idx * 2) + 1]);

		hi = isdigit(hi) ? hi - '0' : hi - 'a' + 10;
		lo = isdigit(lo) ? lo - '0' : lo - 'a' + 10;

		out[idx] = (u_int8_t)((hi << 4) | lo);
	}

	nyfe_mem_zero(opt, len * 2);
	cp->cfg->keys |= key;

	return (0);
}

/*
 * Helper to parse a host:port option.
 */
static int
config_parse_host(struct config_parser *cp, const char *name, char *opt,
    char *host, size_t hostlen, u_int16_t *port)
{
	char		*p;
	int		ret;
	u_int64_t	value;
	size_t		idx, len;

	PRECOND(cp != NULL);
	PRECOND(name != NULL);
	PRECOND(opt != NULL);
	PRECOND(host != NULL);
	PRECOND(hostlen > 0);
	PRECOND(port != NULL);

	if (host[0] != '\0')
		return (config_error(cp, "%s already specified", name));

	p = strrchr(opt, ':');
	if (p == NULL)
		return (config_error(cp, "%s <host:port>", name));

	*(p)++ = '\0';

	len = strlen(opt);
	if (len == 0 || len >= hostlen) {
		ret = config_error(cp, "%s host has an invalid length", name);
		return (ret);
	}

	for (idx = 0; idx < len; idx++) {
		if (!isalnum((unsigned char)opt[idx]) &&
		    opt[idx] != '-' && opt[idx] != '.')
			return (config_error(cp, "%s host is invalid", name));
	}

	if (config_parse_number(p, 10, USHRT_MAX, &value) == -1 || value == 0)
		return (config_error(cp, "%s port is invalid", name));

	*port = value;

	memcpy(host, opt, len + 1);

	return (0);
}

/*
 * Helper to parse an ip/mask option.
 */
static int
config_parse_ip_mask(struct config_parser *cp, const char *name, char *opt,
    struct in_addr *ip, u_int8_t *prefix)
{
	char		*p;
	u_int64_t	mask;

	PRECOND(cp != NULL);
	PRECOND(name != NULL);
	PRECOND(opt != NULL);
	PRECOND(ip != NULL);
	PRECOND(prefix != NULL);

	p = strchr(opt, '/');
	if (p == NULL)
		return (config_error(cp, "%s <ip/mask>", name));

	*(p)++ = '\0';

	if (inet_pton(AF_INET, opt, ip) != 1)
		return (config_error(cp, "%s ip '%s' is invalid", name, opt));

	if (config_parse_number(p, 10, 32, &mask) == -1)
		return (config_error(cp, "%s mask '%s' is invalid", name, p));

	*prefix = (u_int8_t)mask;

	return (0);
}

/*
 * Helper to parse a number in the given base that is at most max.
 */
static int
config_parse_number(const char *opt, int base, u_int64_t max, u_int64_t *out)
{
	char			*ep;
	unsigned long long	value;

	PRECOND(opt != NULL);
	PRECOND(base == 10 || base == 16);
	PRECOND(out != NULL);

	if (base == 10 && !isdigit((unsigned char)opt[0]))
		return (-1);

	if (base == 16 && !isxdigit((unsigned char)opt[0]))
		return (-1);

	errno = 0;
	value = strtoull(opt, &ep, base);
	if (errno != 0 || *ep != '\0' || value > max)
		return (-1);

	*out = value;

	return (0);
}

/*
 * Write an error into the parser error buffer, always returns -1.
 */
static int
config_error(struct config_parser *cp, const char *fmt, ...)
{
	va_list		args;

	PRECOND(cp != NULL);
	PRECOND(fmt != NULL);

	va_start(args, fmt);
	(void)vsnprintf(cp->err, cp->errlen, fmt, args);
	va_end(args);

	return (-1);
}

/*
 * Copy the options from data into the edited configuration. If edits
 * is 1 we only allow editable options, otherwise we skip them.
 */
static int
config_edit_copy(struct config_edit *ce, const char *data, size_t datalen,
    int edits)
{
	size_t		len;
	int		idx, ret;
	char		*buf, *line, *next, *opt;

	PRECOND(ce != NULL);
	PRECOND(data != NULL);

	if (datalen > VIGIL_CONFIG_MAX) {
		(void)snprintf(ce->err, ce->errlen,
		    "configuration has an invalid size");
		return (-1);
	}

	buf = calloc(1, datalen + 1);
	if (buf == NULL)
		fatal("calloc: failed to allocate configuration buffer");

	memcpy(buf, data, datalen);

	ret = -1;

	for (line = buf; line != NULL; line = next) {
		next = strchr(line, '\n');
		if (next != NULL)
			*(next)++ = '\0';

		line[strcspn(line, "\r")] = '\0';

		while (isspace((unsigned char)*line))
			line++;

		len = strlen(line);

		for (idx = len - 1; idx >= 0; idx--) {
			if (!isspace((unsigned char)line[idx]))
				break;
			line[idx] = '\0';
		}

		if (line[0] == '#' || line[0] == '\0')
			continue;

		opt = strchr(line, ' ');
		if (opt == NULL) {
			(void)snprintf(ce->err, ce->errlen,
			    "option '%s' is missing a value", line);
			goto cleanup;
		}

		*(opt)++ = '\0';

		while (isspace((unsigned char)*opt))
			opt++;

		if (edits) {
			if (!config_edit_option(line)) {
				(void)snprintf(ce->err, ce->errlen,
				    "option '%s' cannot be edited", line);
				goto cleanup;
			}
		} else {
			if (config_edit_option(line))
				continue;
		}

		if (config_edit_append(ce, line, opt) == -1)
			goto cleanup;
	}

	ret = 0;

cleanup:
	nyfe_mem_zero(buf, datalen);
	free(buf);

	return (ret);
}

/*
 * Check if the given option is one that may be edited.
 */
static int
config_edit_option(const char *option)
{
	PRECOND(option != NULL);

	if (!strcmp(option, "dns"))
		return (1);
	else if (!strcmp(option, "p2p"))
		return (1);
	else if (!strcmp(option, "route"))
		return (1);
	else if (!strcmp(option, "tier6"))
		return (1);
	else if (!strcmp(option, "instance"))
		return (1);

	return (0);
}

/*
 * Append the option and its value to the edited configuration.
 */
static int
config_edit_append(struct config_edit *ce, const char *option,
    const char *value)
{
	int		len;
	size_t		avail;

	PRECOND(ce != NULL);
	PRECOND(option != NULL);
	PRECOND(value != NULL);
	PRECOND(ce->offset < ce->outlen);

	avail = ce->outlen - ce->offset;
	len = snprintf(ce->out + ce->offset, avail, "%s %s\n", option, value);

	if (len < 0) {
		(void)snprintf(ce->err, ce->errlen,
		    "configuration has an invalid size");
		return (-1);
	}

	if ((size_t)len >= avail) {
		(void)snprintf(ce->err, ce->errlen,
		    "configuration has an invalid size");
		return (-1);
	}

	ce->offset += len;

	return (0);
}

/*
 * Helper to append a json boolean member.
 */
static void
config_json_bool(struct vigil_buf *buf, const char *name, int value)
{
	PRECOND(buf != NULL);
	PRECOND(name != NULL);

	vigil_buf_appendf(buf, "\"%s\":%s,", name, value ? "true" : "false");
}
