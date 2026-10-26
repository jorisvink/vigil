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

#ifndef __H_VIGIL_H
#define __H_VIGIL_H

#include <sys/types.h>
#include <sys/queue.h>

#include <netinet/in.h>

#include <errno.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <syslog.h>

#if !defined(VIGIL_IOS_PLATFORM)
#include <libkyrka/libkyrka.h>

/*
 * We need a libkyrka that lets us send and receive graces, which the
 * patch in dist-build/patches/libkyrka-graces.patch provides.
 */
#if !defined(KYRKA_PACKET_TYPE_GRACE)
#error "libkyrka without packet types, see dist-build/patches"
#endif
#endif

/* Portability for apple platforms. */
#if defined(__APPLE__)
#include <libkern/OSByteOrder.h>
#define htobe16(x)		OSSwapHostToBigInt16(x)
#define htobe32(x)		OSSwapHostToBigInt32(x)
#define htobe64(x)		OSSwapHostToBigInt64(x)
#define be16toh(x)		OSSwapBigToHostInt16(x)
#define be32toh(x)		OSSwapBigToHostInt32(x)
#define be64toh(x)		OSSwapBigToHostInt64(x)
#endif

/* A few handy macros. */
#define errno_s		strerror(errno)

#define PRECOND(x)							\
	do {								\
		if (!(x)) {						\
			fatal("precondition failed in %s:%s:%d",	\
			    __FILE__, __func__, __LINE__);		\
		}							\
	} while (0)

#define VERIFY(x)							\
	do {								\
		if (!(x)) {						\
			fatal("verification failed in %s:%s:%d",	\
			    __FILE__, __func__, __LINE__);		\
		}							\
	} while (0)

/*
 * Atomic operations used in vigil.
 */
#define vigil_atomic_read(x)		\
    __atomic_load_n(x, __ATOMIC_SEQ_CST)

#define vigil_atomic_write(x, v)	\
    __atomic_store_n(x, v, __ATOMIC_SEQ_CST)

#define vigil_atomic_cas(x, e, d)	\
    __atomic_compare_exchange(x, e, d, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)

#define vigil_atomic_cas_simple(x, e, d)	\
    __sync_bool_compare_and_swap(x, e, d)

#define vigil_atomic_add(x, e)		\
    __atomic_fetch_add(x, e, __ATOMIC_SEQ_CST)

#define vigil_atomic_fence()		\
    __atomic_thread_fence(__ATOMIC_SEQ_CST)

/*
 * Use architecture specific instructions to hint to the CPU that
 * we are in a spinloop hopefully avoiding a memory order violation
 * which would incur a performance hit.
 */
#if defined(__arm__) || defined(__arm64__) || defined(__aarch64__)
#define vigil_cpu_pause()					\
	do {							\
		__asm__ volatile("yield" ::: "memory");		\
	} while (0)
#elif defined(__x86_64__)
#define vigil_cpu_pause()					\
	do {							\
		__asm__ volatile("pause" ::: "memory");		\
	} while (0)
#elif defined(__riscv)
#define vigil_cpu_pause()						\
	do {								\
		__asm__ volatile(".4byte 0x100000F" ::: "memory");	\
	} while (0)
#else
#error "unsupported architecture"
#endif

#include "vigil_shared.h"

/* The different types of graces. */
#define VIGIL_GRACE_TYPE_HEARTBEAT	1
#define VIGIL_GRACE_TYPE_MTU_PROBE	2
#define VIGIL_GRACE_TYPE_MTU_ACK	3

/* The milliseconds between grace heartbeats. */
#define VIGIL_GRACE_HEARTBEAT_INTERVAL	(15 * 1000)

/* The milliseconds between heartbeats while holepunching. */
#define VIGIL_GRACE_HOLEPUNCH_INTERVAL	1000

/* The milliseconds between MTU probes during discovery. */
#define VIGIL_GRACE_MTU_PROBE_INTERVAL	(5 * 1000)

/* The milliseconds between MTU discovery runs. */
#define VIGIL_GRACE_MTU_DISCOVERY	(600 * 1000)

/* The smallest MTU we will probe for or accept (sizeof(kyrka_offer)). */
#define VIGIL_GRACE_MTU_MIN		KYRKA_MTU_MINIMUM

/*
 * A grace header.
 */
struct vigil_grace {
	u_int16_t	type;
} __attribute__((packed));

/*
 * An MTU probe/ack sent over the grace channel.
 */
struct vigil_grace_mtu {
	struct vigil_grace	grace;
	u_int16_t		size;
} __attribute__((packed));

/*
 * Timekeeping for a recurring grace.
 */
struct vigil_grace_time {
	u_int64_t		next;
	u_int64_t		reset;
	u_int64_t		interval;
};

/*
 * An ethernet frame header.
 */
struct vigil_ether {
	u_int8_t	dst[VIGIL_ETHER_MAC_LEN];
	u_int8_t	src[VIGIL_ETHER_MAC_LEN];
	u_int16_t	proto;
} __attribute__((packed));

/*
 * A tier6 heartbeat.
 */
struct vigil_ether_hb {
	u_int16_t		port;
	u_int32_t		ips[VIGIL_ETHER_HB_IPV4_MAX];
	u_int32_t		masks[VIGIL_ETHER_HB_IPV4_MAX];
} __attribute__((packed));

/*
 * A MAC address seen on a peer and its ipv4 address, if we know it.
 */
struct vigil_mac {
	u_int64_t			age;
	struct in_addr			ip;
	u_int8_t			addr[VIGIL_ETHER_MAC_LEN];

	LIST_ENTRY(vigil_mac)		list;
};

/*
 * Data carried in a heartbeat packet, must be at most
 * VIGIL_CHAT_MESSAGE_MAX bytes large.
 */
struct vigil_chat_hb {
	u_int64_t		uid;
	u_int8_t		name[VIGIL_CHAT_NICK_MAX];
	u_int8_t		typing;
	u_int8_t		features;
} __attribute__((packed));

/*
 * A single frame in a chat, all frames are sent at full size so text,
 * acks and heartbeats all look the same on the wire.
 */
struct vigil_chat_frame {
	u_int64_t		id;
	u_int16_t		len;
	u_int8_t		type;
	u_int8_t		data[VIGIL_CHAT_MESSAGE_MAX];
} __attribute__((packed));

/*
 * What goes in front of the data we send, its kind and its name from sender.
 */
struct vigil_chat_data_head {
	u_int8_t		kind;
	u_int8_t		len;
	u_int8_t		name[VIGIL_CHAT_DATA_NAME_MAX];
} __attribute__((packed));

/*
 * A chunk of data.
 */
struct vigil_chat_chunk {
	u_int64_t		id;
	u_int32_t		index;
	u_int32_t		count;
	u_int32_t		size;
	u_int8_t		data[VIGIL_CHAT_CHUNK_DATA];
} __attribute__((packed));

/*
 * A fragment of a message on the link, always sent at full size.
 */
struct vigil_link_fragment {
	u_int32_t		total;
	u_int32_t		offset;
	u_int8_t		data[VIGIL_LINK_FRAGMENT_DATA];
} __attribute__((packed));

/*
 * An ack from a peer.
 */
struct vigil_chat_data_ack {
	u_int32_t		base;
	u_int32_t		latest;
	u_int8_t		bits[VIGIL_CHAT_MESSAGE_MAX - 8];
} __attribute__((packed));

/*
 * Voice frame header.
 */
struct vigil_voice_hdr {
	u_int8_t		type;
	u_int8_t		rfu;
	u_int16_t		seq;
	u_int16_t		length;
} __attribute__((packed));

/*
 * Video frame header.
 */
struct vigil_video_hdr {
	u_int8_t		type;
	u_int8_t		flags;
	u_int16_t		length;
	u_int32_t		frame;
	u_int32_t		size;
	u_int16_t		fragment;
	u_int16_t		fragsize;
} __attribute__((packed));

/*
 * Video control channel we send periodically.
 */
struct vigil_video_ctl {
	u_int8_t		camera;
	u_int8_t		keyframe;
	u_int16_t		rfu;
	u_int32_t		frames;
	u_int32_t		lost;
	u_int32_t		fragments;
	u_int32_t		nacks;
} __attribute__((packed));

/* How many video fragments we can be missing. */
#define VIGIL_VIDEO_NACK_MAX	64

struct vigil_video_nack {
	u_int32_t		frame;
	u_int16_t		count;
	u_int16_t		fragments[VIGIL_VIDEO_NACK_MAX];
} __attribute__((packed));

/*
 * A multi-producer, multi-consumer ring queue of indices, see src/ring.c.
 */
#define VIGIL_RING_MAX		64

struct vigil_ring_span {
	volatile u_int32_t	head;
	volatile u_int32_t	tail;
};

struct vigil_ring {
	u_int32_t		elm;
	u_int32_t		mask;
	struct vigil_ring_span	producer;
	struct vigil_ring_span	consumer;
	volatile u_int32_t	data[VIGIL_RING_MAX];
};

/*
 * The video link state.
 */
struct vigil_video_link {
	u_int16_t		mtu;
	u_int64_t		now;

	KYRKA			*ctx;

	int			live;
	int			shroud;
	int			online;
	int			talking;
};

/*
 * ipv4 routing information.
 */
struct vigil_route {
	struct in_addr		net;
	u_int8_t		prefix;
};

/*
 * A vigil configuration.
 *
 * Note that this holds key material, release it via vigil_config_free()
 * which makes sure the key material is zeroized.
 */
struct vigil_config {
	int			mode;
	u_int32_t		keys;
	u_int32_t		flags;
	char			instance[VIGIL_INSTANCE_MAX];

	struct in_addr		ip;
	u_int16_t		mtu;
	u_int8_t		tun_prefix;

	int			route_count;
	struct vigil_route	routes[VIGIL_ROUTES_MAX];

	int			dns_count;
	struct in_addr		dns[VIGIL_DNS_MAX];

	u_int16_t		peer_port;
	char			peer_host[VIGIL_HOST_MAX];

	u_int16_t		cathedral_port;
	u_int16_t		cathedral_nat_port;
	char			cathedral_host[VIGIL_HOST_MAX];

	u_int16_t		spi;
	u_int32_t		cathedral_id;
	u_int64_t		cathedral_flock;
	u_int64_t		cathedral_flock_dst;

	u_int16_t		liturgy_group;
	struct in_addr		liturgy_prefix;

	u_int8_t		kek[VIGIL_KEY_LENGTH];
	u_int8_t		secret[VIGIL_KEY_LENGTH];
	u_int8_t		cathedral_secret[VIGIL_KEY_LENGTH];
	u_int8_t		cathedral_cosk[VIGIL_COSK_LENGTH];
};

/*
 * An i/o event callback with user data.
 */
#define VIGIL_IO_READABLE		(1 << 0)

struct vigil_io {
	int		fd;
	u_int32_t	flags;
	void		(*handle)(void *);
};

/*
 * The cathedrals a cathedral told us about and where we keep them.
 */
struct vigil_remembrance {
	char				path[1024];
	struct kyrka_event_remembrance	list;
};

/*
 * A fixed size buffer we append text to, see src/buf.c.
 */
#define VIGIL_BUF_FULL		0x0001

struct vigil_buf {
	char			*data;
	int			flags;
	size_t			length;
	size_t			offset;
};

/*
 * A cathedral we are talking to and the last time we heard from it.
 */
struct vigil_cathedral {
	struct sockaddr_in		addr;
	u_int64_t			last;
	int				alive;
	u_int32_t			timeout;
};

/*
 * A peer we have a tunnel with. In tunnel and cathedral mode we only
 * have a single peer, in liturgy mode we have one per discovered peer.
 */
#define VIGIL_PEER_FLAG_REMOVE		(1 << 0)
#define VIGIL_PEER_FLAG_ONLINE		(1 << 1)
#define VIGIL_PEER_FLAG_P2P		(1 << 2)
#define VIGIL_PEER_FLAG_LOCAL		(1 << 3)

struct vigil_peer {
	struct vigil_io			io;

	u_int8_t			id;
	u_int16_t			port;
	u_int32_t			flags;
	int				local_discovery;

	struct sockaddr_in		addr;

	u_int64_t			p2p_probe;
	u_int64_t			p2p_retry;
	u_int64_t			p2p_verify;
	struct sockaddr_in		p2p_failed;

	struct vigil_cathedral		cathedral;
	u_int64_t			cathedral_update;

	u_int32_t			ambry;
	u_int32_t			tx_spi;
	u_int32_t			rx_spi;

	u_int16_t			mtu;
	int				mtu_step;
	u_int16_t			mtu_size;
	struct vigil_grace_time		mtu_probes;
	struct vigil_grace_time		heartbeats;

	u_int64_t			rx_pkts;
	u_int64_t			tx_pkts;
	u_int64_t			rx_bytes;
	u_int64_t			tx_bytes;

	u_int64_t			alive;
	KYRKA				*ctx;

	LIST_HEAD(, vigil_mac)		macs;
	u_int32_t			mac_count;

	LIST_ENTRY(vigil_peer)		list;
};

LIST_HEAD(vigil_peer_list, vigil_peer);

/*
 * The global vigil state, only valid while vigil_run() is active.
 */
#define VIGIL_FLAG_RUNNING		(1 << 0)

struct vigil_state {
	int				fd;
	u_int64_t			now;
	u_int32_t			flags;
	u_int64_t			started;

	struct in_addr			ip;
	struct vigil_config		*gospel;
	struct vigil_config		*config;

	struct sockaddr_in		peer;
	struct vigil_cathedral		cathedral;
	struct vigil_remembrance	remembrance;

	u_int64_t			gospel_rx;
	u_int64_t			gospel_tx;
};

/* The maximum number of log lines kept around for the user interface. */
#define VIGIL_LOG_LINES			128

/* The maximum length of a single log line. */
#define VIGIL_LOG_LINE_MAX		192

/* The maximum number of packets we handle per i/o event. */
#define VIGIL_IO_BATCH			128

/* from $(OBJDIR)/version.c */
extern const char	*vigil_build_rev;
extern const char	*vigil_build_date;

/* src/vigil.c */
int	vigil_socket_udp(void);
void	vigil_socket_close(int);
void	vigil_kyrka_enter(void);
void	vigil_kyrka_leave(void);
void	vigil_wake_signal(void);
void	vigil_socket_nonblock(int);
int	vigil_socket_unreachable(int, int);
void	vigil_logv(int, const char *, va_list);
int	vigil_rebind_due(u_int64_t *, u_int64_t *, u_int64_t);
int	vigil_inet_match(struct sockaddr_in *, struct sockaddr_in *);
int	vigil_resolve(const char *, u_int16_t, struct sockaddr_in *);
void	vigil_log(int, const char *, ...)
	    __attribute__((format (printf, 2, 3)));
void	fatal(const char *, ...) __attribute__((format (printf, 1, 2)))
	    __attribute__((noreturn));

u_int64_t	vigil_time_ms(void);
u_int64_t	vigil_rebind_generation(void);
const char	*vigil_address(struct sockaddr_in *);

extern struct vigil_state	*vigil;

/* src/buf.c */
void	vigil_buf_quote(struct vigil_buf *, const char *);
void	vigil_buf_init(struct vigil_buf *, char *, size_t);
void	vigil_buf_appendf(struct vigil_buf *, const char *, ...)
	    __attribute__((format (printf, 2, 3)));

/* src/config.c */
void	vigil_config_free(struct vigil_config *);
int	vigil_config_summary(const struct vigil_config *, char *, size_t);

const char		*vigil_config_mode(const struct vigil_config *);
struct vigil_config	*vigil_config_parse(const char *, size_t,
			    char *, size_t);

/* src/call.c */
void	vigil_call_poke(void);
int	vigil_call_alive(void);
void	vigil_call_update(void);
void	vigil_call_rebind(void);
void	vigil_call_cleanup(void);
int	vigil_call_peer(u_int8_t);
void	vigil_call_init(const char *);
void	vigil_call_liturgies(struct vigil_buf *);
int	vigil_call_cathedral_pick(struct vigil_cathedral *);

struct sockaddr_in	*vigil_call_cathedral(void);

/* src/chat.c */
int	vigil_chat_busy(void);
void	vigil_chat_init(void);
void	vigil_chat_poke(void);
void	vigil_chat_update(void);
void	vigil_chat_rebind(void);
void	vigil_chat_cleanup(void);
void	vigil_chat_status(struct vigil_buf *);

/* src/ether.c */
void	vigil_ether_init(struct vigil_peer *);
void	vigil_ether_poke(struct vigil_peer *);
void	vigil_ether_update(struct vigil_peer *);
void	vigil_ether_cleanup(struct vigil_peer *);
void	vigil_ether_holepunch(struct vigil_peer *);
void	vigil_ether_output(const u_int8_t *, size_t);
void	vigil_ether_input(struct vigil_peer *, struct sockaddr_in *,
	    const u_int8_t *, size_t);

/* src/voice.c */
int	vigil_voice_busy(void);
void	vigil_voice_hangup(void);
void	vigil_voice_update(void);
void	vigil_voice_rebind(void);
void	vigil_voice_cleanup(void);
void	vigil_voice_call(u_int8_t, int);
void	vigil_voice_playback(int16_t *, size_t);
void	vigil_voice_capture(const int16_t *, size_t);

/* src/ring.c */
size_t	vigil_ring_pending(struct vigil_ring *);
void	vigil_ring_init(struct vigil_ring *, size_t);
int	vigil_ring_queue(struct vigil_ring *, u_int32_t);
int	vigil_ring_dequeue(struct vigil_ring *, u_int32_t *);

/* src/video.c */
void	vigil_video_open(void);
void	vigil_video_close(void);
size_t	vigil_video_status(char *, size_t);
void	vigil_video_update(struct vigil_video_link *);
void	vigil_video_ctl(struct vigil_video_link *,
	    const struct vigil_video_ctl *);
void	vigil_video_nack(struct vigil_video_link *,
	    const struct vigil_video_nack *);
void	vigil_video_recv(struct vigil_video_link *, const u_int8_t *,
	    size_t);

/* src/grace.c */
void	vigil_grace_init(struct vigil_peer *);
void	vigil_grace_poke(struct vigil_peer *);
void	vigil_grace_update(struct vigil_peer *);
void	vigil_grace_holepunch(struct vigil_peer *);
void	vigil_grace_mtu_start(struct vigil_peer *);
void	vigil_grace_input(struct vigil_peer *, struct sockaddr_in *,
	    const void *, size_t);

/* src/io.c */
void	vigil_io_wait(int);
void	vigil_io_init(void);
void	vigil_io_cleanup(void);
void	vigil_io_schedule(struct vigil_io *);
void	vigil_io_unschedule(struct vigil_io *);

/* src/peer.c */
void	vigil_peer_poke(void);
void	vigil_peer_init(void);
void	vigil_peer_prune(void);
void	vigil_peer_rebind(void);
void	vigil_peer_update(void);
void	vigil_peer_cleanup(void);
void	vigil_peer_state(u_int8_t, u_int8_t);
void	vigil_peer_output(const u_int8_t *, size_t);
void	vigil_peer_local(struct vigil_peer *, struct sockaddr_in *);

struct vigil_peer_list	*vigil_peer_list(void);

/* src/liturgy.c */
void	vigil_liturgy_poke(void);
void	vigil_liturgy_init(void);
int	vigil_liturgy_alive(void);
void	vigil_liturgy_update(void);
void	vigil_liturgy_rebind(void);
void	vigil_liturgy_cleanup(void);

struct sockaddr_in	*vigil_liturgy_cathedral(void);

/* src/inet.c */
void	vigil_inet_tier6(struct in_addr *, u_int64_t, u_int8_t);
int	vigil_inet_ipv4_length(const u_int8_t *, size_t, size_t *);
int	vigil_inet_rewrite_src(u_int8_t *, size_t, struct in_addr *);
int	vigil_inet_rewrite_dst(u_int8_t *, size_t, struct in_addr *);
int	vigil_inet_ipv4_dst(const u_int8_t *, size_t, struct in_addr *);
int	vigil_inet_ipv4_src(const u_int8_t *, size_t, struct in_addr *);
void	vigil_inet_liturgy(struct in_addr *, const struct in_addr *,
	    u_int8_t, u_int8_t);
size_t	vigil_inet_icmp_frag(const u_int8_t *, size_t, u_int16_t,
	    u_int8_t *, size_t);

u_int8_t	vigil_inet_version(const u_int8_t *, size_t);

/* src/remembrance.c */
void	vigil_remembrance_cleanup(struct vigil_remembrance *);
int	vigil_remembrance_cathedral(struct vigil_remembrance *,
	    struct vigil_cathedral *);
int	vigil_remembrance_address(struct vigil_remembrance *,
	    int, struct sockaddr_in *);
void	vigil_remembrance_save(struct vigil_remembrance *,
	    struct kyrka_event_remembrance *);
void	vigil_remembrance_init(struct vigil_remembrance *,
	    const char *, u_int64_t);

/* src/status.c */
void	vigil_status_init(void);
void	vigil_status_update(int);
void	vigil_status_reset(void);
void	vigil_status_log(const char *);
void	vigil_status_liturgy(struct vigil_buf *, const char *,
	    u_int16_t, struct vigil_cathedral *, u_int64_t, u_int64_t);

/*
 * The platform bits, implemented in src/platform_<platform>.c.
 *
 * These are called from the vigil event loop thread.
 */
int	vigil_platform_socket(int);
void	vigil_platform_socket_close(int);
void	vigil_platform_log(int, const char *);
ssize_t	vigil_platform_tun_read(int, u_int8_t *, size_t);
ssize_t	vigil_platform_tun_write(int, const u_int8_t *, size_t);
void	vigil_platform_fatal(void) __attribute__((noreturn));

/*
 * The audio bits, implemented in src/audio_<platform>.c.
 *
 * These are called from the voice thread, the platform audio calls
 * vigil_voice_capture() and vigil_voice_playback() from its own.
 */
void	vigil_platform_audio_stop(void);
int	vigil_platform_audio_start(void);

/*
 * The video bits, implemented in src/video_<platform>.c.
 *
 * These are called from the voice thread, the platform video calls
 * vigil_video_input() and vigil_video_take() from its own until
 * vigil_platform_video_stop() returned.
 */
void	vigil_platform_video_stop(void);
int	vigil_platform_video_start(void);
void	vigil_platform_video_camera(int);
void	vigil_platform_video_keyframe(void);
void	vigil_platform_video_bitrate(u_int32_t);

#if defined(PLATFORM_ANDROID)
#include <jni.h>

/* src/video_android.c */
int	vigil_android_video_load(JavaVM *, JNIEnv *);
#endif

#endif
