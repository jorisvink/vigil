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

#ifndef __H_VIGIL_SHARED_H
#define __H_VIGIL_SHARED_H

/*
 * The tunnel modes we support, these are selected by the configuration
 * that was imported via a QR code.
 *
 * VIGIL_MODE_TUNNEL    - A direct tunnel.
 * VIGIL_MODE_CATHEDRAL - A tunnel using a cathedral for peer discovery.
 * VIGIL_MODE_LITURGY   - Automatic full-mesh cathedral tunnels.
 * VIGIL_MODE_GOSPEL    - Calls and text messages over cathedral tunnels.
 */
#define VIGIL_MODE_TUNNEL		1
#define VIGIL_MODE_CATHEDRAL		2
#define VIGIL_MODE_LITURGY		3
#define VIGIL_MODE_GOSPEL		4

/* The version of the configuration format we understand. */
#define VIGIL_CONFIG_VERSION		1

/* The maximum length of an instance name. */
#define VIGIL_INSTANCE_MAX		64

/* The maximum length of a hostname. */
#define VIGIL_HOST_MAX			256

/* The maximum number of routes and dns servers in a configuration. */
#define VIGIL_DNS_MAX			4
#define VIGIL_ROUTES_MAX		32

/* The default MTU sizes, mimics what sanctum and kyrka-vpn use. */
#define VIGIL_MTU_DEFAULT		1408
#define VIGIL_MTU_SHROUD		1359

/* Length of our symmetrical keys and the cathedral signing key. */
#define VIGIL_KEY_LENGTH		32
#define VIGIL_COSK_LENGTH		64

/* The liturgy group used if none is configured. */
#define VIGIL_LITURGY_GROUP_DEFAULT	0x0001

/* The interval in milliseconds for our periodic peer updates. */
#define VIGIL_PEER_UPDATE_INTERVAL	1000

/* The milliseconds between reopening our sockets after a network change. */
#define VIGIL_REBIND_INTERVAL		2000

/* The interval in milliseconds between cathedral notifications. */
#define VIGIL_CATHEDRAL_NOTIFY		5000

/* Milliseconds until we consider a cathedral timed out. */
#define VIGIL_CATHEDRAL_TIMEOUT_INIT	(10 * 1000)

/* The milliseconds until we consider an alive cathedral timed out. */
#define VIGIL_CATHEDRAL_TIMEOUT		(45 * 1000)

/* The number of cathedrals our packets travel through with commixtion. */
#define VIGIL_COMMIXTION_HOPS		2

/* The milliseconds before we consider a peer timed out. */
#define VIGIL_PEER_ALIVE_TIMEOUT	(45 * 1000)

/* The maximum length of a configuration we accept. */
#define VIGIL_CONFIG_MAX		4096

/* The buffer vigil_status() needs at most, with all log lines escaped. */
#define VIGIL_STATUS_MAX		(512 * 1024)

/*
 * The link its secret, largest message (at least VIGIL_STATUS_MAX),
 * fragment data and batch (room for a full message).
 */
#define VIGIL_LINK_SECRET_LEN		32
#define VIGIL_LINK_FRAGMENT_DATA	1400
#define VIGIL_LINK_MESSAGE_MAX		(512 * 1024)
#define VIGIL_LINK_BATCH_MAX		(1024 * 1024)

/* The buffer vigil_chat_events() needs at least. */
#define VIGIL_CHAT_EVENTS_MAX		(64 * 1024)

/* The flock domain in which voice liturgies live. */
#define VIGIL_CALL_FLOCK_DOMAIN		0x0a

/* The milliseconds between signaling liturgy updates. */
#define VIGIL_CALL_NOTIFY		3000

/* The milliseconds a cathedral wants at least between liturgy updates. */
#define VIGIL_CALL_NOTIFY_MIN		1500

/* The milliseconds a peer stays present after its last signaling push. */
#define VIGIL_CALL_PRESENT_EXPIRE	(3 * VIGIL_CALL_NOTIFY)

/*
 * Milliseconds until we consider a signaling cathedral timed out, it only
 * answers us with a remembrance every 15 seconds.
 */
#define VIGIL_CALL_TIMEOUT_INIT		(20 * 1000)

/* The state of a peer in the signaling liturgy. */
#define VIGIL_CALL_PEER_WANTED		(1 << 0)
#define VIGIL_CALL_PEER_PRESENT		(1 << 1)
#define VIGIL_CALL_PEER_DONE		(1 << 2)
#define VIGIL_CALL_PEER_ONLINE		(1 << 3)

/* The signaling bits of the state of a peer. */
#define VIGIL_CALL_PEER_SIGNALING	\
    (VIGIL_CALL_PEER_WANTED | VIGIL_CALL_PEER_PRESENT | VIGIL_CALL_PEER_DONE)

/* Flock domains for gospel text messaging. */
#define VIGIL_CHAT_FLOCK_DOMAIN		0x0b
#define VIGIL_CHAT_GROUP_DOMAIN		0x0c

/* The liturgy group gospel signals in, every other one is a group chat. */
#define VIGIL_CHAT_SIGNAL_GROUP		0x0000

/* The maximum length of a message and of a nickname. */
#define VIGIL_CHAT_MESSAGE_MAX		512
#define VIGIL_CHAT_NICK_MAX		16

/* The different types of frames, an id of 0 is reserved. */
#define VIGIL_CHAT_TYPE_TEXT		1
#define VIGIL_CHAT_TYPE_ACK		2
#define VIGIL_CHAT_TYPE_HEARTBEAT	3
#define VIGIL_CHAT_TYPE_DATA_ACK	4

/* The features we tell our peers about in our heartbeats. */
#define VIGIL_CHAT_FEATURE_DATA		0x01

/* The largest data we send or take and how much goes in each chunk. */
#define VIGIL_CHAT_CHUNK_DATA		1152
#define VIGIL_CHAT_DATA_MAX		(4096 * 1024)

/* The id gospel puts on its heartbeats. */
#define VIGIL_CHAT_HEARTBEAT_ID		0xffffffffffffffffULL

/* Milliseconds until we consider a peer deadsies. */
#define VIGIL_PEER_DEAD_TIMEOUT		(120 * 1000)

/* The milliseconds a new p2p path gets to prove it works. */
#define VIGIL_PEER_P2P_VERIFY		(30 * 1000)

/* The milliseconds before we try a p2p path that did not work again. */
#define VIGIL_PEER_P2P_RETRY		(30 * 1000)
#define VIGIL_PEER_P2P_RETRY_MAX	(1800 * 1000)

/* Configuration flags. */
#define VIGIL_CONFIG_FLAG_SHROUD	(1 << 0)
#define VIGIL_CONFIG_FLAG_P2P		(1 << 1)
#define VIGIL_CONFIG_FLAG_HIDDEN	(1 << 3)
#define VIGIL_CONFIG_FLAG_BLOCK_IPV6	(1 << 4)
#define VIGIL_CONFIG_FLAG_MTU_DISCOVERY	(1 << 5)
#define VIGIL_CONFIG_FLAG_TIER6		(1 << 6)
#define VIGIL_CONFIG_FLAG_COMMIXTION	(1 << 7)

/* Which keys were present in a configuration. */
#define VIGIL_CONFIG_KEY_SECRET		(1 << 0)
#define VIGIL_CONFIG_KEY_KEK		(1 << 1)
#define VIGIL_CONFIG_KEY_CS		(1 << 2)
#define VIGIL_CONFIG_KEY_COSK		(1 << 3)

/*
 * The overhead a tunnel adds on the path, ip and udp headers, the
 * sanctum protocol header, its tail and the cipher tag. Shroud adds
 * its header and trailing byte to that.
 *
 * XXX - magic numbers because reasons.
 */
#define VIGIL_SHROUD_OVERHEAD		(16 + 16 + 16 + 1)
#define VIGIL_TUNNEL_OVERHEAD		(20 + 8 + 32 + 2 + 16)

/* Length of an ethernet MAC address. */
#define VIGIL_ETHER_MAC_LEN		6

/* The ethernet protocols we handle. */
#define VIGIL_ETHER_TYPE_ARP		0x0806
#define VIGIL_ETHER_TYPE_IPV4		0x0800

/* Special ethernet types for heartbeats and tier6 local discovery. */
#define VIGIL_ETHER_TYPE_HEARTBEAT	0xdead
#define VIGIL_ETHER_TYPE_DISC_PROBE	0xdeae
#define VIGIL_ETHER_TYPE_DISC_ACK	0xdeaf

/* Maximum number of IPv4 addresses in a heartbeat. */
#define VIGIL_ETHER_HB_IPV4_MAX		32

/* The kinds of data we send and how long the name of it may be. */
#define VIGIL_CHAT_DATA_IMAGE		1
#define VIGIL_CHAT_DATA_FILE		2
#define VIGIL_CHAT_DATA_VOICE		3
#define VIGIL_CHAT_DATA_NAME_MAX	126

/* The different type of voice frames. */
#define VIGIL_VOICE_TYPE_VOICE		1
#define VIGIL_VOICE_TYPE_HANGUP		2
#define VIGIL_VOICE_TYPE_VIDEO		3
#define VIGIL_VOICE_TYPE_VIDEO_CTL	4
#define VIGIL_VOICE_TYPE_VIDEO_NACK	5

/* The size of all frames in a call. */
#define VIGIL_VOICE_PAYLOAD		160

/* The space in a frame for an opus packet. */
#define VIGIL_VOICE_OPUS_MAX		\
    (VIGIL_VOICE_PAYLOAD - sizeof(struct vigil_voice_hdr))

/* The largest encoded video frame we send or take. */
#define VIGIL_VIDEO_FRAME_MAX		(1024 * 1024)

/*
 * The flags of an encoded video frame, a keyframe and the quarter turns
 * the receiver rotates the picture by.
 */
#define VIGIL_VIDEO_FLAG_KEY		0x01
#define VIGIL_VIDEO_ROTATE_SHIFT	1
#define VIGIL_VIDEO_ROTATE_MASK		0x03

/* The audio we capture and play, 20ms of 48khz mono per frame. */
#define VIGIL_VOICE_SAMPLE_COUNT	960
#define VIGIL_VOICE_SAMPLE_RATE		48000

/* How a voice session is run. */
#define VIGIL_VOICE_MODE_CALL		1
#define VIGIL_VOICE_MODE_DECLINE	2
#define VIGIL_VOICE_MODE_ANSWER		3
#define VIGIL_VOICE_MODE_RING		4

/* The states of a voice session. */
#define VIGIL_VOICE_STATE_IDLE		0
#define VIGIL_VOICE_STATE_CONNECTING	1
#define VIGIL_VOICE_STATE_ONLINE	2
#define VIGIL_VOICE_STATE_TALKING	3

/* Why a voice session ended. */
#define VIGIL_VOICE_END_LOCAL		0
#define VIGIL_VOICE_END_REMOTE		1
#define VIGIL_VOICE_END_TIMEOUT		2
#define VIGIL_VOICE_END_ERROR		3

/* src/call.c */
void	vigil_call_hangup(void);
void	vigil_call_dial(u_int8_t);
void	vigil_call_done(u_int8_t);
void	vigil_chat_group(u_int16_t);
void	vigil_chat_forget(u_int8_t);
void	vigil_chat_group_typing(int);
void	vigil_chat_stored(u_int64_t);
int	vigil_chat_nick(const char *);
void	vigil_chat_open(u_int8_t, int);
void	vigil_chat_typing(u_int8_t, int);
size_t	vigil_chat_events(char *, size_t);
ssize_t	vigil_chat_data_take(u_int64_t, void *, size_t);
int	vigil_chat_group_send(u_int64_t, const char *, size_t);
int	vigil_chat_send(u_int8_t, u_int64_t, const char *, size_t);
int	vigil_chat_data_send(u_int8_t, u_int64_t, int, const char *,
	    const void *, size_t);
int	vigil_chat_group_data_send(u_int64_t, int, const char *,
	    const void *, size_t);

/* src/link.c */
void	vigil_link_close(void);
int	vigil_link_ready(void);
int	vigil_link_open(const u_int8_t *, size_t);
ssize_t	vigil_link_input(const u_int8_t *, size_t, u_int8_t *, size_t);
ssize_t	vigil_link_output(const u_int8_t *, size_t, u_int8_t *, size_t);

/* src/config.c */
int	vigil_config_check(const char *, size_t, char *, size_t);
int	vigil_config_edit(const char *, size_t, const char *, size_t,
	    char *, size_t, char *, size_t);

/* src/vigil.c */
void	vigil_poke(void);
void	vigil_rebind(void);
void	vigil_stop(u_int64_t);
int	vigil_gospel(const char *, size_t);
int	vigil_run(u_int64_t, int, const char *, size_t,
	    const char *, size_t, const char *);

u_int64_t	vigil_run_id(void);
const char	*vigil_version(void);

/* src/status.c */
size_t	vigil_status(char *, size_t);

/* src/voice.c */
void	vigil_voice_mute(int);
void	vigil_voice_answer(void);
void	vigil_voice_stop(u_int64_t);
size_t	vigil_voice_status(char *, size_t);
int	vigil_voice_run(u_int64_t, const char *, size_t, u_int8_t, int,
	    const char *);

u_int64_t	vigil_voice_run_id(void);

/* src/video.c */
void	vigil_video_camera(int);
void	vigil_video_refresh(void);
int	vigil_video_input(const u_int8_t *, size_t, int);
size_t	vigil_video_take(u_int8_t *, size_t, int *);

#endif
