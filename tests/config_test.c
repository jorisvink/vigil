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
 * Tests for the configuration parser.
 */

#include <sys/types.h>

#include <stdio.h>

#include "vigil.h"

#define HEX32	"000102030405060708090a0b0c0d0e0f" \
		"101112131415161718191a1b1c1d1e1f"
#define HEX64	HEX32 HEX32

static void	test_ok(const char *, const char *, const char *);
static void	test_fail(const char *, const char *, const char *);
static void	test_edit(const char *, const char *, const char *,
		    const char *);

static int	failures = 0;

int
main(void)
{
	test_ok("tunnel",
	    "vigil 1\n"
	    "# a comment\n"
	    "instance office\n"
	    "tunnel 10.77.0.2/24 1400\n"
	    "peer 198.51.100.1:4500\n"
	    "secret " HEX32 "\n"
	    "route 0.0.0.0/0\n"
	    "route 192.168.10.77/24\n"
	    "dns 10.77.0.1\n",
	    "{\"version\":1,\"instance\":\"office\",\"mode\":\"tunnel\","
	    "\"mtu\":1400,\"mtu_discovery\":false,"
	    "\"address\":\"10.77.0.2/24\","
	    "\"routes\":[\"0.0.0.0/0\",\"192.168.10.0/24\"],"
	    "\"dns\":[\"10.77.0.1\"],\"peer\":\"198.51.100.1:4500\","
	    "\"block_ipv6\":true,\"shroud\":false}");

	test_ok("tunnel with crlf and shroud",
	    "vigil 1\r\n"
	    "  instance a b\r\n"
	    "tunnel 10.0.0.2/32\r\n"
	    "peer vpn.example.com:1\r\n"
	    "shroud yes\r\n"
	    "block_ipv6 no\r\n"
	    "secret " HEX32 "\r\n",
	    "{\"version\":1,\"instance\":\"a b\",\"mode\":\"tunnel\","
	    "\"mtu\":1359,\"mtu_discovery\":false,"
	    "\"address\":\"10.0.0.2/32\",\"routes\":[],"
	    "\"dns\":[],\"peer\":\"vpn.example.com:1\","
	    "\"block_ipv6\":false,\"shroud\":true}");

	test_ok("cathedral",
	    "vigil 1\n"
	    "instance laptop\n"
	    "tunnel 10.88.0.2/24\n"
	    "spi 0c0d\n"
	    "kek " HEX32 "\n"
	    "cathedral 1.2.3.4:4469\n"
	    "cathedral_id 5eb2411f\n"
	    "cathedral_flock 493abf95a07e0c00\n"
	    "cathedral_secret " HEX32 "\n"
	    "cathedral_cosk " HEX64 "\n"
	    "p2p no\n",
	    "{\"version\":1,\"instance\":\"laptop\",\"mode\":\"cathedral\","
	    "\"mtu\":1408,\"mtu_discovery\":false,"
	    "\"address\":\"10.88.0.2/24\",\"routes\":[],"
	    "\"dns\":[],\"cathedral\":\"1.2.3.4:4469\","
	    "\"cathedral_nat_port\":4470,\"cathedral_id\":\"5eb2411f\","
	    "\"cathedral_flock\":\"493abf95a07e0c00\",\"spi\":\"0c0d\","
	    "\"p2p\":false,\"remembrance\":true,"
	    "\"block_ipv6\":true,\"shroud\":false}");

	test_ok("liturgy",
	    "vigil 1\n"
	    "instance mesh\n"
	    "mode liturgy\n"
	    "spi 0c\n"
	    "kek " HEX32 "\n"
	    "liturgy_group cafe\n"
	    "liturgy_prefix 172.31.0.0\n"
	    "liturgy_discoverable no\n"
	    "cathedral 1.2.3.4:4469\n"
	    "cathedral_nat_port 4501\n"
	    "cathedral_remembrance no\n"
	    "cathedral_id 5eb2411f\n"
	    "cathedral_flock 493abf95a07e0c00\n"
	    "cathedral_secret " HEX32 "\n"
	    "cathedral_cosk " HEX64 "\n",
	    "{\"version\":1,\"instance\":\"mesh\",\"mode\":\"liturgy\","
	    "\"mtu\":1408,\"mtu_discovery\":false,"
	    "\"address\":\"172.31.12.12/16\","
	    "\"routes\":[\"172.31.0.0/16\"],\"dns\":[],"
	    "\"cathedral\":\"1.2.3.4:4469\",\"cathedral_nat_port\":4501,"
	    "\"cathedral_id\":\"5eb2411f\","
	    "\"cathedral_flock\":\"493abf95a07e0c00\",\"spi\":\"000c\","
	    "\"p2p\":true,\"remembrance\":false,"
	    "\"liturgy_prefix\":\"172.31.0.0\",\"tier6\":false,"
	    "\"liturgy_group\":\"cafe\",\"liturgy_discoverable\":false,"
	    "\"block_ipv6\":true,\"shroud\":false}");

	test_ok("tier6",
	    "vigil 1\n"
	    "instance flock\n"
	    "mode liturgy\n"
	    "tier6 yes\n"
	    "spi 0c\n"
	    "kek " HEX32 "\n"
	    "liturgy_prefix 172.31.0.0\n"
	    "cathedral 1.2.3.4:4469\n"
	    "cathedral_id 5eb2411f\n"
	    "cathedral_flock 493abf95a07e0c00\n"
	    "cathedral_secret " HEX32 "\n"
	    "cathedral_cosk " HEX64 "\n",
	    "{\"version\":1,\"instance\":\"flock\",\"mode\":\"liturgy\","
	    "\"mtu\":1408,\"mtu_discovery\":false,"
	    "\"address\":\"10.122.84.12/24\","
	    "\"routes\":[\"10.122.84.0/24\"],\"dns\":[],"
	    "\"cathedral\":\"1.2.3.4:4469\",\"cathedral_nat_port\":4470,"
	    "\"cathedral_id\":\"5eb2411f\","
	    "\"cathedral_flock\":\"493abf95a07e0c00\",\"spi\":\"000c\","
	    "\"p2p\":true,\"remembrance\":true,\"tier6\":true,"
	    "\"liturgy_group\":\"0001\",\"liturgy_discoverable\":true,"
	    "\"block_ipv6\":true,\"shroud\":false}");

	test_ok("tier6 with tunnel",
	    "vigil 1\n"
	    "instance flock\n"
	    "mode liturgy\n"
	    "tier6 yes\n"
	    "tunnel 192.168.5.20/23 1300\n"
	    "spi 0c\n"
	    "kek " HEX32 "\n"
	    "cathedral 1.2.3.4:4469\n"
	    "cathedral_id 5eb2411f\n"
	    "cathedral_flock 493abf95a07e0c00\n"
	    "cathedral_secret " HEX32 "\n"
	    "cathedral_cosk " HEX64 "\n",
	    "{\"version\":1,\"instance\":\"flock\",\"mode\":\"liturgy\","
	    "\"mtu\":1300,\"mtu_discovery\":false,"
	    "\"address\":\"192.168.5.20/23\","
	    "\"routes\":[\"192.168.4.0/23\"],\"dns\":[],"
	    "\"cathedral\":\"1.2.3.4:4469\",\"cathedral_nat_port\":4470,"
	    "\"cathedral_id\":\"5eb2411f\","
	    "\"cathedral_flock\":\"493abf95a07e0c00\",\"spi\":\"000c\","
	    "\"p2p\":true,\"remembrance\":true,\"tier6\":true,"
	    "\"liturgy_group\":\"0001\",\"liturgy_discoverable\":true,"
	    "\"block_ipv6\":true,\"shroud\":false}");

	test_ok("gospel",
	    "vigil 1\n"
	    "instance calls\n"
	    "mode gospel\n"
	    "spi 0c\n"
	    "kek " HEX32 "\n"
	    "liturgy_group 0042\n"
	    "cathedral 1.2.3.4:4469\n"
	    "cathedral_id 5eb2411f\n"
	    "cathedral_flock 493abf95a07e0c00\n"
	    "cathedral_secret " HEX32 "\n"
	    "cathedral_cosk " HEX64 "\n",
	    "{\"version\":1,\"instance\":\"calls\",\"mode\":\"gospel\","
	    "\"mtu\":1408,\"mtu_discovery\":false,\"routes\":[],"
	    "\"dns\":[],\"cathedral\":\"1.2.3.4:4469\","
	    "\"cathedral_nat_port\":4470,\"cathedral_id\":\"5eb2411f\","
	    "\"cathedral_flock\":\"493abf95a07e0c00\",\"spi\":\"000c\","
	    "\"p2p\":true,\"remembrance\":true,\"liturgy_group\":\"0042\","
	    "\"block_ipv6\":true,\"shroud\":false}");

	test_ok("voice is gospel",
	    "vigil 1\n"
	    "instance calls\n"
	    "mode voice\n"
	    "spi 0c\n"
	    "kek " HEX32 "\n"
	    "liturgy_group 0042\n"
	    "cathedral 1.2.3.4:4469\n"
	    "cathedral_id 5eb2411f\n"
	    "cathedral_flock 493abf95a07e0c00\n"
	    "cathedral_secret " HEX32 "\n"
	    "cathedral_cosk " HEX64 "\n",
	    "{\"version\":1,\"instance\":\"calls\",\"mode\":\"gospel\","
	    "\"mtu\":1408,\"mtu_discovery\":false,\"routes\":[],"
	    "\"dns\":[],\"cathedral\":\"1.2.3.4:4469\","
	    "\"cathedral_nat_port\":4470,\"cathedral_id\":\"5eb2411f\","
	    "\"cathedral_flock\":\"493abf95a07e0c00\",\"spi\":\"000c\","
	    "\"p2p\":true,\"remembrance\":true,\"liturgy_group\":\"0042\","
	    "\"block_ipv6\":true,\"shroud\":false}");

	test_ok("mtu auto",
	    "vigil 1\n"
	    "instance auto\n"
	    "tunnel 10.0.0.2/24 auto\n"
	    "peer 1.2.3.4:5\n"
	    "secret " HEX32 "\n",
	    "{\"version\":1,\"instance\":\"auto\",\"mode\":\"tunnel\","
	    "\"mtu\":1408,\"mtu_discovery\":true,"
	    "\"address\":\"10.0.0.2/24\",\"routes\":[],"
	    "\"dns\":[],\"peer\":\"1.2.3.4:5\","
	    "\"block_ipv6\":true,\"shroud\":false}");

	test_ok("mtu auto via mtu",
	    "vigil 1\n"
	    "instance auto\n"
	    "tunnel 10.0.0.2/24\n"
	    "mtu auto\n"
	    "shroud yes\n"
	    "peer 1.2.3.4:5\n"
	    "secret " HEX32 "\n",
	    "{\"version\":1,\"instance\":\"auto\",\"mode\":\"tunnel\","
	    "\"mtu\":1359,\"mtu_discovery\":true,"
	    "\"address\":\"10.0.0.2/24\",\"routes\":[],"
	    "\"dns\":[],\"peer\":\"1.2.3.4:5\","
	    "\"block_ipv6\":true,\"shroud\":true}");

	test_fail("tunnel bad mtu word",
	    "vigil 1\ninstance x\ntunnel 10.0.0.2/24 big\n",
	    "tunnel <ip/mask> [mtu|auto]");

	test_fail("empty", "", "configuration has an invalid size");
	test_fail("not ours", "WIFI:S:foo;;", "not a vigil configuration");
	test_fail("version", "vigil 2\n",
	    "unsupported configuration version 2");
	test_fail("unknown option", "vigil 1\nfoo bar\n",
	    "line 2: unknown option 'foo'");
	test_fail("no value", "vigil 1\ninstance\n",
	    "line 2: option 'instance' is missing a value");
	test_fail("no instance", "vigil 1\npeer 1.2.3.4:5\n",
	    "no instance was configured");
	test_fail("no secret",
	    "vigil 1\ninstance x\npeer 1.2.3.4:5\ntunnel 10.0.0.1/24\n",
	    "no secret was configured");
	test_fail("short secret",
	    "vigil 1\ninstance x\nsecret abcd\n",
	    "secret must be 64 hex characters");
	test_fail("bad hex",
	    "vigil 1\ninstance x\nsecret zz"
	    "0102030405060708090a0b0c0d0e0f"
	    "101112131415161718191a1b1c1d1e1f\n",
	    "secret is not valid hex");
	test_fail("bad instance",
	    "vigil 1\ninstance \"quoted\"\n", "instance name is invalid");
	test_fail("mtu too high",
	    "vigil 1\ninstance x\npeer 1.2.3.4:5\ntunnel 10.0.0.1/24 1500\n"
	    "secret " HEX32 "\n", "mtu may not be higher than 1408");
	test_fail("peer and cathedral",
	    "vigil 1\ninstance x\npeer 1.2.3.4:5\ncathedral 1.2.3.4:6\n",
	    "peer cannot be combined with a cathedral");
	test_fail("cathedral missing cosk",
	    "vigil 1\ninstance x\ntunnel 10.0.0.2/24\nspi 0102\n"
	    "kek " HEX32 "\ncathedral 1.2.3.4:6\ncathedral_id 1\n"
	    "cathedral_flock 1\ncathedral_secret " HEX32 "\n",
	    "no cathedral_cosk was configured");
	test_fail("cathedral bad spi",
	    "vigil 1\ninstance x\ntunnel 10.0.0.2/24\nspi 0101\n"
	    "kek " HEX32 "\ncathedral 1.2.3.4:6\ncathedral_id 1\n"
	    "cathedral_flock 1\ncathedral_secret " HEX32 "\n"
	    "cathedral_cosk " HEX64 "\n",
	    "spi must be <src><dst>");
	test_fail("liturgy prefix",
	    "vigil 1\ninstance x\nmode liturgy\nliturgy_prefix 10.1.2.0\n",
	    "liturgy_prefix should be x.x.0.0");
	test_fail("liturgy with tunnel",
	    "vigil 1\ninstance x\nmode liturgy\ntunnel 10.0.0.1/24\n",
	    "tunnel is not used in liturgy, see liturgy_prefix");
	test_fail("tier6 with route",
	    "vigil 1\ninstance x\nmode liturgy\ntier6 yes\n"
	    "route 10.0.0.0/8\n",
	    "route is not used with tier6");
	test_fail("tier6 with mtu auto",
	    "vigil 1\ninstance x\nmode liturgy\ntier6 yes\nmtu auto\n",
	    "mtu auto is not used with tier6");
	test_fail("tier6 with a /31",
	    "vigil 1\ninstance x\nmode liturgy\ntier6 yes\nspi 0c\n"
	    "tunnel 10.0.0.1/31\nkek " HEX32 "\n"
	    "cathedral 1.2.3.4:4469\ncathedral_id 5eb2411f\n"
	    "cathedral_flock 493abf95a07e0c00\n"
	    "cathedral_secret " HEX32 "\ncathedral_cosk " HEX64 "\n",
	    "tunnel mask must be between 1-30");
	test_fail("gospel with tunnel",
	    "vigil 1\ninstance x\nmode gospel\ntunnel 10.0.0.2/24\n",
	    "tunnel is not used for gospel");
	test_fail("gospel with route",
	    "vigil 1\ninstance x\nmode gospel\nroute 10.0.0.0/24\n",
	    "route is not used for gospel");
	test_fail("gospel with liturgy_prefix",
	    "vigil 1\ninstance x\nmode gospel\nliturgy_prefix 172.31.0.0\n",
	    "liturgy_prefix is not used for gospel");
	test_fail("gospel without kek",
	    "vigil 1\ninstance x\nmode gospel\nspi 0c\n"
	    "cathedral 1.2.3.4:4469\ncathedral_id 5eb2411f\n"
	    "cathedral_flock 493abf95a07e0c00\n"
	    "cathedral_secret " HEX32 "\ncathedral_cosk " HEX64 "\n",
	    "no kek was configured");
	test_fail("gospel with last liturgy_group",
	    "vigil 1\ninstance x\nmode gospel\nspi 0c\nkek " HEX32 "\n"
	    "liturgy_group ffff\n"
	    "cathedral 1.2.3.4:4469\ncathedral_id 5eb2411f\n"
	    "cathedral_flock 493abf95a07e0c00\n"
	    "cathedral_secret " HEX32 "\ncathedral_cosk " HEX64 "\n",
	    "liturgy_group must be below ffff for gospel");
	test_fail("gospel without remembrance",
	    "vigil 1\ninstance x\nmode gospel\nspi 0c\nkek " HEX32 "\n"
	    "cathedral 1.2.3.4:4469\ncathedral_id 5eb2411f\n"
	    "cathedral_flock 493abf95a07e0c00\n"
	    "cathedral_secret " HEX32 "\ncathedral_cosk " HEX64 "\n"
	    "cathedral_remembrance no\n",
	    "cathedral_remembrance is always on for gospel");
	test_fail("peer port too large",
	    "vigil 1\ninstance x\npeer 1.2.3.4:70000\n",
	    "peer port is invalid");
	test_fail("peer port with junk",
	    "vigil 1\ninstance x\npeer 1.2.3.4:4500junk\n",
	    "peer port is invalid");
	test_fail("mtu too large",
	    "vigil 1\ninstance x\nmtu 66900\n",
	    "mtu <int|auto>");
	test_fail("tunnel mtu too large",
	    "vigil 1\ninstance x\ntunnel 10.0.0.2/24 66900\n",
	    "tunnel <ip/mask> [mtu|auto]");
	test_fail("tunnel mask with junk",
	    "vigil 1\ninstance x\ntunnel 10.0.0.2/24junk\n",
	    "tunnel mask '24junk' is invalid");
	test_fail("spi too large",
	    "vigil 1\ninstance x\nspi 10c0d\n",
	    "spi <16-bit hex value>");
	test_fail("negative spi",
	    "vigil 1\ninstance x\nspi -1\n",
	    "spi <16-bit hex value>");
	test_fail("cathedral_id too large",
	    "vigil 1\ninstance x\ncathedral_id 15eb2411f\n",
	    "cathedral_id <32-bit hex value>");
	test_fail("nat port too large",
	    "vigil 1\ninstance x\ncathedral_nat_port 69999\n",
	    "cathedral_nat_port <port>");
	test_fail("version with junk",
	    "vigil 1x\ninstance x\n",
	    "not a vigil configuration");
	test_fail("peer host with quotes",
	    "vigil 1\ninstance x\ntunnel 10.0.0.2/32\n"
	    "peer a\",\"routes\":[\"0.0.0.0/0\"],\"b:5\n",
	    "peer host is invalid");
	test_fail("cathedral host with a space",
	    "vigil 1\ninstance x\ncathedral a b:4469\n",
	    "cathedral host is invalid");
	test_fail("secret with cathedral",
	    "vigil 1\ninstance x\nmode liturgy\nsecret " HEX32 "\n"
	    "cathedral 1.2.3.4:5\n",
	    "secret cannot be combined with a cathedral");

	test_edit("edit routes and dns",
	    "vigil 1\n"
	    "instance office\n"
	    "route 0.0.0.0/0\n"
	    "tunnel 10.77.0.2/24 1400\n"
	    "peer 198.51.100.1:4500\n"
	    "  dns 10.77.0.1\n"
	    "# route 1.0.0.0/8\n"
	    "secret " HEX32,
	    "\nroute 10.0.0.0/8\r\n\nroute 192.168.1.0/24\ndns 9.9.9.9\n"
	    "instance office\n",
	    "vigil 1\n"
	    "tunnel 10.77.0.2/24 1400\n"
	    "peer 198.51.100.1:4500\n"
	    "secret " HEX32 "\n"
	    "route 10.0.0.0/8\n"
	    "route 192.168.1.0/24\n"
	    "dns 9.9.9.9\n"
	    "instance office\n");
	test_edit("edit p2p and clear routes",
	    "vigil 1\n"
	    "instance laptop\n"
	    "tunnel 10.88.0.2/24\n"
	    "spi 0c0d\n"
	    "kek " HEX32 "\n"
	    "p2p no\n"
	    "route 10.0.0.0/8\n"
	    "cathedral 1.2.3.4:4469\n"
	    "cathedral_id 5eb2411f\n"
	    "cathedral_flock 493abf95a07e0c00\n"
	    "cathedral_secret " HEX32 "\n"
	    "cathedral_cosk " HEX64 "\n",
	    "p2p yes\ninstance laptop",
	    "vigil 1\n"
	    "tunnel 10.88.0.2/24\n"
	    "spi 0c0d\n"
	    "kek " HEX32 "\n"
	    "cathedral 1.2.3.4:4469\n"
	    "cathedral_id 5eb2411f\n"
	    "cathedral_flock 493abf95a07e0c00\n"
	    "cathedral_secret " HEX32 "\n"
	    "cathedral_cosk " HEX64 "\n"
	    "p2p yes\n"
	    "instance laptop\n");
	test_edit("edit other option",
	    "vigil 1\ninstance x\ntunnel 10.0.0.2/32\n"
	    "peer 1.2.3.4:5\nsecret " HEX32 "\n",
	    "route 10.0.0.0/8\npeer 6.6.6.6:1\n",
	    "option 'peer' cannot be edited");
	test_edit("edit invalid route",
	    "vigil 1\ninstance x\ntunnel 10.0.0.2/32\n"
	    "peer 1.2.3.4:5\nsecret " HEX32 "\n",
	    "route 10.0.0.0/33\ninstance x\n",
	    "route mask '33' is invalid");
	test_edit("edit routes in liturgy",
	    "vigil 1\ninstance x\nmode liturgy\nspi 0c\n"
	    "kek " HEX32 "\nliturgy_prefix 172.31.0.0\n"
	    "cathedral 1.2.3.4:4469\ncathedral_id 5eb2411f\n"
	    "cathedral_flock 493abf95a07e0c00\n"
	    "cathedral_secret " HEX32 "\ncathedral_cosk " HEX64 "\n",
	    "route 10.0.0.0/8\ninstance x\n",
	    "route is not used in liturgy, see liturgy_prefix");
	test_edit("edit liturgy into tier6",
	    "vigil 1\ninstance x\nmode liturgy\nspi 0c\n"
	    "kek " HEX32 "\nliturgy_prefix 172.31.0.0\n"
	    "cathedral 1.2.3.4:4469\ncathedral_id 5eb2411f\n"
	    "cathedral_flock 493abf95a07e0c00\n"
	    "cathedral_secret " HEX32 "\ncathedral_cosk " HEX64 "\n"
	    "tier6 no\n",
	    "tier6 yes\ninstance x\n",
	    "vigil 1\nmode liturgy\nspi 0c\n"
	    "kek " HEX32 "\nliturgy_prefix 172.31.0.0\n"
	    "cathedral 1.2.3.4:4469\ncathedral_id 5eb2411f\n"
	    "cathedral_flock 493abf95a07e0c00\n"
	    "cathedral_secret " HEX32 "\ncathedral_cosk " HEX64 "\n"
	    "tier6 yes\ninstance x\n");
	test_edit("edit instance",
	    "vigil 1\ninstance office\ntunnel 10.0.0.2/32\n"
	    "peer 1.2.3.4:5\nsecret " HEX32 "\n",
	    "instance home office\n",
	    "vigil 1\ntunnel 10.0.0.2/32\n"
	    "peer 1.2.3.4:5\nsecret " HEX32 "\n"
	    "instance home office\n");
	test_edit("edit without instance",
	    "vigil 1\ninstance office\ntunnel 10.0.0.2/32\n"
	    "peer 1.2.3.4:5\nsecret " HEX32 "\n",
	    "route 10.0.0.0/8\n",
	    "no instance was configured");
	test_edit("edit invalid instance",
	    "vigil 1\ninstance office\ntunnel 10.0.0.2/32\n"
	    "peer 1.2.3.4:5\nsecret " HEX32 "\n",
	    "instance a/b\n",
	    "instance name is invalid");

	if (failures != 0) {
		printf("config_test: %d failures\n", failures);
		return (1);
	}

	printf("config_test: ok\n");

	return (0);
}

static void
test_ok(const char *name, const char *cfg, const char *expected)
{
	char		out[4096];

	if (vigil_config_check(cfg, strlen(cfg), out, sizeof(out)) == -1) {
		printf("FAIL %s: %s\n", name, out);
		failures++;
		return;
	}

	if (strcmp(out, expected)) {
		printf("FAIL %s:\n  got:      %s\n  expected: %s\n",
		    name, out, expected);
		failures++;
	}
}

static void
test_fail(const char *name, const char *cfg, const char *reason)
{
	char		out[4096], expected[256];

	(void)snprintf(expected, sizeof(expected),
	    "{\"error\":\"%s\"}", reason);

	if (vigil_config_check(cfg, strlen(cfg), out, sizeof(out)) == 0) {
		printf("FAIL %s: expected failure, got %s\n", name, out);
		failures++;
		return;
	}

	if (strcmp(out, expected)) {
		printf("FAIL %s:\n  got:      %s\n  expected: %s\n",
		    name, out, expected);
		failures++;
	}
}

static void
test_edit(const char *name, const char *cfg, const char *edits,
    const char *expected)
{
	int		ret;
	char		out[4096], err[128];

	ret = vigil_config_edit(cfg, strlen(cfg), edits, strlen(edits),
	    out, sizeof(out), err, sizeof(err));

	if (ret == -1 && strcmp(err, expected)) {
		printf("FAIL %s:\n  got:      %s\n  expected: %s\n",
		    name, err, expected);
		failures++;
	} else if (ret == 0 && strcmp(out, expected)) {
		printf("FAIL %s:\n  got:      %s\n  expected: %s\n",
		    name, out, expected);
		failures++;
	}
}
