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

#include <ctype.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <strings.h>
#include <unistd.h>

#include "vigil.h"

#include <libkyrka/libnyfe.h>

/* The commands the platform gives us. */
#define CHAT_COMMAND_SEND		1
#define CHAT_COMMAND_FORGET		2
#define CHAT_COMMAND_STORED		3
#define CHAT_COMMAND_DATA		4

/* The events we have for the platform. */
#define CHAT_EVENT_MESSAGE		1
#define CHAT_EVENT_SENT			2
#define CHAT_EVENT_DELIVERED		3
#define CHAT_EVENT_DATA			4
#define CHAT_EVENT_AVATAR		5
#define CHAT_EVENT_NAME			6
#define CHAT_EVENT_AMBRY		7
#define CHAT_EVENT_TOPIC		8
#define CHAT_EVENT_ROSTER		9
#define CHAT_EVENT_LEAVE		10

/* Time between heartbeats we send to our peers. */
#define CHAT_HEARTBEAT			(12 * 1000)

/* Time before we re-send an un-acked message. */
#define CHAT_RESEND			(5 * 1000)

/* Time we keep a one-to-one tunnel alive. */
#define CHAT_IDLE			(60 * 1000)

/* Time until we consider a peer offline. */
#define CHAT_RX_TIMEOUT			(30 * 1000)

/* Time between key management and cathedral updates. */
#define CHAT_MANAGE_INTERVAL		(1 * 1000)
#define CHAT_NOTIFY_INTERVAL		(5 * 1000)

/* Time between p2p path check and failure. */
#define CHAT_P2P_VERIFY			(30 * 1000)
#define CHAT_P2P_RETRY			(30 * 1000)
#define CHAT_P2P_RETRY_MAX		(1800 * 1000)

/* Time without p2p traffic while our peer reaches us via the cathedral. */
#define CHAT_P2P_SILENT			(1 * 1000)

/* Time we heartbeat every second after we moved to a p2p path. */
#define CHAT_HOLEPUNCH			(10 * 1000)
#define CHAT_HOLEPUNCH_INTERVAL		(1 * 1000)

/* Time the platform gets to store a message we just received. */
#define CHAT_STORE_WAIT			(10 * 1000)

/* The number of messages we remember per tunnel so we can drop dupes. */
#define CHAT_SEEN_MAX			64

/* The maximum number of commands and events we let queue up. */
#define CHAT_EVENTS_MAX			256
#define CHAT_COMMANDS_MAX		1024

/* Max size for a JSON event. */
#define CHAT_EVENT_JSON_MAX		(4 * 1024)

/* The bits on the kind of data that are not its kind. */
#define CHAT_DATA_FLAGS			\
    (VIGIL_CHAT_DATA_TOPIC | VIGIL_CHAT_DATA_GROUP)

/* The size of the owner, id and serial of a group in front of data. */
#define CHAT_DATA_GROUP			\
    (sizeof(u_int8_t) + sizeof(u_int64_t) + sizeof(u_int32_t))

/* Max size of a packet we put on the wire. */
#define CHAT_WIRE_MAX			\
    (VIGIL_CHAT_DATA_MAX + sizeof(struct vigil_chat_data_head) + \
    CHAT_DATA_GROUP + sizeof(u_int64_t))

/* Max size for a single chunk of data. */
#define CHAT_CHUNKS_MAX			\
    ((CHAT_WIRE_MAX + VIGIL_CHAT_CHUNK_DATA - 1) / VIGIL_CHAT_CHUNK_DATA)

/* The rate at which we start sending data chunks and its upper bound. */
#define CHAT_RATE_START			200
#define CHAT_RATE_MIN			50
#define CHAT_RATE_MAX			8000

/* How many data chunks we will send in one go. */
#define CHAT_CHUNK_BURST		48

/* How often we send acks for chunks or ack-delays. */
#define CHAT_ACK_CHUNKS			16
#define CHAT_ACK_DELAY			20

/* The RTT we assume we have at the start of a link. */
#define CHAT_RTT_START			300

/* Time before we ask for an ack of data we sent. */
#define CHAT_DATA_PROBE			(2 * 1000)

/* Time before we resend un-acked data. */
#define CHAT_DATA_STALL			(20 * 1000)

/* Max size of transfers. */
#define CHAT_RXFERS_MAX			4

/* Max size of the data we hold for our platform code. */
#define CHAT_PARCELS_MAX		(16 * 1024 * 1024)

/* Max size of the data we hold for sending. */
#define CHAT_BLOBS_MAX			(12 * 1024 * 1024)

/* Time the platform gets to pull data that arrived. */
#define CHAT_DATA_WAIT			(60 * 1000)

/* If we talk to gospel, don't send images. */
#define CHAT_DATA_GOSPEL		\
    "I sent you %s, but you are on gospel so you cannot open it, too bad."

/* The name our avatar goes under. */
#define CHAT_AVATAR_NAME		"avatar"

/*
 * A data block we could be sending.
 */
struct chat_blob {
	int			held;
	u_int8_t		peer;
	u_int64_t		ref;
	size_t			size;
	u_int32_t		refs;
	u_int32_t		count;
	u_int8_t		*data;

	TAILQ_ENTRY(chat_blob)	list;
};

TAILQ_HEAD(chat_blob_list, chat_blob);

/*
 * The message a reply is for, who wrote it and its hash. A hash of 0 means
 * there is no reply and an author of 0 is us.
 */
struct chat_quote {
	u_int8_t		author;
	u_int64_t		hash;
};

/*
 * The topic a message is in, an id of 0 means none. A message that sets a
 * topic carries the name of the topic as its text, no name removes it.
 */
struct chat_topic {
	int			set;
	u_int64_t		id;
	u_int32_t		color;
};

/*
 * The group a message is in, an id of 0 means none. The type is the type
 * of frame it goes in, a roster carries the name of the group as its text.
 */
struct chat_group {
	u_int8_t		type;
	u_int8_t		owner;
	u_int8_t		state;
	u_int64_t		id;
	u_int32_t		serial;
	u_int8_t		members[VIGIL_CHAT_GROUP_ROSTER];
};

/*
 * A message we can send to the peer. These wait in the outbox of our peer
 * until they are ack'd and potentially on the sendq of the active tunnel
 * for said peer.
 */
struct chat_msg {
	u_int64_t		id;
	u_int64_t		ref;
	u_int64_t		age;
	int			sent;
	int			pending;

	struct chat_blob	*blob;
	struct chat_quote	quote;
	struct chat_topic	topic;
	struct chat_group	group;

	struct vigil_chat_frame	frame;

	TAILQ_ENTRY(chat_msg)	wlist;
	TAILQ_ENTRY(chat_msg)	slist;
};

TAILQ_HEAD(chat_msg_list, chat_msg);

/*
 * A message we received and handed to the platform.
 */
struct chat_seen {
	u_int64_t		id;
	u_int64_t		age;
	u_int64_t		hash;
	u_int64_t		token;
	u_int64_t		avatar;
	size_t			size;
};

/*
 * An active transfer of data.
 */
struct chat_xfer {
	struct chat_msg		*msg;
	u_int64_t		*sent;
	u_int8_t		*acked;

	int			done;

	u_int32_t		base;
	u_int32_t		next;
	u_int32_t		scan;
	u_int32_t		rate;

	u_int64_t		rtt;
	u_int64_t		tick;
	u_int64_t		probe;
	u_int64_t		credit;
	u_int64_t		rtt_min;
	u_int64_t		last_ack;
};

/*
 * The active transfer on the receiver side.
 */
struct chat_rxfer {
	u_int64_t		id;
	size_t			size;
	u_int8_t		*bits;
	u_int8_t		*data;

	u_int32_t		got;
	u_int32_t		base;
	u_int32_t		count;
	u_int32_t		latest;
	u_int32_t		unacked;

	u_int64_t		last;
	u_int64_t		ack_due;
};

/*
 * A parcel holds the data that we are receiving until we can give
 * it to the platform code.
 */
struct chat_parcel {
	size_t			off;
	u_int64_t		age;
	u_int64_t		token;
	u_int64_t		topic;
	struct chat_group	group;

	size_t			size;
	u_int8_t		*data;

	TAILQ_ENTRY(chat_parcel)	list;
};

TAILQ_HEAD(chat_parcel_list, chat_parcel);

/*
 * A tunnel to a peer.
 */
struct chat_tunnel {
	struct vigil_io		io;

	KYRKA			*ctx;
	struct chat_peer	*peer;

	struct sockaddr_in	addr;
	struct sockaddr_in	from;
	struct vigil_cathedral	cathedral;
	struct sockaddr_in	p2p_peer;
	struct sockaddr_in	p2p_failed;

	int			typed;
	int			heard;
	int			typing;
	int			greeted;

	u_int8_t		features;
	u_int8_t		typed_owner;
	u_int8_t		typing_owner;
	u_int64_t		typed_group;
	u_int64_t		typed_topic;
	u_int64_t		typing_group;
	u_int64_t		typing_topic;

	u_int64_t		msgid;
	u_int64_t		online;
	u_int64_t		p2p_rx;
	u_int64_t		last_rx;
	u_int64_t		holepunch;
	u_int64_t		local_uid;
	u_int64_t		p2p_retry;
	u_int64_t		p2p_verify;
	u_int64_t		remote_uid;
	u_int64_t		p2p_backoff;
	u_int64_t		next_manage;
	u_int64_t		next_notify;
	u_int64_t		next_heartbeat;

	u_int32_t		seen_next;
	struct chat_seen	seen[CHAT_SEEN_MAX];

	struct chat_msg_list	sendq;

	struct chat_xfer	*xfer;
	struct chat_rxfer	*rxfer;

	struct chat_msg		*avatar;
	u_int64_t		avatar_have;
	u_int32_t		avatar_serial;
};

/*
 * A chat peer.
 */
struct chat_peer {
	u_int8_t		id;
	u_int64_t		active;
	u_int64_t		pushed;
	u_int64_t		avatar_held;
	int			present;
	int			faceless;
	int			reported;

	struct chat_msg_list	outbox;
	struct chat_tunnel	*tunnel;

	char			nick[VIGIL_CHAT_NICK_MAX + 1];

	char			name[VIGIL_INSTANCE_MAX];
	char			seen[VIGIL_INSTANCE_MAX + 1];
	u_int8_t		signature[VIGIL_SIGNATURE_LENGTH];
};

/*
 * The signaling liturgy.
 */
struct chat_liturgy {
	struct vigil_io		io;

	KYRKA			*ctx;
	u_int64_t		last;
	u_int16_t		group;
	u_int64_t		notify;
	u_int64_t		updated;

	u_int64_t		rx;
	u_int64_t		tx;

	struct vigil_cathedral	cathedral;
};

/*
 * A command the platform sent to us.
 */
struct chat_command {
	int				type;
	u_int8_t			peer;
	u_int64_t			value;

	size_t				len;
	struct chat_blob		*blob;
	struct chat_quote		quote;
	struct chat_topic		topic;
	struct chat_group		group;
	u_int8_t			data[VIGIL_CHAT_MESSAGE_MAX];

	TAILQ_ENTRY(chat_command)	list;
};

TAILQ_HEAD(chat_command_list, chat_command);

/*
 * An event from our code to the platform.
 */
struct chat_event {
	int				type;
	size_t				size;
	u_int8_t			kind;
	u_int8_t			peer;
	u_int64_t			value;
	int				mention;

	struct chat_quote		quote;
	struct chat_topic		topic;
	struct chat_group		group;
	char				name[VIGIL_INSTANCE_MAX];
	char				nick[VIGIL_CHAT_NICK_MAX + 1];
	char				text[VIGIL_CHAT_MESSAGE_MAX + 1];

	TAILQ_ENTRY(chat_event)		list;
};

TAILQ_HEAD(chat_event_list, chat_event);

static void	chat_liturgy_read(void *);
static void	chat_liturgy_signal(void);
static void	chat_liturgy_rebind(struct chat_liturgy *);
static void	chat_liturgy_cleanup(struct chat_liturgy *);
static int	chat_liturgy_init(struct chat_liturgy *, u_int16_t);
static void	chat_liturgy_cathedral_check(struct chat_liturgy *);
static void	chat_liturgy_event(KYRKA *, union kyrka_event *, void *);
static void	chat_liturgy_send(struct kyrka_packet *, u_int64_t, void *);

static void	chat_group_frame(struct vigil_chat_frame *,
		    const struct chat_msg *);
static void	chat_group_recv(struct chat_tunnel *, u_int64_t,
		    u_int8_t *, u_int16_t);
static void	chat_group_roster(struct chat_tunnel *, u_int64_t,
		    struct chat_group *, u_int8_t *, u_int16_t);

static void	chat_tunnel_read(void *);
static void	chat_tunnel_free(struct chat_tunnel *);
static void	chat_tunnel_poke(struct chat_tunnel *);
static void	chat_tunnel_manage(struct chat_tunnel *);
static void	chat_tunnel_rebind(struct chat_tunnel *);
static int	chat_tunnel_ack(struct chat_tunnel *, u_int64_t);
static void	chat_tunnel_check(struct chat_peer *);
static int	chat_tunnel_online(struct chat_tunnel *);
static void	chat_tunnel_event(KYRKA *, union kyrka_event *, void *);
static void	chat_tunnel_state(struct chat_tunnel *,
		    const char **, int *, int *);

static struct chat_tunnel	*chat_tunnel_new(struct chat_peer *,
				    struct vigil_cathedral *);

static void	chat_cathedral_alive(struct chat_tunnel *);
static void	chat_cathedral_check(struct chat_tunnel *);
static void	chat_cathedral_send(struct kyrka_packet *, u_int64_t, void *);

static void	chat_p2p_fail(struct chat_tunnel *);
static void	chat_p2p_follow(struct chat_tunnel *);
static void	chat_p2p_abandon(struct chat_tunnel *);

static void	chat_avatar_update(void);
static void	chat_avatar_drop(struct chat_tunnel *);
static void	chat_avatar_push(struct chat_tunnel *);
static void	chat_avatar_check(struct chat_peer *, u_int8_t);

static u_int64_t	chat_avatar_hash(const u_int8_t *, size_t);

static void	chat_token_ack(u_int64_t);
static int	chat_nick_valid(const char *);
static void	chat_name_report(struct chat_peer *);
static int	chat_signal_check(struct chat_peer *);
static void	chat_utf8_normalize(u_int8_t *, size_t);
static void	chat_ambry_report(struct chat_tunnel *, u_int32_t);
static void	chat_typing_state(u_int8_t *, u_int64_t *, u_int64_t *);
static void	chat_heaven_input(struct kyrka_packet *, u_int64_t, void *);
static void	chat_hb_recv(struct chat_tunnel *, struct vigil_chat_frame *);
static void	chat_purgatory_input(struct kyrka_packet *, u_int64_t, void *);
static void	chat_hb_name(struct chat_peer *, const struct vigil_chat_hb *);
static void	chat_text_recv(struct chat_tunnel *, u_int64_t,
		    const struct chat_quote *, const struct chat_topic *,
		    const struct chat_group *, u_int8_t *, u_int16_t);
static int	chat_text_empty(const struct chat_topic *,
		    const struct chat_group *);
static int	chat_text_report(struct chat_tunnel *, u_int64_t,
		    const struct chat_quote *, const struct chat_topic *,
		    const struct chat_group *, const u_int8_t *, size_t);

static void	chat_ack_recv(struct chat_tunnel *, u_int64_t);
static void	chat_ack_queue(struct chat_tunnel *, u_int64_t);

static u_int64_t	chat_frame_hash(const u_int8_t *, size_t);
static void		chat_frame_send(struct chat_tunnel *,
			    struct chat_msg *);

static void	chat_msg_free(struct chat_msg *);
static void	chat_msg_resend(struct chat_tunnel *);
static void	chat_msg_queue(struct chat_tunnel *, struct chat_msg *);
static int	chat_msg_waits(struct chat_tunnel *, struct chat_msg *);

static void	chat_outbox_free(struct chat_peer *);
static void	chat_outbox_add(struct chat_peer *, u_int64_t,
		    const struct chat_quote *, const struct chat_topic *,
		    const struct chat_group *, const u_int8_t *, size_t);
static void	chat_outbox_data(struct chat_peer *, u_int64_t,
		    struct chat_blob *);

static void	chat_commands_run(void);
static void	chat_commands_free(void);
static void	chat_command_free(struct chat_command *);
static void	chat_command_next(struct chat_command **);
static int	chat_command_insert(struct chat_command *);
static int	chat_command_queue(int, u_int8_t, u_int64_t,
		    const struct chat_quote *, const struct chat_topic *,
		    const struct chat_group *, const void *, size_t);

static void	chat_reply_frame(struct vigil_chat_frame *,
		    const struct chat_msg *);
static void	chat_reply_recv(struct chat_tunnel *, u_int64_t,
		    const struct chat_group *, u_int8_t *, u_int16_t);

static void	chat_topic_frame(struct chat_tunnel *,
		    struct vigil_chat_frame *, const struct chat_msg *);
static void	chat_topic_recv(struct chat_tunnel *, u_int64_t,
		    const struct chat_group *, u_int8_t *, u_int16_t);
static void	chat_topic_text_recv(struct chat_tunnel *, u_int64_t,
		    const struct chat_group *, u_int8_t *, u_int16_t);

static void	chat_blob_release(struct chat_blob *);
static int	chat_blob_room(u_int8_t, u_int64_t, size_t);

static void	chat_parcels_free(void);
static void	chat_parcels_expire(void);
static void	chat_parcel_free(struct chat_parcel *);

static int	chat_data_kind_valid(int);
static int	chat_data_name_valid(const u_int8_t *, size_t);
static void	chat_data_gospel(struct chat_tunnel *, struct chat_msg *);
static int	chat_data_arrived(struct chat_tunnel *, struct chat_rxfer *);
static void	chat_data_discard(struct chat_tunnel *, struct chat_rxfer *);
static int	chat_data_report(struct chat_tunnel *, struct chat_parcel *);
static void	chat_data_ack(struct chat_tunnel *, u_int64_t,
		    struct chat_rxfer *, u_int32_t);
static int	chat_data_queue(u_int8_t, u_int64_t, const struct chat_group *,
		    u_int64_t, int, const char *, const void *, size_t);
static int	chat_data_tags(struct chat_rxfer *, u_int8_t, size_t *,
		    struct chat_group *, u_int64_t *);

static const char	*chat_data_kind_name(int);
static struct chat_msg	*chat_data_msg(u_int64_t, struct chat_blob *);

static void	chat_xfer_run(struct chat_tunnel *);
static void	chat_xfer_free(struct chat_tunnel *);
static void	chat_xfer_pump(struct chat_tunnel *);
static void	chat_xfer_restart(struct chat_xfer *);
static void	chat_xfer_pace(struct chat_xfer *, u_int64_t);
static void	chat_xfer_new(struct chat_tunnel *, struct chat_msg *);
static int	chat_xfer_pick(struct chat_xfer *, u_int32_t, u_int32_t *);
static void	chat_xfer_ack(struct chat_tunnel *, u_int64_t,
		    const u_int8_t *);

static void	chat_rxfer_free(struct chat_tunnel *);
static void	chat_rxfer_check(struct chat_tunnel *);

static struct chat_rxfer	*chat_rxfer_new(struct chat_tunnel *, u_int64_t,
				    u_int32_t, u_int32_t);

static void	chat_chunk_send(struct chat_tunnel *, u_int32_t);
static void	chat_chunk_recv(struct chat_tunnel *, const void *);

static void	chat_bits_set(u_int8_t *, u_int32_t);
static int	chat_bits_test(const u_int8_t *, u_int32_t);

static int	chat_mention_char(char);
static void	chat_mention_check(struct chat_event *);
static int	chat_mention_name(const char *, const char *);

static void	chat_events_free(void);
static int	chat_event_insert(struct chat_event *);
static void	chat_events_take(struct chat_event_list *, size_t);
static void	chat_event_render(struct vigil_buf *, struct chat_event *, int);
static int	chat_event_queue(int, struct chat_peer *, u_int64_t,
		    const u_int8_t *, size_t);

/* Are we alive? */
static int				running = 0;

/* Each time we start we increase the session counter. */
static u_int64_t			session = 0;

/* If we are accepting commands or not. */
static volatile int			accepting = 0;

/* Incremental token id for the messages we receive. */
static u_int64_t			tokens = 0;

/* The signaling liturgy for the chat. */
static struct chat_liturgy		signaling;

/* What we last told the cathedral in the signaling liturgy. */
static u_int8_t				sent[KYRKA_PEERS_PER_FLOCK];

/* All peers we can chat with, populated from discovery. */
static struct chat_peer			peers[KYRKA_PEERS_PER_FLOCK];

/* The peers whose chat is open and those we type to. */
static volatile u_int8_t		opened[KYRKA_PEERS_PER_FLOCK];
static volatile u_int8_t		typing[KYRKA_PEERS_PER_FLOCK];

/* Protects our commands, events, parcels and blobs. */
static pthread_mutex_t			lock = PTHREAD_MUTEX_INITIALIZER;

/* Our nickname, empty if we have none. */
static char				nick[VIGIL_CHAT_NICK_MAX + 1];

/* The group and topic we are typing in, an id of 0 means none. */
static u_int8_t				typing_owner = 0;
static u_int64_t			typing_group = 0;
static u_int64_t			typing_topic = 0;

/* Set when our flock signed our instance, we then send it to peers. */
static int				named = 0;

/* Our avatar as the platform gave it, the serial counts its changes. */
static size_t				avatar_len = 0;
static u_int32_t			avatar_serial = 0;
static u_int8_t				avatar[VIGIL_CHAT_AVATAR_MAX];

/* The avatar we send to our peers, the serial it came from and its hash. */
static struct chat_blob			*portrait = NULL;
static u_int64_t			portrait_hash = 0;
static u_int32_t			portrait_serial = 0;

/* The commands for the event loop. */
static struct chat_command_list		commands;
static size_t				command_count = 0;

/* The events for the platform. */
static struct chat_event_list		events;
static size_t				event_count = 0;

/* All parcels that arrived and we currently hold. */
static struct chat_parcel_list		parcels;
static size_t				parcel_bytes = 0;

/* All data we hold for sending and how much of it. */
static struct chat_blob_list		blobs;
static size_t				blob_bytes = 0;

/* Number of xfers in-flight and how many of those we are receiving. */
static u_int32_t			transfers = 0;
static u_int32_t			receiving = 0;

/*
 * Setup the chat part of gospel.
 */
void
vigil_chat_init(void)
{
	int		idx;

	PRECOND(running == 0);
	PRECOND(vigil->gospel != NULL);
	PRECOND(vigil->gospel->mode == VIGIL_MODE_GOSPEL);

	memset(sent, 0, sizeof(sent));
	memset(&signaling, 0, sizeof(signaling));

	for (idx = 0; idx < KYRKA_PEERS_PER_FLOCK; idx++) {
		memset(&peers[idx], 0, sizeof(peers[idx]));
		TAILQ_INIT(&peers[idx].outbox);
		peers[idx].id = idx;
	}

	named = 0;

	if (vigil->gospel->keys & VIGIL_CONFIG_KEY_NAME_SIGNATURE) {
		if (vigil_config_name_verify(vigil->gospel,
		    vigil->gospel->spi & 0xff, vigil->gospel->instance,
		    vigil->gospel->name_signature,
		    sizeof(vigil->gospel->name_signature)) == 0) {
			named = 1;
		} else {
			vigil_log(LOG_NOTICE,
			    "[chat] our instance does not match its signature");
		}
	}

	portrait = NULL;
	portrait_serial = 0;
	portrait_hash = 0;

	vigil_lock(&lock);

	transfers = 0;
	blob_bytes = 0;
	event_count = 0;
	command_count = 0;

	TAILQ_INIT(&blobs);
	TAILQ_INIT(&events);
	TAILQ_INIT(&parcels);
	TAILQ_INIT(&commands);

	vigil_unlock(&lock);

	if (chat_liturgy_init(&signaling, VIGIL_CHAT_SIGNAL_GROUP) == -1) {
		vigil_log(LOG_NOTICE, "chats are not available");
		return;
	}

	session++;
	running = 1;

	vigil_atomic_write(&accepting, 1);
	vigil_log(LOG_INFO, "chats running");
}

/*
 * Cleanup any resources allocated by the chat.
 */
void
vigil_chat_cleanup(void)
{
	int		idx;

	if (!running)
		return;

	vigil_atomic_write(&accepting, 0);

	for (idx = 0; idx < KYRKA_PEERS_PER_FLOCK; idx++) {
		if (peers[idx].tunnel != NULL)
			chat_tunnel_free(peers[idx].tunnel);
		chat_outbox_free(&peers[idx]);
	}

	if (portrait != NULL) {
		chat_blob_release(portrait);
		portrait = NULL;
		portrait_hash = 0;
	}

	chat_liturgy_cleanup(&signaling);
	chat_commands_free();
	chat_parcels_free();
	chat_events_free();

	running = 0;
}

/*
 * Process the commands the platform has sent us.
 */
void
vigil_chat_update(void)
{
	int			idx;
	u_int8_t		local;

	if (!running)
		return;

	chat_commands_run();
	chat_avatar_update();
	chat_parcels_expire();

	local = vigil->gospel->spi & 0xff;

	chat_liturgy_cathedral_check(&signaling);

	for (idx = 1; idx < KYRKA_PEERS_PER_FLOCK; idx++) {
		if (idx == local)
			continue;

		if (peers[idx].present &&
		    (vigil->now - peers[idx].pushed) >=
		    VIGIL_CALL_PRESENT_EXPIRE) {
			vigil_log(LOG_INFO, "[chat] %02x is gone", idx);
			peers[idx].present = 0;
		}

		chat_tunnel_check(&peers[idx]);
	}

	chat_liturgy_signal();
}

/*
 * Rebind everything, network shifted below us.
 */
void
vigil_chat_rebind(void)
{
	int		idx;

	if (!running)
		return;

	chat_liturgy_rebind(&signaling);

	for (idx = 0; idx < KYRKA_PEERS_PER_FLOCK; idx++) {
		if (peers[idx].tunnel != NULL)
			chat_tunnel_rebind(peers[idx].tunnel);
	}
}

/*
 * Notify all of our cathedrals.
 */
void
vigil_chat_poke(void)
{
	int		idx;

	if (!running)
		return;

	signaling.notify = 0;

	for (idx = 0; idx < KYRKA_PEERS_PER_FLOCK; idx++) {
		if (peers[idx].tunnel != NULL)
			chat_tunnel_poke(peers[idx].tunnel);
	}
}

/*
 * Queue a message for the given peer to be sent.
 */
int
vigil_chat_send(u_int8_t peer, u_int64_t ref, const char *text, size_t len)
{
	int		ret;

	PRECOND(peer > 0 && peer < KYRKA_PEERS_PER_FLOCK);
	PRECOND(text != NULL);
	PRECOND(len > 0 && len <= VIGIL_CHAT_MESSAGE_MAX);

	ret = chat_command_queue(CHAT_COMMAND_SEND,
	    peer, ref, NULL, NULL, NULL, text, len);

	return (ret);
}

/*
 * Queue a reply for the given peer to be sent, author is who wrote the
 * message we reply to (0 for us) and hash is the hash of that message.
 */
int
vigil_chat_reply(u_int8_t peer, u_int64_t ref, u_int8_t author,
    u_int64_t hash, const char *text, size_t len)
{
	int			ret;
	struct chat_quote	quote;

	PRECOND(peer > 0 && peer < KYRKA_PEERS_PER_FLOCK);
	PRECOND(hash != 0);
	PRECOND(text != NULL);
	PRECOND(len > 0 && len <= VIGIL_CHAT_REPLY_MAX);

	quote.hash = hash;
	quote.author = author;

	ret = chat_command_queue(CHAT_COMMAND_SEND,
	    peer, ref, &quote, NULL, NULL, text, len);

	return (ret);
}

/*
 * Queue the given topic for the given peer, no name removes the topic.
 */
int
vigil_chat_topic(u_int8_t peer, u_int64_t ref, u_int64_t id, u_int32_t color,
    const char *name, size_t len)
{
	int			ret;
	struct chat_topic	topic;

	PRECOND(peer > 0 && peer < KYRKA_PEERS_PER_FLOCK);
	PRECOND(id != 0);
	PRECOND(name != NULL || len == 0);
	PRECOND(len <= VIGIL_CHAT_TOPIC_NAME_MAX);

	memset(&topic, 0, sizeof(topic));

	topic.id = id;
	topic.set = 1;
	topic.color = color & 0xffffff;

	ret = chat_command_queue(CHAT_COMMAND_SEND,
	    peer, ref, NULL, &topic, NULL, name, len);

	return (ret);
}

/*
 * Queue a message in the given topic for the given peer, author and hash
 * are as in vigil_chat_reply() and a hash of 0 means it replies to nothing.
 */
int
vigil_chat_topic_send(u_int8_t peer, u_int64_t ref, u_int64_t id,
    u_int8_t author, u_int64_t hash, const char *text, size_t len)
{
	int			ret;
	struct chat_quote	quote;
	struct chat_topic	topic;

	PRECOND(peer > 0 && peer < KYRKA_PEERS_PER_FLOCK);
	PRECOND(id != 0);
	PRECOND(text != NULL);
	PRECOND(len > 0 && len <= VIGIL_CHAT_TOPIC_MAX);

	memset(&quote, 0, sizeof(quote));
	memset(&topic, 0, sizeof(topic));

	topic.id = id;
	quote.hash = hash;
	quote.author = author;

	ret = chat_command_queue(CHAT_COMMAND_SEND,
	    peer, ref, &quote, &topic, NULL, text, len);

	return (ret);
}

/*
 * Queue the roster of our group with the given id for the given peer, a bit
 * in members for every member. A peer that is not in it is no longer.
 */
int
vigil_chat_group_roster(u_int8_t peer, u_int64_t ref, u_int64_t id,
    u_int32_t serial, int state, const u_int8_t *members, const char *name,
    size_t len)
{
	int			ret;
	struct chat_group	group;

	PRECOND(peer > 0 && peer < KYRKA_PEERS_PER_FLOCK);
	PRECOND(id != 0);
	PRECOND(state == VIGIL_CHAT_GROUP_OPEN ||
	    state == VIGIL_CHAT_GROUP_ENDED);
	PRECOND(members != NULL);
	PRECOND(name != NULL || len == 0);
	PRECOND(len <= VIGIL_CHAT_GROUP_NAME_MAX);

	memset(&group, 0, sizeof(group));

	group.id = id;
	group.state = state;
	group.serial = serial;
	group.owner = vigil->gospel->spi & 0xff;
	group.type = VIGIL_CHAT_TYPE_ROSTER;
	memcpy(group.members, members, sizeof(group.members));

	ret = chat_command_queue(CHAT_COMMAND_SEND,
	    peer, ref, NULL, NULL, &group, name, len);

	return (ret);
}

/*
 * Queue a leave for the group with the given id that the given peer owns.
 */
int
vigil_chat_group_leave(u_int8_t owner, u_int64_t ref, u_int64_t id)
{
	int			ret;
	struct chat_group	group;

	PRECOND(owner > 0 && owner < KYRKA_PEERS_PER_FLOCK);
	PRECOND(id != 0);

	memset(&group, 0, sizeof(group));

	group.id = id;
	group.owner = owner;
	group.type = VIGIL_CHAT_TYPE_LEAVE;

	ret = chat_command_queue(CHAT_COMMAND_SEND,
	    owner, ref, NULL, NULL, &group, NULL, 0);

	return (ret);
}

/*
 * Queue a message in the given group for the given peer, in the given topic
 * if it is not 0, serial is that of the roster we know. Author and hash are
 * as in vigil_chat_reply() and a hash of 0 means it replies to nothing.
 */
int
vigil_chat_group_send(u_int8_t peer, u_int64_t ref, u_int8_t owner,
    u_int64_t id, u_int32_t serial, u_int64_t tid, u_int8_t author,
    u_int64_t hash, const char *text, size_t len)
{
	int			ret;
	size_t			max;
	struct chat_quote	quote;
	struct chat_group	group;
	struct chat_topic	topic;

	PRECOND(peer > 0 && peer < KYRKA_PEERS_PER_FLOCK);
	PRECOND(owner > 0 && owner < KYRKA_PEERS_PER_FLOCK);
	PRECOND(id != 0);
	PRECOND(text != NULL);

	if (tid != 0)
		max = VIGIL_CHAT_GROUP_TOPIC_MAX;
	else if (hash != 0)
		max = VIGIL_CHAT_GROUP_REPLY_MAX;
	else
		max = VIGIL_CHAT_GROUP_MAX;

	PRECOND(len > 0 && len <= max);

	memset(&quote, 0, sizeof(quote));
	memset(&group, 0, sizeof(group));
	memset(&topic, 0, sizeof(topic));

	topic.id = tid;

	group.id = id;
	group.owner = owner;
	group.serial = serial;
	group.type = VIGIL_CHAT_TYPE_TEXT;

	quote.hash = hash;
	quote.author = author;

	ret = chat_command_queue(CHAT_COMMAND_SEND,
	    peer, ref, &quote, &topic, &group, text, len);

	return (ret);
}

/*
 * Queue the given topic in the group with the given id that we own for
 * the given peer, no name removes the topic. Serial is that of our roster.
 */
int
vigil_chat_group_topic(u_int8_t peer, u_int64_t ref, u_int64_t id,
    u_int32_t serial, u_int64_t tid, u_int32_t color, const char *name,
    size_t len)
{
	int			ret;
	struct chat_group	group;
	struct chat_topic	topic;

	PRECOND(peer > 0 && peer < KYRKA_PEERS_PER_FLOCK);
	PRECOND(id != 0);
	PRECOND(tid != 0);
	PRECOND(name != NULL || len == 0);
	PRECOND(len <= VIGIL_CHAT_TOPIC_NAME_MAX);

	memset(&group, 0, sizeof(group));
	memset(&topic, 0, sizeof(topic));

	group.id = id;
	group.serial = serial;
	group.type = VIGIL_CHAT_TYPE_TEXT;
	group.owner = vigil->gospel->spi & 0xff;

	topic.id = tid;
	topic.set = 1;
	topic.color = color & 0xffffff;

	ret = chat_command_queue(CHAT_COMMAND_SEND,
	    peer, ref, NULL, &topic, &group, name, len);

	return (ret);
}

/*
 * Tell our peers we are typing in the given group and topic, an id of 0
 * means we are not typing in any group and a topic of 0 in none of its
 * topics.
 */
void
vigil_chat_group_typing(u_int8_t owner, u_int64_t id, u_int64_t topic)
{
	PRECOND(id == 0 || (owner > 0 && owner < KYRKA_PEERS_PER_FLOCK));

	if (id == 0) {
		owner = 0;
		topic = 0;
	}

	vigil_lock(&lock);
	typing_owner = owner;
	typing_group = id;
	typing_topic = topic;
	vigil_unlock(&lock);

	vigil_wake_signal();
}

/*
 * Send data to our peer.
 */
int
vigil_chat_data_send(u_int8_t peer, u_int64_t ref, int kind, const char *name,
    const void *data, size_t len)
{
	int		ret;

	PRECOND(peer > 0 && peer < KYRKA_PEERS_PER_FLOCK);
	PRECOND(name != NULL);
	PRECOND(data != NULL);
	PRECOND(len > 0 && len <= VIGIL_CHAT_DATA_MAX);

	ret = chat_data_queue(peer, ref, NULL, 0, kind, name, data, len);

	return (ret);
}

/*
 * Send data to our peer in the given group, in the given topic if it is
 * not 0, serial is that of the roster we know.
 */
int
vigil_chat_group_data_send(u_int8_t peer, u_int64_t ref, u_int8_t owner,
    u_int64_t id, u_int32_t serial, u_int64_t topic, int kind,
    const char *name, const void *data, size_t len)
{
	int			ret;
	struct chat_group	group;

	PRECOND(peer > 0 && peer < KYRKA_PEERS_PER_FLOCK);
	PRECOND(owner > 0 && owner < KYRKA_PEERS_PER_FLOCK);
	PRECOND(id != 0);
	PRECOND(name != NULL);
	PRECOND(data != NULL);
	PRECOND(len > 0 && len <= VIGIL_CHAT_DATA_MAX);

	memset(&group, 0, sizeof(group));

	group.id = id;
	group.owner = owner;
	group.serial = serial;

	ret = chat_data_queue(peer, ref, &group, topic, kind, name, data, len);

	return (ret);
}

/*
 * Send data to our peer in the given topic.
 */
int
vigil_chat_topic_data_send(u_int8_t peer, u_int64_t ref, u_int64_t id,
    int kind, const char *name, const void *data, size_t len)
{
	int		ret;

	PRECOND(peer > 0 && peer < KYRKA_PEERS_PER_FLOCK);
	PRECOND(id != 0);
	PRECOND(name != NULL);
	PRECOND(data != NULL);
	PRECOND(len > 0 && len <= VIGIL_CHAT_DATA_MAX);

	ret = chat_data_queue(peer, ref, NULL, id, kind, name, data, len);

	return (ret);
}

/*
 * Copy the data that arrived into the given buffer.
 */
ssize_t
vigil_chat_data_take(u_int64_t token, void *buf, size_t len)
{
	size_t			off;
	size_t			left;
	struct chat_parcel	*parcel;

	PRECOND(buf != NULL);

	vigil_lock(&lock);

	TAILQ_FOREACH(parcel, &parcels, list) {
		if (parcel->token == token)
			break;
	}

	if (parcel == NULL) {
		vigil_unlock(&lock);
		return (-1);
	}

	off = parcel->off;

	if (parcel->size < off) {
		vigil_unlock(&lock);
		return (-1);
	}

	left = parcel->size - off;

	if (left > len) {
		vigil_unlock(&lock);
		return (-1);
	}

	TAILQ_REMOVE(&parcels, parcel, list);
	parcel_bytes -= parcel->size;
	vigil_unlock(&lock);

	memcpy(buf, parcel->data + off, left);
	chat_parcel_free(parcel);

	return (left);
}

/*
 * Check if we need to speed up the event loop because xfers are in flight.
 */
int
vigil_chat_busy(void)
{
	if (!running)
		return (0);

	return (transfers > 0);
}

/*
 * Drop any and all messages we hold for the given peer.
 */
int
vigil_chat_forget(u_int8_t peer)
{
	int		ret;

	PRECOND(peer > 0 && peer < KYRKA_PEERS_PER_FLOCK);

	if (!vigil_atomic_read(&accepting))
		return (0);

	ret = chat_command_queue(CHAT_COMMAND_FORGET,
	    peer, 0, NULL, NULL, NULL, NULL, 0);

	return (ret);
}

/*
 * The platform stored the message, so go ahead and ack it.
 */
int
vigil_chat_stored(u_int64_t token)
{
	int		ret;

	ret = chat_command_queue(CHAT_COMMAND_STORED,
	    0, token, NULL, NULL, NULL, NULL, 0);

	return (ret);
}

/*
 * Mark the chat with the given peer as open or closed, we keep signaling
 * the peer while it is open.
 */
void
vigil_chat_open(u_int8_t peer, int on)
{
	PRECOND(peer > 0 && peer < KYRKA_PEERS_PER_FLOCK);
	PRECOND(on == 0 || on == 1);

	vigil_atomic_write(&opened[peer], on);
	vigil_wake_signal();
}

/*
 * Tell the given peer if we are typing, in the given topic if it is not 0.
 */
void
vigil_chat_typing(u_int8_t peer, u_int64_t topic, int on)
{
	PRECOND(peer > 0 && peer < KYRKA_PEERS_PER_FLOCK);
	PRECOND(on == 0 || on == 1);

	if (on == 0)
		topic = 0;

	vigil_lock(&lock);
	typing_topic = topic;
	vigil_unlock(&lock);

	vigil_atomic_write(&typing[peer], on);
	vigil_wake_signal();
}

/*
 * Set the nickname we announce to peers in heartbeats, if empty we clear it.
 */
int
vigil_chat_nick(const char *name)
{
	int		len;

	PRECOND(name != NULL);

	if (name[0] != '\0' && chat_nick_valid(name) == -1)
		return (-1);

	vigil_lock(&lock);
	memset(nick, 0, sizeof(nick));
	len = snprintf(nick, sizeof(nick), "%s", name);
	vigil_unlock(&lock);

	if (len < 0)
		return (-1);

	if ((size_t)len >= sizeof(nick))
		return (-1);

	return (0);
}

/*
 * Set the avatar we send to our peers, if empty we have none.
 */
void
vigil_chat_avatar(const void *data, size_t len)
{
	PRECOND(data != NULL || len == 0);
	PRECOND(len <= VIGIL_CHAT_AVATAR_MAX);

	vigil_lock(&lock);

	if (len == avatar_len && (len == 0 || !memcmp(avatar, data, len))) {
		vigil_unlock(&lock);
		return;
	}

	nyfe_mem_zero(avatar, sizeof(avatar));

	if (len > 0)
		memcpy(avatar, data, len);

	avatar_len = len;
	avatar_serial++;

	vigil_unlock(&lock);

	vigil_wake_signal();
}

/*
 * Grab all events currently pending and return them as JSON.
 */
size_t
vigil_chat_events(char *buf, size_t len)
{
	struct vigil_buf	out;
	struct chat_event	*evt;
	int			first;
	struct chat_event_list	taken;

	PRECOND(buf != NULL);

	if (len < VIGIL_CHAT_EVENTS_MAX)
		return (0);

	first = 1;
	vigil_buf_init(&out, buf, len);

	TAILQ_INIT(&taken);
	chat_events_take(&taken, (len - 2) / CHAT_EVENT_JSON_MAX);

	vigil_buf_appendf(&out, "[");

	while ((evt = TAILQ_FIRST(&taken)) != NULL) {
		TAILQ_REMOVE(&taken, evt, list);

		chat_event_render(&out, evt, first);
		first = 0;

		nyfe_mem_zero(evt, sizeof(*evt));
		free(evt);
	}

	vigil_buf_appendf(&out, "]");

	if (out.flags & VIGIL_BUF_FULL)
		return (0);

	return (out.offset);
}

/*
 * Append the state of our chats to the status, this includes our peers and
 * our tunnels.
 */
void
vigil_chat_status(struct vigil_buf *buf)
{
	int			idx;
	struct chat_tunnel	*tun;
	struct chat_msg		*msg;
	struct chat_peer	*peer;
	const char		*link;
	size_t			queued;
	int			first, online, p2p;
	char			name[VIGIL_CHAT_NICK_MAX + 1];

	PRECOND(buf != NULL);

	if (!running)
		return;

	vigil_buf_appendf(buf,
	    ",\"chat\":{\"session\":%" PRIu64 ",\"alive\":%s,\"id\":\"%02x\"",
	    session, signaling.last != 0 ? "true" : "false",
	    vigil->gospel->spi & 0xff);

	vigil_lock(&lock);
	memcpy(name, nick, sizeof(name));
	vigil_unlock(&lock);

	vigil_buf_appendf(buf, ",\"nick\":");
	vigil_buf_quote(buf, name);
	vigil_buf_appendf(buf, ",\"signed\":");
	vigil_buf_quote(buf, named ? vigil->gospel->instance : "");
	vigil_buf_appendf(buf, ",\"liturgies\":[");

	vigil_status_liturgy(buf, "chat", signaling.group,
	    &signaling.cathedral, signaling.rx, signaling.tx);

	first = 1;
	vigil_buf_appendf(buf, "],\"peers\":[");

	for (idx = 1; idx < KYRKA_PEERS_PER_FLOCK; idx++) {
		peer = &peers[idx];
		tun = peer->tunnel;

		queued = 0;
		TAILQ_FOREACH(msg, &peer->outbox, wlist)
			queued++;

		if (peer->present == 0 && tun == NULL && queued == 0)
			continue;

		chat_tunnel_state(tun, &link, &online, &p2p);
		vigil_buf_appendf(buf,
		    "%s{\"id\":\"%02x\",\"present\":%s,\"online\":%s",
		    first ? "" : ",", idx, peer->present ? "true" : "false",
		    online ? "true" : "false");

		vigil_buf_appendf(buf,
		    ",\"link\":\"%s\",\"typing\":%s,\"p2p\":%s"
		    ",\"queued\":%zu,\"nick\":", link,
		    online && tun->typing ? "true" : "false",
		    p2p ? "true" : "false", queued);

		vigil_buf_quote(buf, peer->nick);
		vigil_buf_appendf(buf, ",\"signed\":");
		vigil_buf_quote(buf, peer->name);

		if (online && tun->typing_group != 0) {
			vigil_buf_appendf(buf, ",\"typing_owner\":\"%02x\""
			    ",\"typing_group\":\"%016" PRIx64 "\"",
			    tun->typing_owner, tun->typing_group);
		}

		if (online && tun->typing_topic != 0) {
			vigil_buf_appendf(buf,
			    ",\"typing_topic\":\"%016" PRIx64 "\"",
			    tun->typing_topic);
		}

		vigil_buf_appendf(buf, "}");

		first = 0;
	}

	vigil_buf_appendf(buf, "]}");
}

/*
 * Setup a liturgy.
 */
static int
chat_liturgy_init(struct chat_liturgy *lit, u_int16_t id)
{
	struct kyrka_cathedral_cfg	cfg;
	struct vigil_config		*gospel;

	PRECOND(lit != NULL);
	PRECOND(lit->ctx == NULL);

	gospel = vigil->gospel;
	memset(lit, 0, sizeof(*lit));

	lit->io.fd = vigil_socket_udp();
	if (lit->io.fd == -1)
		return (-1);

	lit->group = id;
	lit->io.handle = chat_liturgy_read;
	vigil_io_schedule(&lit->io);

	lit->ctx = kyrka_ctx_alloc(chat_liturgy_event, lit);
	if (lit->ctx == NULL)
		fatal("failed to create chat liturgy context");

	memset(&cfg, 0, sizeof(cfg));

	cfg.group = id;
	cfg.udata = lit;
	cfg.send = chat_liturgy_send;
	cfg.tunnel = gospel->spi & 0xff;
	cfg.identity = gospel->cathedral_id;
	cfg.flock_src = gospel->cathedral_flock | VIGIL_CHAT_GROUP_DOMAIN;
	cfg.flock_dst = cfg.flock_src;

	if (id == VIGIL_CHAT_SIGNAL_GROUP)
		cfg.remembrance = 1;

	if (kyrka_cathedral_config(lit->ctx, &cfg) == -1) {
		fatal("failed to configure cathedral: %d",
		    kyrka_last_error(lit->ctx));
	}

	if (kyrka_cathedral_secret_load(lit->ctx, gospel->cathedral_secret,
	    sizeof(gospel->cathedral_secret)) == -1) {
		fatal("kyrka_cathedral_secret_load: %d",
		    kyrka_last_error(lit->ctx));
	}

	if (kyrka_cathedral_cosk_load(lit->ctx, gospel->cathedral_cosk,
	    sizeof(gospel->cathedral_cosk)) == -1) {
		fatal("kyrka_cathedral_cosk_load: %d",
		    kyrka_last_error(lit->ctx));
	}

	if (gospel->flags & VIGIL_CONFIG_FLAG_SHROUD) {
		if (kyrka_shroud_enable(lit->ctx) == -1) {
			fatal("failed to enable shroud: %d",
			    kyrka_last_error(lit->ctx));
		}
	}

	if (kyrka_mtu_size(lit->ctx, gospel->mtu) == -1) {
		fatal("failed to set libkyrka mtu size: %d",
		    kyrka_last_error(lit->ctx));
	}

	return (0);
}

/*
 * Let the cathedral know which peers we are interested in.
 */
static void
chat_liturgy_signal(void)
{
	int		idx;
	u_int8_t	want[KYRKA_PEERS_PER_FLOCK];

	if (signaling.cathedral.addr.sin_port == 0)
		return;

	for (idx = 0; idx < KYRKA_PEERS_PER_FLOCK; idx++)
		want[idx] = chat_signal_check(&peers[idx]);

	if (signaling.notify != 0 && vigil->now < signaling.notify) {
		if (!memcmp(want, sent, sizeof(want)))
			return;
		if ((vigil->now - signaling.updated) < VIGIL_CALL_NOTIFY_MIN)
			return;
	}

	signaling.updated = vigil->now;
	signaling.notify = vigil->now + VIGIL_CALL_NOTIFY;
	memcpy(sent, want, sizeof(sent));

	if (kyrka_cathedral_liturgy(signaling.ctx, want, sizeof(want)) == -1) {
		vigil_log(LOG_NOTICE, "[chat] kyrka_cathedral_liturgy: %d",
		    kyrka_last_error(signaling.ctx));
	}
}

/*
 * Check our current cathedral if it timed out and re-assign a new one.
 */
static void
chat_liturgy_cathedral_check(struct chat_liturgy *lit)
{
	int		idx;

	PRECOND(lit != NULL);

	if (lit->ctx == NULL)
		return;

	if (lit->cathedral.addr.sin_port == 0) {
		if (vigil_call_cathedral_pick(&lit->cathedral) == -1)
			return;

		if (lit == &signaling)
			lit->cathedral.timeout = VIGIL_CALL_TIMEOUT_INIT;

		vigil_log(LOG_INFO, "[chat] liturgy %04x via %s", lit->group,
		    vigil_address(&lit->cathedral.addr));
		lit->notify = 0;
		return;
	}

	if ((vigil->now - lit->cathedral.last) <= lit->cathedral.timeout)
		return;

	if (lit->last != 0) {
		vigil_log(LOG_NOTICE, "[chat] liturgy %04x timed out",
		    lit->group);
	}

	lit->last = 0;
	lit->cathedral.alive = 0;

	if (lit == &signaling) {
		for (idx = 0; idx < KYRKA_PEERS_PER_FLOCK; idx++)
			peers[idx].present = 0;
	}

	if (vigil_call_cathedral_pick(&lit->cathedral) == -1) {
		lit->cathedral.last = vigil->now;
		return;
	}

	if (lit == &signaling)
		lit->cathedral.timeout = VIGIL_CALL_TIMEOUT_INIT;

	vigil_log(LOG_NOTICE, "[chat] liturgy %04x switching to %s",
	    lit->group, vigil_address(&lit->cathedral.addr));
	lit->notify = 0;
}

/*
 * Give the given liturgy a new socket, if it is running.
 */
static void
chat_liturgy_rebind(struct chat_liturgy *lit)
{
	int		fd;

	PRECOND(lit != NULL);

	if (lit->ctx == NULL)
		return;

	fd = vigil_socket_udp();
	if (fd == -1)
		return;

	vigil_io_unschedule(&lit->io);
	vigil_socket_close(lit->io.fd);

	lit->notify = 0;
	lit->io.fd = fd;
	vigil_io_schedule(&lit->io);
}

/*
 * Release the given liturgy.
 */
static void
chat_liturgy_cleanup(struct chat_liturgy *lit)
{
	PRECOND(lit != NULL);

	if (lit->ctx == NULL)
		return;

	vigil_io_unschedule(&lit->io);
	vigil_socket_close(lit->io.fd);

	kyrka_ctx_free(lit->ctx);
	memset(lit, 0, sizeof(*lit));
}

/*
 * Callback for when an event occurred on the socket of a liturgy,
 * we push what we read into its libkyrka context.
 */
static void
chat_liturgy_read(void *udata)
{
	int			idx;
	size_t			len;
	ssize_t			ret;
	struct kyrka_packet	pkt;
	u_int8_t		*ptr;
	struct chat_liturgy	*lit;

	PRECOND(udata != NULL);

	lit = udata;

	if (!(lit->io.flags & VIGIL_IO_READABLE))
		return;

	for (idx = 0; idx < VIGIL_IO_BATCH; idx++) {
		ptr = kyrka_packet_recvbuf(lit->ctx, &pkt, &len);
		if (ptr == NULL)
			fatal("failed to get chat receive buffer");

		ret = read(lit->io.fd, ptr, len);
		if (ret == -1) {
			if (errno == EINTR)
				continue;
			if (errno != EWOULDBLOCK && errno != EAGAIN)
				vigil_log(LOG_NOTICE, "[chat] read: %s",
				    errno_s);
			lit->io.flags &= ~VIGIL_IO_READABLE;
			break;
		}

		if (ret == 0)
			continue;

		lit->rx += ret;
		vigil->gospel_rx += ret;

		pkt.length = ret;
		pkt.shroud = KYRKA_PACKET_SHROUD_CATHEDRAL;

		if (kyrka_purgatory_input(lit->ctx, &pkt) == -1) {
			vigil_log(LOG_NOTICE, "[chat] purgatory input: %d",
			    kyrka_last_error(lit->ctx));
		}
	}
}

/*
 * Callback for one of our liturgies.
 */
static void
chat_liturgy_event(KYRKA *ctx, union kyrka_event *evt, void *udata)
{
	struct chat_liturgy	*lit;
	int			idx;
	u_int8_t		local;

	PRECOND(ctx != NULL);
	PRECOND(evt != NULL);
	PRECOND(udata != NULL);

	lit = udata;
	local = vigil->gospel->spi & 0xff;

	if (evt->type == KYRKA_EVENT_LITURGY_RECEIVED ||
	    evt->type == KYRKA_EVENT_REMEMBRANCE_RECEIVED) {
		if (lit->last == 0) {
			vigil_log(LOG_INFO, "[chat] liturgy %04x is alive",
			    lit->group);
		}

		lit->last = vigil->now;
		lit->cathedral.alive = 1;
		lit->cathedral.last = vigil->now;
		lit->cathedral.timeout = VIGIL_CATHEDRAL_TIMEOUT;
	}

	switch (evt->type) {
	case KYRKA_EVENT_LITURGY_RECEIVED:
		for (idx = 1; idx < KYRKA_PEERS_PER_FLOCK; idx++) {
			if (idx == local)
				continue;

			/*
			 * The cathedral pushes us each peer that signals us
			 * on its own, peers expire in vigil_chat_update().
			 */
			if (evt->liturgy.peers[idx] == 0)
				continue;

			if (peers[idx].present == 0) {
				vigil_log(LOG_INFO,
				    "[chat] %02x is around", idx);
			}

			peers[idx].present = 1;
			peers[idx].pushed = vigil->now;
		}
		break;
	case KYRKA_EVENT_LOGMSG:
		vigil_log(LOG_INFO, "[chat] libkyrka: %s", evt->logmsg.log);
		break;
	default:
		break;
	}
}

/*
 * Callback from libkyrka when we have data to send to a cathedral.
 */
static void
chat_liturgy_send(struct kyrka_packet *pkt, u_int64_t magic, void *udata)
{
	size_t			len;
	u_int8_t		*ptr;
	struct sockaddr_in	*sin;
	struct chat_liturgy	*lit;

	PRECOND(pkt != NULL);
	PRECOND(udata != NULL);

	lit = udata;
	sin = &lit->cathedral.addr;

	if (sin->sin_port == 0)
		return;

	ptr = kyrka_packet_sendbuf(lit->ctx, pkt, &len);
	if (ptr == NULL)
		fatal("failed to get chat sendbuf");

	if (sendto(lit->io.fd, ptr, len, 0,
	    (const struct sockaddr *)sin, sizeof(*sin)) == -1) {
		if (errno != EAGAIN && errno != EWOULDBLOCK &&
		    !vigil_socket_unreachable(errno, 1))
			vigil_log(LOG_NOTICE, "[chat] sendto: %s", errno_s);
		return;
	}

	lit->tx += len;
	vigil->gospel_tx += len;
}

/*
 * Set up or tear down the tunnel with the given peer.
 *
 * We want a tunnel while the peer shows up in the signaling liturgy, it only
 * does while it signals us, so we keep a tunnel that is online for as long
 * as we want the peer too. Each tunnel uses a cathedral picked at random
 * from the ones we know.
 */
static void
chat_tunnel_check(struct chat_peer *peer)
{
	struct vigil_cathedral	cat;
	struct chat_tunnel	*tun;
	int			want;

	PRECOND(peer != NULL);

	tun = peer->tunnel;
	want = peer->present;

	if (chat_tunnel_online(tun) && chat_signal_check(peer))
		want = 1;

	if (want == 0) {
		if (tun != NULL)
			chat_tunnel_free(tun);
		return;
	}

	if (tun == NULL) {
		memset(&cat, 0, sizeof(cat));

		if (vigil_call_cathedral_pick(&cat) == -1)
			return;

		tun = chat_tunnel_new(peer, &cat);
	}

	if (tun != NULL)
		chat_tunnel_manage(tun);
}

/*
 * Set up a tunnel with the given peer via the given cathedral, any messages
 * we hold for the peer are queued right away.
 */
static struct chat_tunnel *
chat_tunnel_new(struct chat_peer *peer, struct vigil_cathedral *cathedral)
{
	struct kyrka_cathedral_cfg	cfg;
	struct chat_msg			*msg;
	struct chat_tunnel		*tun;
	u_int8_t			local;
	struct vigil_config		*gospel;

	PRECOND(peer != NULL);
	PRECOND(cathedral != NULL);
	PRECOND(peer->tunnel == NULL);

	gospel = vigil->gospel;
	local = gospel->spi & 0xff;

	tun = calloc(1, sizeof(*tun));
	if (tun == NULL)
		fatal("calloc: failed to allocate chat tunnel");

	tun->io.fd = vigil_socket_udp();
	if (tun->io.fd == -1) {
		free(tun);
		return (NULL);
	}

	TAILQ_INIT(&tun->sendq);

	tun->msgid = 1;
	tun->peer = peer;
	tun->io.handle = chat_tunnel_read;

	nyfe_random_bytes(&tun->local_uid, sizeof(tun->local_uid));

	memcpy(&tun->cathedral, cathedral, sizeof(*cathedral));
	memcpy(&tun->addr, &cathedral->addr, sizeof(cathedral->addr));

	tun->ctx = kyrka_ctx_alloc(chat_tunnel_event, tun);
	if (tun->ctx == NULL)
		fatal("failed to create chat context");

	memset(&cfg, 0, sizeof(cfg));

	cfg.udata = tun;
	cfg.send = chat_cathedral_send;
	cfg.tunnel = (local << 8) | peer->id;
	cfg.identity = gospel->cathedral_id;
	cfg.flock_src = gospel->cathedral_flock | VIGIL_CHAT_FLOCK_DOMAIN;
	cfg.flock_dst = cfg.flock_src;

	if (gospel->flags & VIGIL_CONFIG_FLAG_COMMIXTION)
		cfg.hops = VIGIL_COMMIXTION_HOPS;

	if (gospel->flags & VIGIL_CONFIG_FLAG_SHROUD) {
		if (kyrka_shroud_enable(tun->ctx) == -1) {
			fatal("failed to enable shroud: %d",
			    kyrka_last_error(tun->ctx));
		}
	}

	if (kyrka_cathedral_config(tun->ctx, &cfg) == -1) {
		fatal("failed to configure cathedral: %d",
		    kyrka_last_error(tun->ctx));
	}

	if (kyrka_device_kek_load(tun->ctx, gospel->kek,
	    sizeof(gospel->kek)) == -1) {
		fatal("kyrka_device_kek_load: %d", kyrka_last_error(tun->ctx));
	}

	if (kyrka_cathedral_secret_load(tun->ctx, gospel->cathedral_secret,
	    sizeof(gospel->cathedral_secret)) == -1) {
		fatal("kyrka_cathedral_secret_load: %d",
		    kyrka_last_error(tun->ctx));
	}

	if (kyrka_cathedral_cosk_load(tun->ctx, gospel->cathedral_cosk,
	    sizeof(gospel->cathedral_cosk)) == -1) {
		fatal("kyrka_cathedral_cosk_load: %d",
		    kyrka_last_error(tun->ctx));
	}

	if (kyrka_mtu_size(tun->ctx, gospel->mtu) == -1) {
		fatal("failed to set libkyrka mtu size: %d",
		    kyrka_last_error(tun->ctx));
	}

	if (kyrka_heaven_ifc(tun->ctx, chat_heaven_input, tun) == -1)
		fatal("failed to set heaven interface");

	if (kyrka_purgatory_ifc(tun->ctx, chat_purgatory_input, tun) == -1)
		fatal("failed to set purgatory interface");

	vigil_io_schedule(&tun->io);

	peer->tunnel = tun;

	TAILQ_FOREACH(msg, &peer->outbox, wlist)
		chat_msg_queue(tun, msg);

	vigil_log(LOG_INFO, "[chat] %02x tunnel via %s",
	    peer->id, vigil_address(&cathedral->addr));

	return (tun);
}

/*
 * The cathedral the given tunnel goes via answered us.
 */
static void
chat_cathedral_alive(struct chat_tunnel *tun)
{
	PRECOND(tun != NULL);

	tun->cathedral.last = vigil->now;
	tun->cathedral.timeout = VIGIL_CATHEDRAL_TIMEOUT;

	if (tun->cathedral.alive)
		return;

	tun->cathedral.alive = 1;
	vigil_log(LOG_INFO, "[chat] %02x cathedral %s is alive",
	    tun->peer->id, vigil_address(&tun->cathedral.addr));
}

/*
 * Move the given tunnel to another cathedral if the one it goes via
 * stopped answering us.
 */
static void
chat_cathedral_check(struct chat_tunnel *tun)
{
	int		relayed;

	PRECOND(tun != NULL);

	if ((vigil->now - tun->cathedral.last) <= tun->cathedral.timeout)
		return;

	if (tun->cathedral.alive) {
		vigil_log(LOG_NOTICE, "[chat] %02x cathedral %s timed out",
		    tun->peer->id, vigil_address(&tun->cathedral.addr));
	}

	tun->cathedral.alive = 0;
	relayed = vigil_inet_match(&tun->addr, &tun->cathedral.addr);

	if (vigil_call_cathedral_pick(&tun->cathedral) == -1) {
		tun->cathedral.last = vigil->now;
		return;
	}

	vigil_log(LOG_NOTICE, "[chat] %02x switching to cathedral %s",
	    tun->peer->id, vigil_address(&tun->cathedral.addr));

	if (relayed) {
		memcpy(&tun->addr, &tun->cathedral.addr,
		    sizeof(tun->cathedral.addr));
	}

	tun->next_notify = 0;
}

/*
 * Tear down the given tunnel. Messages that were not acked stay on the
 * outbox of the peer for the next tunnel.
 */
static void
chat_tunnel_free(struct chat_tunnel *tun)
{
	struct chat_msg		*msg;
	struct chat_peer	*peer;

	PRECOND(tun != NULL);
	PRECOND(tun->peer != NULL);

	peer = tun->peer;

	while ((msg = TAILQ_FIRST(&tun->sendq)) != NULL) {
		TAILQ_REMOVE(&tun->sendq, msg, slist);

		if (msg->frame.type != VIGIL_CHAT_TYPE_TEXT)
			free(msg);
	}

	chat_xfer_free(tun);
	chat_rxfer_free(tun);
	chat_avatar_drop(tun);

	TAILQ_FOREACH(msg, &peer->outbox, wlist) {
		msg->id = 0;
		msg->pending = 0;
	}

	vigil_io_unschedule(&tun->io);
	vigil_socket_close(tun->io.fd);

	kyrka_ctx_free(tun->ctx);

	vigil_log(LOG_INFO, "[chat] %02x tunnel closed", peer->id);
	peer->tunnel = NULL;

	nyfe_mem_zero(tun, sizeof(*tun));
	free(tun);
}

/*
 * Tell how the given tunnel reaches its peer, connecting until we hear from
 * our peer and relay or p2p after that.
 */
static void
chat_tunnel_state(struct chat_tunnel *tun, const char **link, int *online,
    int *p2p)
{
	PRECOND(link != NULL);
	PRECOND(online != NULL);
	PRECOND(p2p != NULL);

	*p2p = 0;
	*online = 0;
	*link = "none";

	if (tun == NULL)
		return;

	*online = chat_tunnel_online(tun);

	if (!vigil_inet_match(&tun->addr, &tun->cathedral.addr))
		*p2p = 1;

	if (*online == 0)
		*link = "connecting";
	else if (*p2p)
		*link = "p2p";
	else
		*link = "relay";
}

/*
 * Returns 1 if the given tunnel heard from its peer recently.
 */
static int
chat_tunnel_online(struct chat_tunnel *tun)
{
	if (tun == NULL)
		return (0);

	if (tun->online == 0)
		return (0);

	return ((vigil->now - tun->last_rx) < CHAT_RX_TIMEOUT);
}

/*
 * Manage keys, keep the cathedral informed and send what is on the sendq.
 *
 * We also send a heartbeat if our peer has not heard from us for a while or
 * when we started or stopped typing.
 */
static void
chat_tunnel_manage(struct chat_tunnel *tun)
{
	struct chat_msg		*msg;
	int			typed;
	u_int8_t		owner;
	u_int64_t		group;
	u_int64_t		topic;

	PRECOND(tun != NULL);
	PRECOND(tun->ctx != NULL);

	chat_cathedral_check(tun);

	if (tun->p2p_verify != 0 && vigil->now >= tun->p2p_verify) {
		vigil_log(LOG_INFO, "[chat] %02x p2p via %s did not work",
		    tun->peer->id, vigil_address(&tun->addr));
		chat_p2p_fail(tun);
	}

	if (vigil->now >= tun->next_notify) {
		tun->next_notify = vigil->now + CHAT_NOTIFY_INTERVAL;

		if (kyrka_cathedral_notify(tun->ctx) == -1) {
			vigil_log(LOG_NOTICE, "[chat] cathedral notify: %d",
			    kyrka_last_error(tun->ctx));
		}

		if ((vigil->gospel->flags & VIGIL_CONFIG_FLAG_P2P) &&
		    kyrka_cathedral_nat_detection(tun->ctx) == -1) {
			vigil_log(LOG_NOTICE, "[chat] nat detection: %d",
			    kyrka_last_error(tun->ctx));
		}
	}

	if (vigil->now >= tun->next_manage) {
		tun->next_manage = vigil->now + CHAT_MANAGE_INTERVAL;

		if (kyrka_key_manage(tun->ctx) == -1 &&
		    kyrka_last_error(tun->ctx) != KYRKA_ERROR_NO_SECRET) {
			vigil_log(LOG_NOTICE, "[chat] key manage: %d",
			    kyrka_last_error(tun->ctx));
		}
	}

	if (tun->online == 0)
		return;

	chat_msg_resend(tun);
	chat_avatar_push(tun);
	chat_xfer_run(tun);
	chat_rxfer_check(tun);

	typed = vigil_atomic_read(&typing[tun->peer->id]);
	chat_typing_state(&owner, &group, &topic);

	if (!tun->greeted || typed != tun->typed ||
	    owner != tun->typed_owner || group != tun->typed_group ||
	    topic != tun->typed_topic || vigil->now >= tun->next_heartbeat)
		chat_frame_send(tun, NULL);

	while ((msg = TAILQ_FIRST(&tun->sendq)) != NULL)
		chat_frame_send(tun, msg);
}

/*
 * Give the given tunnel a new socket and go back to the cathedral, the
 * network changed so any p2p path we had is gone.
 */
static void
chat_tunnel_rebind(struct chat_tunnel *tun)
{
	int		fd;

	PRECOND(tun != NULL);
	PRECOND(tun->ctx != NULL);

	fd = vigil_socket_udp();
	if (fd == -1)
		return;

	vigil_io_unschedule(&tun->io);
	vigil_socket_close(tun->io.fd);

	tun->io.fd = fd;
	vigil_io_schedule(&tun->io);

	tun->p2p_retry = 0;
	tun->p2p_verify = 0;
	tun->p2p_backoff = 0;
	tun->next_notify = 0;

	if (!vigil_inet_match(&tun->addr, &tun->cathedral.addr)) {
		memcpy(&tun->addr, &tun->cathedral.addr,
		    sizeof(tun->cathedral.addr));
		(void)kyrka_p2p_active(tun->ctx, 0);
	}
}

/*
 * Have the given tunnel manage keys, tell the cathedral about us and
 * send a frame on the next update.
 */
static void
chat_tunnel_poke(struct chat_tunnel *tun)
{
	PRECOND(tun != NULL);

	tun->next_heartbeat = 0;
	tun->next_manage = 0;
	tun->next_notify = 0;
}

/*
 * Callback for when an event occurred on the socket of a tunnel,
 * we push what we read into its libkyrka context.
 */
static void
chat_tunnel_read(void *udata)
{
	int			idx;
	size_t			len;
	ssize_t			ret;
	struct kyrka_packet	pkt;
	u_int8_t		*ptr;
	struct sockaddr_in	from;
	struct chat_tunnel	*tun;
	socklen_t		fromlen;

	PRECOND(udata != NULL);

	tun = udata;
	PRECOND(tun->ctx != NULL);

	if (!(tun->io.flags & VIGIL_IO_READABLE))
		return;

	tun->io.flags &= ~VIGIL_IO_READABLE;

	for (idx = 0; idx < VIGIL_IO_BATCH; idx++) {
		ptr = kyrka_packet_recvbuf(tun->ctx, &pkt, &len);
		if (ptr == NULL)
			fatal("failed to get chat receive buffer");

		fromlen = sizeof(from);

		ret = recvfrom(tun->io.fd, ptr, len, 0,
		    (struct sockaddr *)&from, &fromlen);
		if (ret == -1) {
			if (errno == EINTR)
				continue;
			if (errno != EWOULDBLOCK && errno != EAGAIN)
				vigil_log(LOG_NOTICE, "[chat] read: %s",
				    errno_s);
			break;
		}

		if (ret == 0)
			continue;

		vigil->gospel_rx += ret;
		pkt.length = ret;
		memcpy(&tun->from, &from, sizeof(from));

		if (vigil_inet_match(&from, &tun->cathedral.addr)) {
			chat_cathedral_alive(tun);
			pkt.shroud = KYRKA_PACKET_SHROUD_CATHEDRAL;
		} else {
			pkt.shroud = KYRKA_PACKET_SHROUD_PEER;
		}

		if (kyrka_purgatory_input(tun->ctx, &pkt) == -1 &&
		    kyrka_last_error(tun->ctx) != KYRKA_ERROR_NO_RX_KEY) {
			vigil_log(LOG_NOTICE, "[chat] %02x purgatory input: %d",
			    tun->peer->id, kyrka_last_error(tun->ctx));
		}
	}
}

/*
 * Callback from libkyrka when an event occurred on one of our tunnels.
 */
static void
chat_tunnel_event(KYRKA *ctx, union kyrka_event *evt, void *udata)
{
	struct sockaddr_in	sin;
	struct chat_tunnel	*tun;

	PRECOND(ctx != NULL);
	PRECOND(evt != NULL);
	PRECOND(udata != NULL);

	tun = udata;

	switch (evt->type) {
	case KYRKA_EVENT_KEYS_INFO:
		tun->next_notify = 0;

		if (evt->keys.tx_spi == 0)
			break;

		if (tun->online != 0)
			break;

		vigil_log(LOG_INFO, "[chat] %02x online", tun->peer->id);

		tun->online = vigil->now;
		tun->last_rx = vigil->now;
		break;
	case KYRKA_EVENT_KEYS_ERASED:
		vigil_log(LOG_INFO, "[chat] %02x keys erased", tun->peer->id);
		tun->p2p_verify = 0;
		memcpy(&tun->addr, &tun->cathedral.addr,
		    sizeof(tun->cathedral.addr));
		(void)kyrka_p2p_active(tun->ctx, 0);
		break;
	case KYRKA_EVENT_PEER_DISCOVERY:
		chat_cathedral_alive(tun);

		if (!(vigil->gospel->flags & VIGIL_CONFIG_FLAG_P2P))
			break;

		memset(&sin, 0, sizeof(sin));
		sin.sin_family = AF_INET;
		sin.sin_port = evt->peer.port;
		sin.sin_addr.s_addr = evt->peer.ip;

		/* No hairpinning and we will never reach a peer on loopback. */
		if ((evt->peer.flags & KYRKA_INFO_FLAG_SAME_EXTERNAL_IPV4) ||
		    (ntohl(sin.sin_addr.s_addr) >> 24) == 127) {
			memset(&tun->p2p_peer, 0, sizeof(tun->p2p_peer));
			memcpy(&sin, &tun->cathedral.addr, sizeof(sin));
		} else {
			memcpy(&tun->p2p_peer, &sin, sizeof(sin));
		}

		if (vigil->now < tun->p2p_retry &&
		    vigil_inet_match(&sin, &tun->p2p_failed))
			memcpy(&sin, &tun->cathedral.addr, sizeof(sin));

		if (vigil_inet_match(&sin, &tun->addr))
			break;

		memcpy(&tun->addr, &sin, sizeof(sin));

		if (vigil_inet_match(&sin, &tun->cathedral.addr)) {
			tun->p2p_verify = 0;
			(void)kyrka_p2p_active(tun->ctx, 0);
		} else {
			vigil_log(LOG_INFO, "[chat] %02x p2p via %s",
			    tun->peer->id, vigil_address(&sin));
			tun->p2p_rx = vigil->now;
			tun->p2p_verify = vigil->now + CHAT_P2P_VERIFY;
			tun->holepunch = vigil->now + CHAT_HOLEPUNCH;
			tun->next_heartbeat = vigil->now;
			(void)kyrka_p2p_active(tun->ctx, 1);
		}
		break;
	case KYRKA_EVENT_EXCHANGE_INFO:
		vigil_log(LOG_INFO, "[chat] exchange: %s",
		    evt->exchange.reason);
		break;
	case KYRKA_EVENT_AMBRY_RECEIVED:
		chat_ambry_report(tun, evt->ambry.generation);
		break;
	case KYRKA_EVENT_LOGMSG:
		vigil_log(LOG_INFO, "[chat] libkyrka: %s", evt->logmsg.log);
		break;
	default:
		break;
	}
}

/*
 * Callback from libkyrka with a decrypted frame from our peer.
 */
static void
chat_heaven_input(struct kyrka_packet *pkt, u_int64_t seq, void *udata)
{
	u_int64_t		id;
	u_int16_t		len;
	struct chat_tunnel	*tun;
	struct vigil_chat_frame	*frame;

	PRECOND(pkt != NULL);
	PRECOND(udata != NULL);

	tun = udata;
	tun->last_rx = vigil->now;

	if (vigil_inet_match(&tun->from, &tun->cathedral.addr))
		chat_p2p_abandon(tun);
	else
		chat_p2p_follow(tun);

	if (pkt->type != KYRKA_PACKET_TYPE_IP)
		return;

	if (pkt->length == sizeof(struct vigil_chat_chunk)) {
		chat_chunk_recv(tun, kyrka_packet_data(pkt));
		return;
	}

	if (pkt->length != sizeof(*frame))
		return;

	frame = kyrka_packet_data(pkt);

	id = be64toh(frame->id);
	len = be16toh(frame->len);

	if (id == 0)
		return;

	if (len > sizeof(frame->data))
		return;

	switch (frame->type) {
	case VIGIL_CHAT_TYPE_TEXT:
		chat_text_recv(tun, id, NULL, NULL, NULL, frame->data, len);
		break;
	case VIGIL_CHAT_TYPE_REPLY:
		chat_reply_recv(tun, id, NULL, frame->data, len);
		break;
	case VIGIL_CHAT_TYPE_TOPIC:
		chat_topic_recv(tun, id, NULL, frame->data, len);
		break;
	case VIGIL_CHAT_TYPE_TOPIC_TEXT:
		chat_topic_text_recv(tun, id, NULL, frame->data, len);
		break;
	case VIGIL_CHAT_TYPE_GROUP:
		chat_group_recv(tun, id, frame->data, len);
		break;
	case VIGIL_CHAT_TYPE_ACK:
		chat_ack_recv(tun, id);
		break;
	case VIGIL_CHAT_TYPE_HEARTBEAT:
		chat_hb_recv(tun, frame);
		break;
	case VIGIL_CHAT_TYPE_DATA_ACK:
		if (len == sizeof(struct vigil_chat_data_ack))
			chat_xfer_ack(tun, id, frame->data);
		break;
	default:
		break;
	}
}

/*
 * Callback from libkyrka with ciphertext to send to our peer.
 */
static void
chat_purgatory_input(struct kyrka_packet *pkt, u_int64_t seq, void *udata)
{
	size_t			len;
	u_int8_t		*ptr;
	struct chat_tunnel	*tun;

	PRECOND(pkt != NULL);
	PRECOND(udata != NULL);

	tun = udata;

	ptr = kyrka_packet_sendbuf(tun->ctx, pkt, &len);
	if (ptr == NULL)
		fatal("failed to get chat send buffer");

	if (sendto(tun->io.fd, ptr, len, 0,
	    (const struct sockaddr *)&tun->addr, sizeof(tun->addr)) == -1) {
		if (errno != EAGAIN && errno != EWOULDBLOCK &&
		    !vigil_socket_unreachable(errno,
		    vigil_inet_match(&tun->addr, &tun->cathedral.addr)))
			vigil_log(LOG_NOTICE, "[chat] sendto: %s", errno_s);
		return;
	}

	vigil->gospel_tx += len;
}

/*
 * Callback from libkyrka with data for the cathedral.
 */
static void
chat_cathedral_send(struct kyrka_packet *pkt, u_int64_t magic, void *udata)
{
	size_t			len;
	struct sockaddr_in	sin;
	u_int8_t		*ptr;
	struct chat_tunnel	*tun;

	PRECOND(pkt != NULL);
	PRECOND(udata != NULL);

	tun = udata;

	memcpy(&sin, &tun->cathedral.addr, sizeof(sin));

	if (magic == KYRKA_CATHEDRAL_NAT_MAGIC)
		sin.sin_port = htons(vigil->gospel->cathedral_nat_port);

	ptr = kyrka_packet_sendbuf(tun->ctx, pkt, &len);
	if (ptr == NULL)
		fatal("failed to get chat cathedral buffer");

	if (sendto(tun->io.fd, ptr, len, 0,
	    (const struct sockaddr *)&sin, sizeof(sin)) == -1) {
		if (errno != EAGAIN && errno != EWOULDBLOCK &&
		    !vigil_socket_unreachable(errno, 1))
			vigil_log(LOG_NOTICE, "[chat] sendto: %s", errno_s);
		return;
	}

	vigil->gospel_tx += len;
}

/*
 * We got a packet from our peer that did not come via the cathedral.
 *
 * If it came in on our p2p path that path works. If we are relaying and it
 * came from where the cathedral said our peer is, our peer already uses
 * that path and we follow it.
 */
static void
chat_p2p_follow(struct chat_tunnel *tun)
{
	PRECOND(tun != NULL);
	PRECOND(tun->ctx != NULL);

	if (vigil_inet_match(&tun->from, &tun->addr)) {
		if (tun->p2p_verify != 0)
			tun->p2p_backoff = 0;

		tun->p2p_rx = vigil->now;
		tun->p2p_retry = 0;
		tun->p2p_verify = 0;
		return;
	}

	if (!(vigil->gospel->flags & VIGIL_CONFIG_FLAG_P2P))
		return;

	if (!vigil_inet_match(&tun->addr, &tun->cathedral.addr))
		return;

	if (!vigil_inet_match(&tun->from, &tun->p2p_peer))
		return;

	vigil_log(LOG_INFO, "[chat] %02x p2p via %s, following our peer",
	    tun->peer->id, vigil_address(&tun->from));

	memcpy(&tun->addr, &tun->from, sizeof(tun->from));

	tun->p2p_rx = vigil->now;
	tun->p2p_retry = 0;
	tun->p2p_verify = vigil->now + CHAT_P2P_VERIFY;
	tun->holepunch = vigil->now + CHAT_HOLEPUNCH;
	tun->next_heartbeat = vigil->now;
	(void)kyrka_p2p_active(tun->ctx, 1);
}

/*
 * Our peer reaches us via the cathedral. If we did not hear from it over
 * p2p for a while it cannot use our p2p path, so we go back to the cathedral.
 */
static void
chat_p2p_abandon(struct chat_tunnel *tun)
{
	PRECOND(tun != NULL);
	PRECOND(tun->ctx != NULL);

	if (vigil_inet_match(&tun->addr, &tun->cathedral.addr))
		return;

	/* Our peer may not know about this path yet. */
	if (tun->p2p_verify != 0)
		return;

	if ((vigil->now - tun->p2p_rx) < CHAT_P2P_SILENT)
		return;

	vigil_log(LOG_INFO, "[chat] %02x p2p via %s went silent",
	    tun->peer->id, vigil_address(&tun->addr));

	chat_p2p_fail(tun);
}

/*
 * The p2p path we are on does not work, go back to the cathedral and
 * do not try it again for a while.
 */
static void
chat_p2p_fail(struct chat_tunnel *tun)
{
	PRECOND(tun != NULL);
	PRECOND(tun->ctx != NULL);

	if (tun->p2p_backoff == 0)
		tun->p2p_backoff = CHAT_P2P_RETRY;
	else
		tun->p2p_backoff *= 2;

	if (tun->p2p_backoff > CHAT_P2P_RETRY_MAX)
		tun->p2p_backoff = CHAT_P2P_RETRY_MAX;

	tun->p2p_verify = 0;
	tun->p2p_retry = vigil->now + tun->p2p_backoff;

	memcpy(&tun->p2p_failed, &tun->addr, sizeof(tun->addr));
	memcpy(&tun->addr, &tun->cathedral.addr, sizeof(tun->cathedral.addr));

	(void)kyrka_p2p_active(tun->ctx, 0);
}

/*
 * Pick up the avatar the platform gave us if it changed.
 */
static void
chat_avatar_update(void)
{
	struct vigil_chat_data_head	head;
	struct chat_blob		*blob;

	vigil_lock(&lock);

	if (avatar_serial == portrait_serial) {
		vigil_unlock(&lock);
		return;
	}

	blob = NULL;
	portrait_hash = 0;
	portrait_serial = avatar_serial;

	if (avatar_len > 0) {
		portrait_hash = chat_avatar_hash(avatar, avatar_len);

		memset(&head, 0, sizeof(head));
		head.kind = VIGIL_CHAT_DATA_AVATAR;
		head.len = strlen(CHAT_AVATAR_NAME);
		memcpy(head.name, CHAT_AVATAR_NAME, head.len);

		blob = calloc(1, sizeof(*blob));
		if (blob == NULL)
			fatal("calloc: failed to allocate our avatar");

		blob->refs = 1;
		blob->size = sizeof(head) + avatar_len;
		blob->count = (blob->size + VIGIL_CHAT_CHUNK_DATA - 1) /
		    VIGIL_CHAT_CHUNK_DATA;

		blob->data = malloc(blob->size);
		if (blob->data == NULL)
			fatal("malloc: failed to allocate our avatar data");

		memcpy(blob->data, &head, sizeof(head));
		memcpy(blob->data + sizeof(head), avatar, avatar_len);
	}

	vigil_unlock(&lock);

	if (portrait != NULL)
		chat_blob_release(portrait);

	portrait = blob;
}

/*
 * Send our avatar to the peer of the given tunnel once we heard from it,
 * and again when ours changes. Peers on gospel cannot take it.
 */
static void
chat_avatar_push(struct chat_tunnel *tun)
{
	struct chat_msg		*msg;

	PRECOND(tun != NULL);

	if (!tun->heard)
		return;

	if (!(tun->features & VIGIL_CHAT_FEATURE_DATA))
		return;

	if (tun->avatar_serial == portrait_serial)
		return;

	tun->avatar_serial = portrait_serial;
	chat_avatar_drop(tun);

	if (portrait == NULL)
		return;

	if (tun->avatar_have == portrait_hash)
		return;

	msg = chat_data_msg(0, portrait);
	msg->sent = 1;

	chat_msg_queue(tun, msg);
	tun->avatar = msg;
}

/*
 * Drop the avatar we are sending on the given tunnel, if any.
 */
static void
chat_avatar_drop(struct chat_tunnel *tun)
{
	PRECOND(tun != NULL);

	if (tun->avatar == NULL)
		return;

	if (tun->xfer != NULL && tun->xfer->msg == tun->avatar)
		chat_xfer_free(tun);

	chat_msg_free(tun->avatar);
	tun->avatar = NULL;
}

/*
 * The hash of an avatar in a heartbeat, 0 means none.
 */
static u_int64_t
chat_avatar_hash(const u_int8_t *data, size_t len)
{
	u_int64_t	hash;

	PRECOND(data != NULL);

	hash = chat_frame_hash(data, len);

	return (hash == 0 ? 1 : hash);
}

/*
 * Tell the platform once that the given peer has no avatar, so it can
 * drop the one it may still hold for it.
 */
static void
chat_avatar_check(struct chat_peer *peer, u_int8_t features)
{
	PRECOND(peer != NULL);

	if (features & VIGIL_CHAT_FEATURE_AVATAR) {
		peer->faceless = 0;
		return;
	}

	peer->avatar_held = 0;

	if (peer->faceless)
		return;

	if (chat_event_queue(CHAT_EVENT_AVATAR, peer, 0, NULL, 0) == -1)
		return;

	peer->faceless = 1;
}

/*
 * Send a frame from the sendq to our peer, this can also be a heartbeat
 * with our nickname and typing state.
 */
static void
chat_frame_send(struct chat_tunnel *tun, struct chat_msg *msg)
{
	size_t			max;
	struct kyrka_packet	pkt;
	struct vigil_chat_hb	*hb;
	u_int8_t		*ptr;
	struct vigil_chat_frame	*frame;

	PRECOND(tun != NULL);
	PRECOND(tun->ctx != NULL);
	PRECOND(msg == NULL || msg == TAILQ_FIRST(&tun->sendq));

	ptr = kyrka_packet_databuf(tun->ctx, &pkt, &max);
	if (ptr == NULL)
		fatal("failed to get chat data buffer");

	if (sizeof(*frame) > max)
		fatal("chat frame does not fit");

	frame = (struct vigil_chat_frame *)ptr;

	if (msg != NULL) {
		TAILQ_REMOVE(&tun->sendq, msg, slist);

		msg->pending = 0;
		memcpy(frame, &msg->frame, sizeof(*frame));

		if (msg->group.id != 0) {
			if (tun->features & VIGIL_CHAT_FEATURE_GROUP) {
				chat_group_frame(frame, msg);
			} else {
				frame->len = 0;
				memset(frame->data, 0, sizeof(frame->data));
			}
		} else if (msg->topic.id != 0) {
			chat_topic_frame(tun, frame, msg);
		} else if (msg->quote.hash != 0 &&
		    (tun->features & VIGIL_CHAT_FEATURE_REPLY)) {
			chat_reply_frame(frame, msg);
		}

		if (msg->frame.type == VIGIL_CHAT_TYPE_TEXT) {
			msg->age = vigil->now;

			if (msg->sent == 0 &&
			    chat_event_queue(CHAT_EVENT_SENT, tun->peer,
			    msg->ref, NULL, 0) != -1)
				msg->sent = 1;
		} else {
			free(msg);
		}
	} else {
		memset(frame, 0, sizeof(*frame));

		frame->id = VIGIL_CHAT_HEARTBEAT_ID;
		frame->type = VIGIL_CHAT_TYPE_HEARTBEAT;

		VERIFY(sizeof(*hb) <= sizeof(frame->data));

		hb = (struct vigil_chat_hb *)frame->data;
		hb->uid = tun->local_uid;
		hb->features = VIGIL_CHAT_FEATURE_DATA |
		    VIGIL_CHAT_FEATURE_REPLY | VIGIL_CHAT_FEATURE_TOPIC |
		    VIGIL_CHAT_FEATURE_GROUP;

		if (named) {
			hb->features |= VIGIL_CHAT_FEATURE_NAME;
			memcpy(hb->instance, vigil->gospel->instance,
			    sizeof(hb->instance));
			memcpy(hb->signature,
			    vigil->gospel->name_signature,
			    sizeof(hb->signature));
		}

		if (portrait != NULL)
			hb->features |= VIGIL_CHAT_FEATURE_AVATAR;

		hb->avatar_have = htobe64(tun->peer->avatar_held);
		hb->typing = vigil_atomic_read(&typing[tun->peer->id]);

		chat_typing_state(&tun->typed_owner, &tun->typed_group,
		    &tun->typed_topic);
		hb->typing_owner = tun->typed_owner;
		hb->typing_group = htobe64(tun->typed_group);
		hb->typing_topic = htobe64(tun->typed_topic);

		vigil_lock(&lock);
		memcpy(hb->name, nick, sizeof(hb->name));
		vigil_unlock(&lock);

		tun->greeted = 1;
		tun->typed = hb->typing;

		if (vigil->now < tun->holepunch) {
			tun->next_heartbeat = vigil->now +
			    CHAT_HOLEPUNCH_INTERVAL;
		} else {
			tun->next_heartbeat = vigil->now + CHAT_HEARTBEAT;
		}
	}

	pkt.length = sizeof(*frame);

	if (vigil_inet_match(&tun->addr, &tun->cathedral.addr))
		pkt.shroud = KYRKA_PACKET_SHROUD_CATHEDRAL;
	else
		pkt.shroud = KYRKA_PACKET_SHROUD_PEER;

	if (kyrka_heaven_input(tun->ctx, &pkt) == -1 &&
	    kyrka_last_error(tun->ctx) != KYRKA_ERROR_NO_TX_KEY) {
		vigil_log(LOG_NOTICE, "[chat] kyrka_heaven_input: %d",
		    kyrka_last_error(tun->ctx));
	}
}

/*
 * Queue an ack for the message with the given id.
 */
static void
chat_ack_queue(struct chat_tunnel *tun, u_int64_t id)
{
	struct chat_msg		*msg;

	PRECOND(tun != NULL);
	PRECOND(id != 0);

	msg = calloc(1, sizeof(*msg));
	if (msg == NULL)
		fatal("calloc: failed to allocate chat ack");

	msg->pending = 1;
	msg->frame.id = htobe64(id);
	msg->frame.type = VIGIL_CHAT_TYPE_ACK;

	TAILQ_INSERT_TAIL(&tun->sendq, msg, slist);
}

/*
 * Our peer acked the message with the given id, tell the platform it was
 * delivered and drop it.
 */
static void
chat_ack_recv(struct chat_tunnel *tun, u_int64_t id)
{
	struct chat_msg		*msg;

	PRECOND(tun != NULL);
	PRECOND(id != 0);

	if (tun->avatar != NULL && tun->avatar->id == id) {
		vigil_log(LOG_INFO,
		    "[chat] %02x has our avatar", tun->peer->id);
		chat_avatar_drop(tun);
		return;
	}

	TAILQ_FOREACH(msg, &tun->peer->outbox, wlist) {
		if (msg->id == id)
			break;
	}

	if (msg == NULL)
		return;

	if (chat_event_queue(CHAT_EVENT_DELIVERED,
	    tun->peer, msg->ref, NULL, 0) == -1)
		return;

	tun->peer->active = vigil->now;

	if (msg->pending)
		TAILQ_REMOVE(&tun->sendq, msg, slist);

	if (tun->xfer != NULL && tun->xfer->msg == msg)
		chat_xfer_free(tun);

	TAILQ_REMOVE(&tun->peer->outbox, msg, wlist);
	chat_msg_free(msg);
}

/*
 * We got a heartbeat from our peer, take its nickname and typing state. A
 * new uid means our peer restarted and its ids start over.
 */
static void
chat_hb_recv(struct chat_tunnel *tun, struct vigil_chat_frame *frame)
{
	struct vigil_chat_hb	*hb;
	struct chat_peer	*peer;
	char			name[VIGIL_CHAT_NICK_MAX + 1];

	PRECOND(tun != NULL);
	PRECOND(frame != NULL);
	PRECOND(frame->type == VIGIL_CHAT_TYPE_HEARTBEAT);

	peer = tun->peer;
	hb = (struct vigil_chat_hb *)frame->data;

	memset(name, 0, sizeof(name));
	memcpy(name, hb->name, sizeof(hb->name));

	if (chat_nick_valid(name) == -1)
		name[0] = '\0';

	if (strcmp(name, peer->nick)) {
		vigil_log(LOG_INFO, "[chat] %02x is now known as '%s'",
		    peer->id, name);
		memcpy(peer->nick, name, sizeof(name));
		peer->reported = 0;
	}

	chat_hb_name(peer, hb);
	chat_name_report(peer);
	chat_avatar_check(peer, hb->features);

	tun->heard = 1;
	tun->features = hb->features;
	tun->typing = hb->typing ? 1 : 0;
	tun->typing_owner = hb->typing_owner;
	tun->typing_group = be64toh(hb->typing_group);
	tun->typing_topic = be64toh(hb->typing_topic);

	if (tun->typing_group == 0)
		tun->typing_owner = 0;

	if (tun->avatar_have != be64toh(hb->avatar_have)) {
		tun->avatar_have = be64toh(hb->avatar_have);
		tun->avatar_serial = 0;
	}

	if (tun->remote_uid == hb->uid)
		return;

	if (tun->remote_uid != 0) {
		tun->seen_next = 0;
		tun->avatar_serial = 0;
		memset(tun->seen, 0, sizeof(tun->seen));
		chat_rxfer_free(tun);
	}

	tun->remote_uid = hb->uid;
}

/*
 * Take the signed name from a heartbeat. We only accept it if our flock
 * signed it for the KEK id of the tunnel it came in on, otherwise the
 * peer has no signed name.
 */
static void
chat_hb_name(struct chat_peer *peer, const struct vigil_chat_hb *hb)
{
	char		name[VIGIL_INSTANCE_MAX + 1];

	PRECOND(peer != NULL);
	PRECOND(hb != NULL);

	memset(name, 0, sizeof(name));

	if (hb->features & VIGIL_CHAT_FEATURE_NAME)
		memcpy(name, hb->instance, sizeof(hb->instance));

	if (!strcmp(name, peer->seen) &&
	    !memcmp(hb->signature, peer->signature, sizeof(hb->signature)))
		return;

	memcpy(peer->seen, name, sizeof(peer->seen));
	memcpy(peer->signature, hb->signature, sizeof(peer->signature));
	memset(peer->name, 0, sizeof(peer->name));

	peer->reported = 0;

	if (name[0] == '\0')
		return;

	if (vigil_config_name_verify(vigil->gospel, peer->id, name,
	    hb->signature, sizeof(hb->signature)) == -1) {
		vigil_log(LOG_NOTICE,
		    "[chat] %02x sent a name that does not verify", peer->id);
		return;
	}

	vigil_log(LOG_INFO, "[chat] %02x is signed as '%s'", peer->id, name);
	memcpy(peer->name, name, strlen(name) + 1);
}

/*
 * Tell the platform the nickname and signed name of the given peer once
 * we heard from it, and again when either of them changes.
 */
static void
chat_name_report(struct chat_peer *peer)
{
	PRECOND(peer != NULL);

	if (peer->reported)
		return;

	if (chat_event_queue(CHAT_EVENT_NAME, peer, 0,
	    (const u_int8_t *)peer->name, strlen(peer->name)) == -1)
		return;

	peer->reported = 1;
}

/*
 * We got a message from our peer and hand it to the platform.
 *
 * If we had it already we ack it again once it was stored, if the platform
 * did not store it in time we hand it over again under a new token.
 */
static void
chat_text_recv(struct chat_tunnel *tun, u_int64_t id,
    const struct chat_quote *quote, const struct chat_topic *topic,
    const struct chat_group *group, u_int8_t *data, u_int16_t len)
{
	int			ret;
	u_int32_t		idx;
	u_int64_t		hash;
	struct chat_seen	*seen;

	PRECOND(tun != NULL);
	PRECOND(id != 0);
	PRECOND(data != NULL);
	PRECOND(len <= VIGIL_CHAT_MESSAGE_MAX);

	if (len == 0 && chat_text_empty(topic, group)) {
		chat_ack_queue(tun, id);
		return;
	}

	hash = chat_frame_hash(data, len);
	for (idx = 0; idx < CHAT_SEEN_MAX; idx++) {
		seen = &tun->seen[idx];
		if (seen->id == id && seen->hash == hash)
			break;
	}

	if (idx < CHAT_SEEN_MAX) {
		if (seen->token == 0) {
			chat_ack_queue(tun, id);
			return;
		}

		/* The platform never stored it, hand it over again. */
		if ((vigil->now - seen->age) < CHAT_STORE_WAIT)
			return;
	} else {
		seen = NULL;
	}

	tokens++;
	chat_utf8_normalize(data, len);

	ret = chat_text_report(tun, tokens, quote, topic, group, data, len);
	if (ret == -1)
		return;

	if (seen == NULL)
		seen = &tun->seen[tun->seen_next++ % CHAT_SEEN_MAX];

	seen->id = id;
	seen->size = 0;
	seen->hash = hash;
	seen->avatar = 0;
	seen->token = tokens;
	seen->age = vigil->now;

	tun->peer->active = vigil->now;
}

/*
 * Returns 1 if an empty text with the given topic and group is one we drop.
 * A topic without a name was removed and a roster or leave may carry no
 * text.
 */
static int
chat_text_empty(const struct chat_topic *topic,
    const struct chat_group *group)
{
	if (group != NULL && group->type != VIGIL_CHAT_TYPE_TEXT)
		return (0);

	if (topic != NULL && topic->set)
		return (0);

	return (1);
}

/*
 * We got a reply from our peer, in the given group if any, take who wrote
 * the message it replies to and hand it over like any other message.
 */
static void
chat_reply_recv(struct chat_tunnel *tun, u_int64_t id,
    const struct chat_group *group, u_int8_t *data, u_int16_t len)
{
	size_t			head;
	struct chat_quote	quote;
	struct vigil_chat_reply	*reply;

	PRECOND(tun != NULL);
	PRECOND(id != 0);
	PRECOND(data != NULL);
	PRECOND(len <= VIGIL_CHAT_MESSAGE_MAX);

	reply = (struct vigil_chat_reply *)data;
	head = sizeof(*reply) - sizeof(reply->text);

	if (len < head)
		return;

	quote.author = reply->author;
	quote.hash = be64toh(reply->hash);

	if (quote.author == (vigil->gospel->spi & 0xff))
		quote.author = 0;

	if (quote.hash == 0) {
		chat_text_recv(tun, id, NULL, NULL, group,
		    reply->text, len - head);
		return;
	}

	chat_text_recv(tun, id, &quote, NULL, group, reply->text, len - head);
}

/*
 * Turn the given frame into a reply, for peers that take them.
 */
static void
chat_reply_frame(struct vigil_chat_frame *frame, const struct chat_msg *msg)
{
	u_int16_t		len;
	struct vigil_chat_reply	*reply;

	PRECOND(frame != NULL);
	PRECOND(msg != NULL);
	PRECOND(msg->quote.hash != 0);

	len = be16toh(msg->frame.len);
	reply = (struct vigil_chat_reply *)frame->data;

	PRECOND(len <= sizeof(reply->text));

	reply->author = msg->quote.author;
	reply->hash = htobe64(msg->quote.hash);
	memcpy(reply->text, msg->frame.data, len);

	frame->type = VIGIL_CHAT_TYPE_REPLY;
	frame->len = htobe16(len + sizeof(*reply) - sizeof(reply->text));
}

/*
 * Our peer set up or removed a topic, in the given group if any, hand it
 * over like a message. One we cannot use is acked right away so our peer
 * stops sending it.
 */
static void
chat_topic_recv(struct chat_tunnel *tun, u_int64_t id,
    const struct chat_group *group, u_int8_t *data, u_int16_t len)
{
	size_t			head;
	struct vigil_chat_topic	*wire;
	struct chat_topic	topic;

	PRECOND(tun != NULL);
	PRECOND(id != 0);
	PRECOND(data != NULL);
	PRECOND(len <= VIGIL_CHAT_MESSAGE_MAX);

	wire = (struct vigil_chat_topic *)data;
	head = sizeof(*wire) - sizeof(wire->name);

	if (len < head)
		return;

	if (len - head > sizeof(wire->name)) {
		chat_ack_queue(tun, id);
		return;
	}

	memset(&topic, 0, sizeof(topic));

	topic.set = 1;
	topic.id = be64toh(wire->id);
	topic.color = be32toh(wire->color) & 0xffffff;

	if (topic.id == 0) {
		chat_ack_queue(tun, id);
		return;
	}

	chat_text_recv(tun, id, NULL, &topic, group, wire->name, len - head);
}

/*
 * We got a message in a topic from our peer, in the given group if any,
 * hand it over like any other message. One we cannot use is acked right
 * away so our peer stops sending it.
 */
static void
chat_topic_text_recv(struct chat_tunnel *tun, u_int64_t id,
    const struct chat_group *group, u_int8_t *data, u_int16_t len)
{
	size_t				head;
	struct vigil_chat_topic_text	*wire;
	struct chat_quote		quote;
	struct chat_topic		topic;

	PRECOND(tun != NULL);
	PRECOND(id != 0);
	PRECOND(data != NULL);
	PRECOND(len <= VIGIL_CHAT_MESSAGE_MAX);

	wire = (struct vigil_chat_topic_text *)data;
	head = sizeof(*wire) - sizeof(wire->text);

	if (len < head)
		return;

	memset(&quote, 0, sizeof(quote));
	memset(&topic, 0, sizeof(topic));

	quote.author = wire->author;
	quote.hash = be64toh(wire->hash);
	topic.id = be64toh(wire->topic);

	if (topic.id == 0) {
		chat_ack_queue(tun, id);
		return;
	}

	if (quote.author == (vigil->gospel->spi & 0xff))
		quote.author = 0;

	if (quote.hash == 0) {
		chat_text_recv(tun, id, NULL, &topic, group,
		    wire->text, len - head);
		return;
	}

	chat_text_recv(tun, id, &quote, &topic, group, wire->text, len - head);
}

/*
 * Turn the given frame into one for the topic of its message. A peer that
 * does not take topics gets the message as it is, and an empty text for a
 * topic it cannot know about.
 */
static void
chat_topic_frame(struct chat_tunnel *tun, struct vigil_chat_frame *frame,
    const struct chat_msg *msg)
{
	u_int16_t			len;
	struct vigil_chat_topic_text	*text;
	struct vigil_chat_topic		*topic;

	PRECOND(tun != NULL);
	PRECOND(frame != NULL);
	PRECOND(msg != NULL);
	PRECOND(msg->topic.id != 0);

	len = be16toh(msg->frame.len);

	if (!(tun->features & VIGIL_CHAT_FEATURE_TOPIC)) {
		if (msg->topic.set) {
			frame->len = 0;
			memset(frame->data, 0, sizeof(frame->data));
		} else if (msg->quote.hash != 0 &&
		    (tun->features & VIGIL_CHAT_FEATURE_REPLY)) {
			chat_reply_frame(frame, msg);
		}
		return;
	}

	if (msg->topic.set) {
		topic = (struct vigil_chat_topic *)frame->data;

		PRECOND(len <= sizeof(topic->name));

		topic->id = htobe64(msg->topic.id);
		topic->color = htobe32(msg->topic.color);
		memcpy(topic->name, msg->frame.data, len);

		frame->type = VIGIL_CHAT_TYPE_TOPIC;
		frame->len = htobe16(len + sizeof(*topic) - sizeof(topic->name));
		return;
	}

	text = (struct vigil_chat_topic_text *)frame->data;

	PRECOND(len <= sizeof(text->text));

	text->topic = htobe64(msg->topic.id);
	text->author = msg->quote.author;
	text->hash = htobe64(msg->quote.hash);
	memcpy(text->text, msg->frame.data, len);

	frame->type = VIGIL_CHAT_TYPE_TOPIC_TEXT;
	frame->len = htobe16(len + sizeof(*text) - sizeof(text->text));
}

/*
 * We got a frame in a group from our peer, hand what it carries over like
 * any other message. One we cannot use is acked right away so our peer
 * stops sending it.
 */
static void
chat_group_recv(struct chat_tunnel *tun, u_int64_t id, u_int8_t *data,
    u_int16_t len)
{
	size_t			head;
	struct chat_group	group;
	struct vigil_chat_group	*wire;

	PRECOND(tun != NULL);
	PRECOND(id != 0);
	PRECOND(data != NULL);
	PRECOND(len <= VIGIL_CHAT_MESSAGE_MAX);

	wire = (struct vigil_chat_group *)data;
	head = sizeof(*wire) - sizeof(wire->data);

	if (len < head)
		return;

	len -= head;
	memset(&group, 0, sizeof(group));

	group.type = wire->type;
	group.owner = wire->owner;
	group.id = be64toh(wire->id);
	group.serial = be32toh(wire->serial);

	if (group.id == 0) {
		chat_ack_queue(tun, id);
		return;
	}

	if (group.owner == 0) {
		chat_ack_queue(tun, id);
		return;
	}

	if (group.owner >= KYRKA_PEERS_PER_FLOCK) {
		chat_ack_queue(tun, id);
		return;
	}

	switch (group.type) {
	case VIGIL_CHAT_TYPE_TEXT:
		chat_text_recv(tun, id, NULL, NULL, &group, wire->data, len);
		break;
	case VIGIL_CHAT_TYPE_REPLY:
		group.type = VIGIL_CHAT_TYPE_TEXT;
		chat_reply_recv(tun, id, &group, wire->data, len);
		break;
	case VIGIL_CHAT_TYPE_TOPIC:
		if (group.owner != tun->peer->id) {
			vigil_log(LOG_NOTICE,
			    "[chat] %02x sent a topic for %02x",
			    tun->peer->id, group.owner);
			chat_ack_queue(tun, id);
			return;
		}

		group.type = VIGIL_CHAT_TYPE_TEXT;
		chat_topic_recv(tun, id, &group, wire->data, len);
		break;
	case VIGIL_CHAT_TYPE_TOPIC_TEXT:
		group.type = VIGIL_CHAT_TYPE_TEXT;
		chat_topic_text_recv(tun, id, &group, wire->data, len);
		break;
	case VIGIL_CHAT_TYPE_ROSTER:
		chat_group_roster(tun, id, &group, wire->data, len);
		break;
	case VIGIL_CHAT_TYPE_LEAVE:
		if (group.owner != (vigil->gospel->spi & 0xff)) {
			chat_ack_queue(tun, id);
			return;
		}

		chat_text_recv(tun, id, NULL, NULL, &group, wire->data, 0);
		break;
	default:
		chat_ack_queue(tun, id);
		break;
	}
}

/*
 * We got the roster of a group, only the owner of a group sends it.
 */
static void
chat_group_roster(struct chat_tunnel *tun, u_int64_t id,
    struct chat_group *group, u_int8_t *data, u_int16_t len)
{
	size_t				head;
	struct vigil_chat_roster	*roster;

	PRECOND(tun != NULL);
	PRECOND(id != 0);
	PRECOND(group != NULL);
	PRECOND(data != NULL);

	roster = (struct vigil_chat_roster *)data;
	head = sizeof(*roster) - sizeof(roster->name);

	if (len < head) {
		chat_ack_queue(tun, id);
		return;
	}

	if (len - head > sizeof(roster->name)) {
		chat_ack_queue(tun, id);
		return;
	}

	if (group->owner != tun->peer->id) {
		vigil_log(LOG_NOTICE, "[chat] %02x sent a roster for %02x",
		    tun->peer->id, group->owner);
		chat_ack_queue(tun, id);
		return;
	}

	if (roster->state != VIGIL_CHAT_GROUP_OPEN &&
	    roster->state != VIGIL_CHAT_GROUP_ENDED) {
		chat_ack_queue(tun, id);
		return;
	}

	group->state = roster->state;
	memcpy(group->members, roster->members, sizeof(group->members));

	chat_text_recv(tun, id, NULL, NULL, group, roster->name, len - head);
}

/*
 * Turn the given frame into one for the group of its message.
 */
static void
chat_group_frame(struct vigil_chat_frame *frame, const struct chat_msg *msg)
{
	size_t				off;
	u_int16_t			len;
	struct vigil_chat_group		*wire;
	struct vigil_chat_topic		*topic;
	struct vigil_chat_reply		*reply;
	struct vigil_chat_roster	*roster;
	struct vigil_chat_topic_text	*text;

	PRECOND(frame != NULL);
	PRECOND(msg != NULL);
	PRECOND(msg->group.id != 0);

	len = be16toh(msg->frame.len);
	wire = (struct vigil_chat_group *)frame->data;

	wire->type = msg->group.type;
	wire->owner = msg->group.owner;
	wire->id = htobe64(msg->group.id);
	wire->serial = htobe32(msg->group.serial);

	switch (msg->group.type) {
	case VIGIL_CHAT_TYPE_ROSTER:
		roster = (struct vigil_chat_roster *)wire->data;

		PRECOND(len <= sizeof(roster->name));

		roster->state = msg->group.state;
		memcpy(roster->members, msg->group.members,
		    sizeof(roster->members));
		memcpy(roster->name, msg->frame.data, len);

		off = len + sizeof(*roster) - sizeof(roster->name);
		break;
	case VIGIL_CHAT_TYPE_LEAVE:
		off = 0;
		break;
	default:
		if (msg->topic.set) {
			topic = (struct vigil_chat_topic *)wire->data;

			PRECOND(len <= sizeof(topic->name));

			topic->id = htobe64(msg->topic.id);
			topic->color = htobe32(msg->topic.color);
			memcpy(topic->name, msg->frame.data, len);

			wire->type = VIGIL_CHAT_TYPE_TOPIC;
			off = len + sizeof(*topic) - sizeof(topic->name);
			break;
		}

		if (msg->topic.id != 0) {
			text = (struct vigil_chat_topic_text *)wire->data;

			PRECOND(len <= VIGIL_CHAT_GROUP_TOPIC_MAX);

			text->topic = htobe64(msg->topic.id);
			text->author = msg->quote.author;
			text->hash = htobe64(msg->quote.hash);
			memcpy(text->text, msg->frame.data, len);

			wire->type = VIGIL_CHAT_TYPE_TOPIC_TEXT;
			off = len + sizeof(*text) - sizeof(text->text);
			break;
		}

		if (msg->quote.hash == 0) {
			PRECOND(len <= sizeof(wire->data));

			memcpy(wire->data, msg->frame.data, len);
			off = len;
			break;
		}

		reply = (struct vigil_chat_reply *)wire->data;

		PRECOND(len <= VIGIL_CHAT_GROUP_REPLY_MAX);

		reply->author = msg->quote.author;
		reply->hash = htobe64(msg->quote.hash);
		memcpy(reply->text, msg->frame.data, len);

		wire->type = VIGIL_CHAT_TYPE_REPLY;
		off = len + sizeof(*reply) - sizeof(reply->text);
		break;
	}

	frame->type = VIGIL_CHAT_TYPE_GROUP;
	frame->len = htobe16(off + sizeof(*wire) - sizeof(wire->data));
}

/*
 * Queue an event for a message we got, a message that sets a topic is a
 * topic event. Returns -1 if there are too many.
 */
static int
chat_text_report(struct chat_tunnel *tun, u_int64_t token,
    const struct chat_quote *quote, const struct chat_topic *topic,
    const struct chat_group *group, const u_int8_t *data, size_t len)
{
	struct chat_event	*evt;

	PRECOND(tun != NULL);
	PRECOND(data != NULL);
	PRECOND(len <= VIGIL_CHAT_MESSAGE_MAX);

	evt = calloc(1, sizeof(*evt));
	if (evt == NULL)
		fatal("calloc: failed to allocate chat event");

	evt->value = token;
	evt->peer = tun->peer->id;
	evt->type = CHAT_EVENT_MESSAGE;

	if (quote != NULL)
		memcpy(&evt->quote, quote, sizeof(*quote));

	if (topic != NULL)
		memcpy(&evt->topic, topic, sizeof(*topic));

	if (group != NULL)
		memcpy(&evt->group, group, sizeof(*group));

	if (evt->topic.set)
		evt->type = CHAT_EVENT_TOPIC;

	if (evt->group.type == VIGIL_CHAT_TYPE_ROSTER)
		evt->type = CHAT_EVENT_ROSTER;

	if (evt->group.type == VIGIL_CHAT_TYPE_LEAVE)
		evt->type = CHAT_EVENT_LEAVE;

	memcpy(evt->text, data, len);
	memcpy(evt->name, tun->peer->name, sizeof(evt->name));
	memcpy(evt->nick, tun->peer->nick, sizeof(evt->nick));

	if (evt->type == CHAT_EVENT_MESSAGE)
		chat_mention_check(evt);

	return (chat_event_insert(evt));
}

/*
 * Returns 1 if we want the given peer in the signaling liturgy, this is the
 * case if we hold messages for it, its chat is open or we talked recently.
 *
 * We also signal a peer that signals us until our tunnel with it is up,
 * the cathedral does not tell it we are around otherwise.
 */
static int
chat_signal_check(struct chat_peer *peer)
{
	PRECOND(peer != NULL);

	if (peer->id == 0)
		return (0);

	if (!TAILQ_EMPTY(&peer->outbox))
		return (1);

	if (vigil_atomic_read(&opened[peer->id]))
		return (1);

	if (peer->active != 0 && (vigil->now - peer->active) < CHAT_IDLE)
		return (1);

	if (peer->present && !chat_tunnel_online(peer->tunnel))
		return (1);

	return (0);
}

/*
 * Queue the messages for the given tunnel that were not acked in time.
 */
static void
chat_msg_resend(struct chat_tunnel *tun)
{
	struct chat_msg		*msg;

	PRECOND(tun != NULL);

	TAILQ_FOREACH(msg, &tun->peer->outbox, wlist) {
		if (msg->pending)
			continue;

		if (msg->id == 0)
			continue;

		if (msg->blob != NULL)
			continue;

		if ((vigil->now - msg->age) < CHAT_RESEND)
			continue;

		if (chat_msg_waits(tun, msg))
			continue;

		msg->pending = 1;
		TAILQ_INSERT_TAIL(&tun->sendq, msg, slist);
	}
}

/*
 * Give the given message an id on the given tunnel and queue it.
 */
static void
chat_msg_queue(struct chat_tunnel *tun, struct chat_msg *msg)
{
	PRECOND(tun != NULL);
	PRECOND(msg != NULL);
	PRECOND(msg->pending == 0);

	msg->id = tun->msgid++;
	msg->frame.id = htobe64(msg->id);

	if (msg->blob != NULL) {
		msg->age = vigil->now;
		return;
	}

	if (chat_msg_waits(tun, msg)) {
		msg->age = 0;
		return;
	}

	msg->pending = 1;
	TAILQ_INSERT_TAIL(&tun->sendq, msg, slist);
}

/*
 * Returns 1 if the given message waits until we know what our peer takes,
 * replies and anything in a topic or group do.
 */
static int
chat_msg_waits(struct chat_tunnel *tun, struct chat_msg *msg)
{
	PRECOND(tun != NULL);
	PRECOND(msg != NULL);

	if (tun->heard)
		return (0);

	if (msg->quote.hash != 0)
		return (1);

	if (msg->topic.id != 0)
		return (1);

	if (msg->group.id != 0)
		return (1);

	return (0);
}

/*
 * Add a message to the outbox of the given peer unless we have it.
 */
static void
chat_outbox_add(struct chat_peer *peer, u_int64_t ref,
    const struct chat_quote *quote, const struct chat_topic *topic,
    const struct chat_group *group, const u_int8_t *data, size_t len)
{
	struct chat_msg		*msg;

	PRECOND(peer != NULL);
	PRECOND(quote != NULL);
	PRECOND(topic != NULL);
	PRECOND(group != NULL);
	PRECOND(data != NULL);
	PRECOND(len > 0 || topic->set ||
	    group->type == VIGIL_CHAT_TYPE_ROSTER ||
	    group->type == VIGIL_CHAT_TYPE_LEAVE);
	PRECOND(len <= VIGIL_CHAT_MESSAGE_MAX);

	TAILQ_FOREACH(msg, &peer->outbox, wlist) {
		if (msg->ref == ref)
			return;
	}

	msg = calloc(1, sizeof(*msg));
	if (msg == NULL)
		fatal("calloc: failed to allocate chat message");

	msg->ref = ref;
	msg->frame.len = htobe16(len);
	msg->frame.type = VIGIL_CHAT_TYPE_TEXT;

	memcpy(msg->frame.data, data, len);
	memcpy(&msg->quote, quote, sizeof(*quote));
	memcpy(&msg->topic, topic, sizeof(*topic));
	memcpy(&msg->group, group, sizeof(*group));

	TAILQ_INSERT_TAIL(&peer->outbox, msg, wlist);

	if (peer->tunnel != NULL)
		chat_msg_queue(peer->tunnel, msg);
}

/*
 * Drop all messages on the outbox of the given peer.
 */
static void
chat_outbox_free(struct chat_peer *peer)
{
	struct chat_msg		*msg;

	PRECOND(peer != NULL);

	if (peer->tunnel != NULL)
		chat_xfer_free(peer->tunnel);

	while ((msg = TAILQ_FIRST(&peer->outbox)) != NULL) {
		if (msg->pending)
			TAILQ_REMOVE(&peer->tunnel->sendq, msg, slist);

		TAILQ_REMOVE(&peer->outbox, msg, wlist);
		chat_msg_free(msg);
	}
}

/*
 * The platform stored the message with the given token, ack it if
 * the tunnel it came in on is still there.
 */
static void
chat_token_ack(u_int64_t token)
{
	int		idx;

	if (token == 0)
		return;

	for (idx = 0; idx < KYRKA_PEERS_PER_FLOCK; idx++) {
		if (chat_tunnel_ack(peers[idx].tunnel, token) != -1)
			return;
	}
}

/*
 * Ack the message with the given token if it came in on the given
 * tunnel, returns -1 if it did not.
 */
static int
chat_tunnel_ack(struct chat_tunnel *tun, u_int64_t token)
{
	u_int32_t	slot;

	if (tun == NULL)
		return (-1);

	for (slot = 0; slot < CHAT_SEEN_MAX; slot++) {
		if (tun->seen[slot].token != token)
			continue;

		tun->seen[slot].token = 0;
		chat_ack_queue(tun, tun->seen[slot].id);

		if (tun->seen[slot].avatar != 0)
			tun->peer->avatar_held = tun->seen[slot].avatar;

		return (0);
	}

	return (-1);
}

/*
 * Queue a command for the event loop, returns -1 if we are not taking
 * commands or have too many queued up.
 */
static int
chat_command_queue(int type, u_int8_t peer, u_int64_t value,
    const struct chat_quote *quote, const struct chat_topic *topic,
    const struct chat_group *group, const void *data, size_t len)
{
	struct chat_command	*cmd;

	PRECOND(type == CHAT_COMMAND_SEND ||
	    type == CHAT_COMMAND_FORGET ||
	    type == CHAT_COMMAND_STORED);
	PRECOND(data != NULL || len == 0);
	PRECOND(len <= VIGIL_CHAT_MESSAGE_MAX);

	cmd = calloc(1, sizeof(*cmd));
	if (cmd == NULL)
		fatal("calloc: failed to allocate chat command");

	cmd->len = len;
	cmd->type = type;
	cmd->peer = peer;
	cmd->value = value;

	if (quote != NULL)
		memcpy(&cmd->quote, quote, sizeof(*quote));

	if (topic != NULL)
		memcpy(&cmd->topic, topic, sizeof(*topic));

	if (group != NULL)
		memcpy(&cmd->group, group, sizeof(*group));

	if (len > 0)
		memcpy(cmd->data, data, len);

	return (chat_command_insert(cmd));
}

/*
 * Queue a command for the event loop that carries a copy of the given
 * data, returns -1 if we are not taking any or have no room for it.
 * Data we already hold is not copied again.
 */
static int
chat_data_queue(u_int8_t peer, u_int64_t value, const struct chat_group *group,
    u_int64_t topic, int kind, const char *name, const void *data, size_t len)
{
	int				ret;
	u_int64_t			id;
	size_t				off;
	size_t				size;
	u_int32_t			serial;
	struct chat_command		*cmd;
	struct vigil_chat_data_head	head;
	struct chat_blob		*blob;

	PRECOND(name != NULL);
	PRECOND(data != NULL);
	PRECOND(len > 0 && len <= VIGIL_CHAT_DATA_MAX);

	if (!vigil_atomic_read(&accepting))
		return (-1);

	if (chat_data_kind_valid(kind) == -1)
		return (-1);

	if (kind == VIGIL_CHAT_DATA_AVATAR)
		return (-1);

	if (chat_data_name_valid((const u_int8_t *)name, strlen(name)) == -1)
		return (-1);

	size = sizeof(head) + len;

	if (group != NULL)
		size += CHAT_DATA_GROUP;

	if (topic != 0)
		size += sizeof(topic);

	vigil_lock(&lock);
	ret = chat_blob_room(peer, value, size);
	vigil_unlock(&lock);

	if (ret == 1)
		return (0);

	if (ret == -1)
		return (-1);

	memset(&head, 0, sizeof(head));
	head.kind = kind;
	head.len = strlen(name);
	memcpy(head.name, name, head.len);

	if (group != NULL)
		head.kind |= VIGIL_CHAT_DATA_GROUP;

	if (topic != 0)
		head.kind |= VIGIL_CHAT_DATA_TOPIC;

	blob = calloc(1, sizeof(*blob));
	if (blob == NULL)
		fatal("calloc: failed to allocate chat data");

	blob->refs = 1;
	blob->size = size;
	blob->peer = peer;
	blob->ref = value;
	blob->count = (blob->size + VIGIL_CHAT_CHUNK_DATA - 1) /
	    VIGIL_CHAT_CHUNK_DATA;

	blob->data = malloc(blob->size);
	if (blob->data == NULL)
		fatal("malloc: failed to allocate chat data buffer");

	off = sizeof(head);
	memcpy(blob->data, &head, sizeof(head));

	if (group != NULL) {
		id = htobe64(group->id);
		serial = htobe32(group->serial);
		blob->data[off++] = group->owner;
		memcpy(blob->data + off, &id, sizeof(id));
		off += sizeof(id);
		memcpy(blob->data + off, &serial, sizeof(serial));
		off += sizeof(serial);
	}

	if (topic != 0) {
		topic = htobe64(topic);
		memcpy(blob->data + off, &topic, sizeof(topic));
		off += sizeof(topic);
	}

	memcpy(blob->data + off, data, len);

	cmd = calloc(1, sizeof(*cmd));
	if (cmd == NULL)
		fatal("calloc: failed to allocate chat command");

	cmd->peer = peer;
	cmd->blob = blob;
	cmd->value = value;
	cmd->type = CHAT_COMMAND_DATA;

	return (chat_command_insert(cmd));
}

/*
 * Put the given command on the queue for the event loop, returns -1 and
 * frees it if we are not taking any. A command with data we already hold
 * is freed, one with data we have no room for is refused.
 */
static int
chat_command_insert(struct chat_command *cmd)
{
	int		ret;

	PRECOND(cmd != NULL);

	vigil_lock(&lock);

	if (!vigil_atomic_read(&accepting) ||
	    command_count >= CHAT_COMMANDS_MAX) {
		vigil_unlock(&lock);
		chat_command_free(cmd);
		return (-1);
	}

	if (cmd->blob != NULL) {
		ret = chat_blob_room(cmd->blob->peer,
		    cmd->blob->ref, cmd->blob->size);

		if (ret != 0) {
			vigil_unlock(&lock);
			chat_command_free(cmd);
			if (ret == 1)
				return (0);
			return (-1);
		}

		cmd->blob->held = 1;
		blob_bytes += cmd->blob->size;
		TAILQ_INSERT_TAIL(&blobs, cmd->blob, list);
	}

	command_count++;
	TAILQ_INSERT_TAIL(&commands, cmd, list);

	vigil_unlock(&lock);

	vigil_wake_signal();

	return (0);
}

/*
 * Free the given command and the data it carries, if any.
 */
static void
chat_command_free(struct chat_command *cmd)
{
	PRECOND(cmd != NULL);

	if (cmd->blob != NULL)
		chat_blob_release(cmd->blob);

	nyfe_mem_zero(cmd, sizeof(*cmd));
	free(cmd);
}

/*
 * Run all commands the platform gave us.
 */
static void
chat_commands_run(void)
{
	struct chat_command	*cmd;
	u_int8_t		local;
	struct chat_peer	*peer;

	local = vigil->gospel->spi & 0xff;

	for (;;) {
		chat_command_next(&cmd);
		if (cmd == NULL)
			break;

		peer = &peers[cmd->peer];

		switch (cmd->type) {
		case CHAT_COMMAND_SEND:
			if (cmd->quote.hash != 0 && cmd->quote.author == 0)
				cmd->quote.author = local;

			if (cmd->peer != local) {
				chat_outbox_add(peer, cmd->value, &cmd->quote,
				    &cmd->topic, &cmd->group, cmd->data,
				    cmd->len);
			}
			break;
		case CHAT_COMMAND_FORGET:
			chat_outbox_free(peer);
			break;
		case CHAT_COMMAND_STORED:
			chat_token_ack(cmd->value);
			break;
		case CHAT_COMMAND_DATA:
			if (cmd->peer != local)
				chat_outbox_data(peer, cmd->value, cmd->blob);
			break;
		}

		chat_command_free(cmd);
	}
}

/*
 * Drop all commands we did not run.
 */
static void
chat_commands_free(void)
{
	struct chat_command	*cmd;

	for (;;) {
		chat_command_next(&cmd);
		if (cmd == NULL)
			break;

		chat_command_free(cmd);
	}
}

/*
 * Queue an event for the platform, returns -1 if there are too many.
 */
static int
chat_event_queue(int type, struct chat_peer *peer, u_int64_t value,
    const u_int8_t *data, size_t len)
{
	struct chat_event	*evt;

	PRECOND(type == CHAT_EVENT_SENT ||
	    type == CHAT_EVENT_DELIVERED ||
	    type == CHAT_EVENT_AVATAR ||
	    type == CHAT_EVENT_NAME ||
	    type == CHAT_EVENT_AMBRY);
	PRECOND(peer != NULL);
	PRECOND(data != NULL || len == 0);
	PRECOND(len <= VIGIL_CHAT_MESSAGE_MAX);

	evt = calloc(1, sizeof(*evt));
	if (evt == NULL)
		fatal("calloc: failed to allocate chat event");

	evt->type = type;
	evt->value = value;
	evt->peer = peer->id;

	memcpy(evt->name, peer->name, sizeof(evt->name));
	memcpy(evt->nick, peer->nick, sizeof(evt->nick));

	if (len > 0)
		memcpy(evt->text, data, len);

	return (chat_event_insert(evt));
}

/*
 * Tell the platform the ambry generation the given tunnel received.
 */
static void
chat_ambry_report(struct chat_tunnel *tun, u_int32_t generation)
{
	PRECOND(tun != NULL);

	vigil_log(LOG_INFO, "[chat] %02x ambry generation %08x",
	    tun->peer->id, generation);

	if (chat_event_queue(CHAT_EVENT_AMBRY,
	    tun->peer, generation, NULL, 0) == -1) {
		vigil_log(LOG_NOTICE, "[chat] %02x ambry generation %08x, "
		    "too many events to tell", tun->peer->id, generation);
	}
}

/*
 * Take the group and topic we are typing in, an id of 0 means none.
 */
static void
chat_typing_state(u_int8_t *owner, u_int64_t *id, u_int64_t *topic)
{
	PRECOND(owner != NULL);
	PRECOND(id != NULL);
	PRECOND(topic != NULL);

	vigil_lock(&lock);
	*id = typing_group;
	*topic = typing_topic;
	*owner = typing_owner;
	vigil_unlock(&lock);
}

/*
 * Put the given event on the queue for the platform, returns -1 and
 * frees it if there are too many.
 */
static int
chat_event_insert(struct chat_event *evt)
{
	PRECOND(evt != NULL);

	vigil_lock(&lock);

	if (event_count >= CHAT_EVENTS_MAX) {
		vigil_unlock(&lock);
		vigil_log(LOG_NOTICE, "[chat] too many events queued");
		nyfe_mem_zero(evt, sizeof(*evt));
		free(evt);
		return (-1);
	}

	event_count++;
	TAILQ_INSERT_TAIL(&events, evt, list);

	vigil_unlock(&lock);

	return (0);
}

/*
 * Move at most max events for the platform onto the given list.
 */
static void
chat_events_take(struct chat_event_list *list, size_t max)
{
	struct chat_event	*evt;

	PRECOND(list != NULL);

	vigil_lock(&lock);

	while ((evt = TAILQ_FIRST(&events)) != NULL) {
		if (max == 0)
			break;

		TAILQ_REMOVE(&events, evt, list);
		TAILQ_INSERT_TAIL(list, evt, list);

		event_count--;
		max--;
	}

	vigil_unlock(&lock);
}

/*
 * Append the given event as a JSON object to the given buffer.
 */
static void
chat_event_render(struct vigil_buf *buf, struct chat_event *evt, int first)
{
	int		idx, more;

	PRECOND(buf != NULL);
	PRECOND(evt != NULL);

	vigil_buf_appendf(buf, "%s{\"peer\":\"%02x\"",
	    first ? "" : ",", evt->peer);

	if (evt->group.id != 0) {
		vigil_buf_appendf(buf,
		    ",\"owner\":\"%02x\",\"group\":\"%016" PRIx64 "\""
		    ",\"serial\":%u", evt->group.owner, evt->group.id,
		    evt->group.serial);
	}

	switch (evt->type) {
	case CHAT_EVENT_MESSAGE:
		vigil_buf_appendf(buf,
		    ",\"type\":\"message\",\"token\":%" PRIu64
		    ",\"nick\":", evt->value);
		vigil_buf_quote(buf, evt->nick);
		vigil_buf_appendf(buf, ",\"signed\":");
		vigil_buf_quote(buf, evt->name);
		vigil_buf_appendf(buf, ",\"text\":");
		vigil_buf_quote(buf, evt->text);

		if (evt->mention)
			vigil_buf_appendf(buf, ",\"mention\":true");

		if (evt->quote.hash != 0) {
			vigil_buf_appendf(buf,
			    ",\"author\":\"%02x\",\"hash\":\"%016" PRIx64 "\"",
			    evt->quote.author, evt->quote.hash);
		}

		if (evt->topic.id != 0) {
			vigil_buf_appendf(buf, ",\"topic\":\"%016" PRIx64 "\"",
			    evt->topic.id);
		}
		break;
	case CHAT_EVENT_TOPIC:
		vigil_buf_appendf(buf,
		    ",\"type\":\"topic\",\"token\":%" PRIu64
		    ",\"topic\":\"%016" PRIx64 "\",\"color\":\"%06x\""
		    ",\"name\":", evt->value, evt->topic.id, evt->topic.color);
		vigil_buf_quote(buf, evt->text);
		break;
	case CHAT_EVENT_SENT:
		vigil_buf_appendf(buf,
		    ",\"type\":\"sent\",\"ref\":%" PRIu64, evt->value);
		break;
	case CHAT_EVENT_DELIVERED:
		vigil_buf_appendf(buf,
		    ",\"type\":\"delivered\",\"ref\":%" PRIu64, evt->value);
		break;
	case CHAT_EVENT_ROSTER:
		vigil_buf_appendf(buf,
		    ",\"type\":\"roster\",\"token\":%" PRIu64
		    ",\"ended\":%s,\"members\":[", evt->value,
		    evt->group.state == VIGIL_CHAT_GROUP_ENDED ?
		    "true" : "false");

		more = 0;

		for (idx = 1; idx < KYRKA_PEERS_PER_FLOCK; idx++) {
			if (!(evt->group.members[idx / 8] & (1 << (idx % 8))))
				continue;

			vigil_buf_appendf(buf, "%s\"%02x\"",
			    more ? "," : "", idx);
			more = 1;
		}

		vigil_buf_appendf(buf, "],\"name\":");
		vigil_buf_quote(buf, evt->text);
		break;
	case CHAT_EVENT_LEAVE:
		vigil_buf_appendf(buf,
		    ",\"type\":\"leave\",\"token\":%" PRIu64, evt->value);
		break;
	case CHAT_EVENT_DATA:
		vigil_buf_appendf(buf, ",\"type\":\"data\",\"kind\":\"%s\""
		    ",\"token\":%" PRIu64 ",\"size\":%zu,\"name\":",
		    chat_data_kind_name(evt->kind), evt->value, evt->size);
		vigil_buf_quote(buf, evt->text);
		vigil_buf_appendf(buf, ",\"nick\":");
		vigil_buf_quote(buf, evt->nick);
		vigil_buf_appendf(buf, ",\"signed\":");
		vigil_buf_quote(buf, evt->name);

		if (evt->topic.id != 0) {
			vigil_buf_appendf(buf, ",\"topic\":\"%016" PRIx64 "\"",
			    evt->topic.id);
		}
		break;
	case CHAT_EVENT_NAME:
		vigil_buf_appendf(buf, ",\"type\":\"name\",\"nick\":");
		vigil_buf_quote(buf, evt->nick);
		vigil_buf_appendf(buf, ",\"signed\":");
		vigil_buf_quote(buf, evt->text);
		break;
	case CHAT_EVENT_AVATAR:
		vigil_buf_appendf(buf, ",\"type\":\"avatar\",\"token\":%" PRIu64
		    ",\"size\":%zu", evt->value, evt->size);
		break;
	case CHAT_EVENT_AMBRY:
		vigil_buf_appendf(buf, ",\"type\":\"ambry\""
		    ",\"generation\":\"%08" PRIx64 "\"", evt->value);
		break;
	}

	vigil_buf_appendf(buf, "}");
}

/*
 * Drop all events the platform did not pick up.
 */
static void
chat_events_free(void)
{
	struct chat_event	*evt;

	vigil_lock(&lock);

	while ((evt = TAILQ_FIRST(&events)) != NULL) {
		TAILQ_REMOVE(&events, evt, list);
		nyfe_mem_zero(evt, sizeof(*evt));
		free(evt);
	}

	event_count = 0;

	vigil_unlock(&lock);
}

/*
 * Flag the given group message if it mentions us by our nickname, our
 * signed name or our id.
 */
static void
chat_mention_check(struct chat_event *evt)
{
	char		id[3];
	char		name[VIGIL_CHAT_NICK_MAX + 1];

	PRECOND(evt != NULL);

	if (evt->group.id == 0)
		return;

	vigil_lock(&lock);
	memcpy(name, nick, sizeof(name));
	vigil_unlock(&lock);

	(void)snprintf(id, sizeof(id), "%02x", vigil->gospel->spi & 0xff);

	if (chat_mention_name(evt->text, name) ||
	    chat_mention_name(evt->text, id))
		evt->mention = 1;

	if (named && chat_mention_name(evt->text, vigil->gospel->instance))
		evt->mention = 1;
}

/*
 * Returns 1 if the given text has @name in it with no other part of a
 * name right next to it.
 */
static int
chat_mention_name(const char *text, const char *name)
{
	size_t		idx, len;

	PRECOND(text != NULL);
	PRECOND(name != NULL);

	len = strlen(name);
	if (len == 0)
		return (0);

	for (idx = 0; text[idx] != '\0'; idx++) {
		if (text[idx] != '@')
			continue;

		if (idx > 0 && chat_mention_char(text[idx - 1]))
			continue;

		if (strncasecmp(&text[idx + 1], name, len))
			continue;

		if (!chat_mention_char(text[idx + len + 1]))
			return (1);
	}

	return (0);
}

/*
 * Returns 1 if the given character can be part of a name.
 */
static int
chat_mention_char(char c)
{
	if (c == '-' || c == '_')
		return (1);

	return (isalnum((unsigned char)c) != 0);
}

/*
 * Check if the given nickname is one gospel accepts, returns -1 if not.
 */
static int
chat_nick_valid(const char *name)
{
	size_t		idx, len;

	PRECOND(name != NULL);

	len = strlen(name);

	if (len < 2 || len > VIGIL_CHAT_NICK_MAX)
		return (-1);

	for (idx = 0; idx < len; idx++) {
		if (name[idx] >= '0' && name[idx] <= '9')
			continue;
		if (name[idx] >= 'A' && name[idx] <= 'Z')
			continue;
		if (name[idx] >= 'a' && name[idx] <= 'z')
			continue;
		return (-1);
	}

	return (0);
}

/*
 * Replace everything in the given message that is not valid utf-8, or
 * is a control character other than a newline or tab, with a '?'.
 */
static void
chat_utf8_normalize(u_int8_t *data, size_t len)
{
	u_int32_t	cp, min;
	size_t		idx, need, off;

	PRECOND(data != NULL);

	idx = 0;

	while (idx < len) {
		if (data[idx] < 0x80) {
			if (data[idx] < 0x20 &&
			    data[idx] != '\n' && data[idx] != '\t')
				data[idx] = '?';
			idx++;
			continue;
		}

		if ((data[idx] & 0xe0) == 0xc0) {
			need = 1;
			min = 0x80;
			cp = data[idx] & 0x1f;
		} else if ((data[idx] & 0xf0) == 0xe0) {
			need = 2;
			min = 0x800;
			cp = data[idx] & 0x0f;
		} else if ((data[idx] & 0xf8) == 0xf0) {
			need = 3;
			min = 0x10000;
			cp = data[idx] & 0x07;
		} else {
			data[idx++] = '?';
			continue;
		}

		if (need >= len - idx) {
			data[idx++] = '?';
			continue;
		}

		for (off = 1; off <= need; off++) {
			if ((data[idx + off] & 0xc0) != 0x80)
				break;
			cp = (cp << 6) | (data[idx + off] & 0x3f);
		}

		if (off <= need || cp < min || cp > 0x10ffff ||
		    (cp >= 0xd800 && cp <= 0xdfff)) {
			data[idx++] = '?';
			continue;
		}

		idx += need + 1;
	}
}

/*
 * A 64-bit FNV-1a hash of the given data, used to tell resends apart
 * from a peer that started its ids over.
 */
static u_int64_t
chat_frame_hash(const u_int8_t *data, size_t len)
{
	size_t		idx;
	u_int64_t	hash;

	PRECOND(data != NULL);

	hash = 0xcbf29ce484222325ULL;

	for (idx = 0; idx < len; idx++) {
		hash ^= data[idx];
		hash *= 0x100000001b3ULL;
	}

	return (hash);
}

/*
 * Pulls the next command for the event loop, or NULL if there is none.
 */
void
chat_command_next(struct chat_command **cmd)
{
	PRECOND(cmd != NULL);

	vigil_lock(&lock);

	*cmd = TAILQ_FIRST(&commands);

	if (*cmd != NULL) {
		TAILQ_REMOVE(&commands, *cmd, list);
		command_count--;
	}

	vigil_unlock(&lock);
}

/*
 * Free the given message and drop the data it carries, if any.
 */
static void
chat_msg_free(struct chat_msg *msg)
{
	PRECOND(msg != NULL);

	if (msg->blob != NULL)
		chat_blob_release(msg->blob);

	nyfe_mem_zero(msg, sizeof(*msg));
	free(msg);
}

/*
 * Drop a reference to the given data, it is gone with the last one.
 */
static void
chat_blob_release(struct chat_blob *blob)
{
	PRECOND(blob != NULL);
	PRECOND(blob->refs > 0);

	blob->refs--;

	if (blob->refs > 0)
		return;

	if (blob->held) {
		vigil_lock(&lock);
		blob_bytes -= blob->size;
		TAILQ_REMOVE(&blobs, blob, list);
		vigil_unlock(&lock);
	}

	nyfe_mem_zero(blob->data, blob->size);
	free(blob->data);

	nyfe_mem_zero(blob, sizeof(*blob));
	free(blob);
}

/*
 * Check if we can hold data of the given size for the given peer and ref,
 * returns 1 if we already hold it, -1 if there is no room and 0 if there
 * is. Must be called with our lock held.
 */
static int
chat_blob_room(u_int8_t peer, u_int64_t ref, size_t size)
{
	struct chat_blob	*blob;

	TAILQ_FOREACH(blob, &blobs, list) {
		if (blob->peer == peer && blob->ref == ref)
			return (1);
	}

	if ((blob_bytes + size) > CHAT_BLOBS_MAX) {
		vigil_log(LOG_NOTICE, "[chat] no room for more data to send");
		return (-1);
	}

	return (0);
}

/*
 * Create a message for the given data, its frame is the text a peer on
 * gospel gets instead.
 */
static struct chat_msg *
chat_data_msg(u_int64_t ref, struct chat_blob *blob)
{
	int				len;
	u_int8_t			kind;
	struct chat_msg			*msg;
	struct vigil_chat_data_head	*head;
	char				what[VIGIL_CHAT_DATA_NAME_MAX + 16];

	PRECOND(blob != NULL);
	PRECOND(blob->size > sizeof(*head));

	msg = calloc(1, sizeof(*msg));
	if (msg == NULL)
		fatal("calloc: failed to allocate chat message");

	head = (struct vigil_chat_data_head *)blob->data;
	kind = head->kind & ~CHAT_DATA_FLAGS;

	if (kind == VIGIL_CHAT_DATA_IMAGE) {
		(void)snprintf(what, sizeof(what), "an image");
	} else if (kind == VIGIL_CHAT_DATA_VOICE) {
		(void)snprintf(what, sizeof(what), "a voice message");
	} else {
		(void)snprintf(what, sizeof(what), "the file %.*s",
		    head->len, head->name);
	}

	len = snprintf((char *)msg->frame.data, sizeof(msg->frame.data),
	    CHAT_DATA_GOSPEL, what);
	if (len < 0)
		fatal("failed to write the gospel notice for data");

	if ((size_t)len >= sizeof(msg->frame.data))
		fatal("gospel notice for data does not fit");

	blob->refs++;

	msg->ref = ref;
	msg->blob = blob;
	msg->frame.len = htobe16(len);
	msg->frame.type = VIGIL_CHAT_TYPE_TEXT;

	return (msg);
}

/*
 * Add data to the outbox of the given peer unless we have it.
 */
static void
chat_outbox_data(struct chat_peer *peer, u_int64_t ref,
    struct chat_blob *blob)
{
	struct chat_msg		*msg;

	PRECOND(peer != NULL);
	PRECOND(blob != NULL);

	TAILQ_FOREACH(msg, &peer->outbox, wlist) {
		if (msg->ref == ref)
			return;
	}

	msg = chat_data_msg(ref, blob);
	TAILQ_INSERT_TAIL(&peer->outbox, msg, wlist);

	if (peer->tunnel != NULL)
		chat_msg_queue(peer->tunnel, msg);
}

/*
 * Start the xfer for the peer if it indicated it can take more.
 */
static void
chat_xfer_run(struct chat_tunnel *tun)
{
	struct chat_msg		*msg;

	PRECOND(tun != NULL);

	if (tun->xfer == NULL) {
		if (!tun->heard)
			return;

		TAILQ_FOREACH(msg, &tun->peer->outbox, wlist) {
			if (msg->blob == NULL || msg->id == 0)
				continue;

			if (tun->features & VIGIL_CHAT_FEATURE_DATA)
				break;

			chat_data_gospel(tun, msg);
		}

		if (msg == NULL)
			msg = tun->avatar;

		if (msg == NULL)
			return;

		chat_xfer_new(tun, msg);
	}

	chat_xfer_pump(tun);
}

/*
 * Start a new xfer of data for the given tunnel and tack it onto the
 * message.
 */
static void
chat_xfer_new(struct chat_tunnel *tun, struct chat_msg *msg)
{
	struct chat_xfer	*xfer;
	u_int32_t		count;

	PRECOND(tun != NULL);
	PRECOND(tun->xfer == NULL);
	PRECOND(msg != NULL);
	PRECOND(msg->blob != NULL);

	count = msg->blob->count;

	xfer = calloc(1, sizeof(*xfer));
	if (xfer == NULL)
		fatal("calloc: failed to allocate data transfer");

	xfer->acked = calloc(1, (count + 7) / 8);
	if (xfer->acked == NULL)
		fatal("calloc: failed to allocate data transfer acks");

	xfer->sent = calloc(count, sizeof(*xfer->sent));
	if (xfer->sent == NULL)
		fatal("calloc: failed to allocate data transfer times");

	xfer->msg = msg;
	xfer->tick = vigil->now;
	xfer->last_ack = vigil->now;

	xfer->rtt = CHAT_RTT_START;
	xfer->rate = CHAT_RATE_START;

	tun->xfer = xfer;

	transfers++;
	vigil_log(LOG_INFO, "[chat] %02x sending data of %zu bytes",
	    tun->peer->id, msg->blob->size);
}

/*
 * Stop the data transfer on the given tunnel, if there is one.
 */
static void
chat_xfer_free(struct chat_tunnel *tun)
{
	struct chat_xfer	*xfer;

	PRECOND(tun != NULL);

	xfer = tun->xfer;
	if (xfer == NULL)
		return;

	if (!xfer->done)
		transfers--;

	free(xfer->acked);
	free(xfer->sent);
	free(xfer);

	tun->xfer = NULL;
}

/*
 * Send as many chunks of the current data as the rate allows.
 *
 * Once our peer has all of them we ask for its ack every now and then by
 * sending the first chunk again, if we did not hear from it for a while we
 * start over.
 */
static void
chat_xfer_pump(struct chat_tunnel *tun)
{
	struct chat_xfer	*xfer;
	u_int32_t		count, idx, burst;

	PRECOND(tun != NULL);
	PRECOND(tun->xfer != NULL);

	xfer = tun->xfer;
	count = xfer->msg->blob->count;

	if (xfer->done) {
		if (vigil->now < xfer->probe)
			return;

		xfer->probe = vigil->now + CHAT_DATA_PROBE;
		chat_chunk_send(tun, 0);
		return;
	}

	if ((vigil->now - xfer->last_ack) > CHAT_DATA_STALL) {
		vigil_log(LOG_NOTICE, "[chat] %02x data stalled, resending",
		    tun->peer->id);
		chat_xfer_restart(xfer);
	}

	xfer->credit += (vigil->now - xfer->tick) * xfer->rate;
	xfer->tick = vigil->now;

	if (xfer->credit > CHAT_CHUNK_BURST * 1000)
		xfer->credit = CHAT_CHUNK_BURST * 1000;

	xfer->scan = xfer->base;

	for (burst = 0; burst < CHAT_CHUNK_BURST; burst++) {
		if (xfer->credit < 1000)
			break;

		if (chat_xfer_pick(xfer, count, &idx) == -1)
			break;

		chat_chunk_send(tun, idx);
		xfer->credit -= 1000;
	}
}

/*
 * Start sending the data on the given xfer from the first chunk again.
 */
static void
chat_xfer_restart(struct chat_xfer *xfer)
{
	u_int32_t	count;

	PRECOND(xfer != NULL);

	count = xfer->msg->blob->count;

	memset(xfer->acked, 0, (count + 7) / 8);
	memset(xfer->sent, 0, count * sizeof(*xfer->sent));

	xfer->done = 0;
	xfer->base = 0;
	xfer->next = 0;
	xfer->credit = 0;
	xfer->rtt_min = 0;
	xfer->rtt = CHAT_RTT_START;
	xfer->rate = CHAT_RATE_START;
	xfer->tick = vigil->now;
	xfer->last_ack = vigil->now;
}

/*
 * Pick the next chunk to send.
 */
static int
chat_xfer_pick(struct chat_xfer *xfer, u_int32_t count, u_int32_t *idx)
{
	u_int64_t	rto, flight;

	PRECOND(xfer != NULL);
	PRECOND(idx != NULL);

	rto = xfer->rtt + xfer->rtt / 2 + 30;

	for (; xfer->scan < xfer->next; xfer->scan++) {
		if (chat_bits_test(xfer->acked, xfer->scan))
			continue;

		if ((vigil->now - xfer->sent[xfer->scan]) < rto)
			continue;

		*idx = xfer->scan++;
		return (0);
	}

	if (xfer->next >= count)
		return (-1);

	flight = (xfer->rate * xfer->rtt * 2) / 1000 + CHAT_CHUNK_BURST;
	if ((xfer->next - xfer->base) >= flight)
		return (-1);

	*idx = xfer->next++;

	return (0);
}

/*
 * Our peer told us which chunks of the data it has.
 */
static void
chat_xfer_ack(struct chat_tunnel *tun, u_int64_t id, const u_int8_t *data)
{
	struct vigil_chat_data_ack	ack;
	struct chat_xfer		*xfer;
	u_int32_t			idx, chunk, base, count, latest;

	PRECOND(tun != NULL);
	PRECOND(data != NULL);

	if (tun->xfer == NULL)
		return;

	xfer = tun->xfer;
	if (xfer->msg->id != id)
		return;

	memcpy(&ack, data, sizeof(ack));

	count = xfer->msg->blob->count;
	base = be32toh(ack.base);
	latest = be32toh(ack.latest);

	if (base > count)
		return;

	if (xfer->done) {
		if (base == count)
			return;

		if (latest != 0)
			return;

		vigil_log(LOG_NOTICE, "[chat] %02x lost the data, resending",
		    tun->peer->id);

		chat_xfer_restart(xfer);
		transfers++;
	}

	for (idx = xfer->base; idx < base; idx++)
		chat_bits_set(xfer->acked, idx);

	for (idx = 0; idx < sizeof(ack.bits) * 8; idx++) {
		chunk = base + 1 + idx;
		if (chunk >= count)
			break;

		if (ack.bits[idx / 8] & (1 << (idx % 8)))
			chat_bits_set(xfer->acked, chunk);
	}

	if (base > xfer->base)
		xfer->base = base;

	/* After we started over our peer may already have more than we sent. */
	if (xfer->next < xfer->base)
		xfer->next = xfer->base;

	xfer->last_ack = vigil->now;
	if (latest < count && xfer->sent[latest] != 0)
		chat_xfer_pace(xfer, vigil->now - xfer->sent[latest]);

	if (xfer->base < count || xfer->done)
		return;

	xfer->done = 1;
	xfer->probe = vigil->now + CHAT_DATA_PROBE;

	transfers--;
	vigil_log(LOG_INFO, "[chat] %02x has all of the data", tun->peer->id);
}

/*
 * Adjust the data rate of a xfer based on the rtt we saw.
 */
static void
chat_xfer_pace(struct chat_xfer *xfer, u_int64_t rtt)
{
	PRECOND(xfer != NULL);

	if (xfer->rtt_min == 0 || rtt < xfer->rtt_min)
		xfer->rtt_min = rtt;

	xfer->rtt = (xfer->rtt * 7 + rtt) / 8;

	if (rtt <= xfer->rtt_min + xfer->rtt_min / 4 + 10) {
		xfer->rate += xfer->rate / 8 + 8;
		if (xfer->rate > CHAT_RATE_MAX)
			xfer->rate = CHAT_RATE_MAX;
	} else if (rtt > xfer->rtt_min * 2 + 20) {
		xfer->rate -= xfer->rate / 8;
		if (xfer->rate < CHAT_RATE_MIN)
			xfer->rate = CHAT_RATE_MIN;
	}
}

/*
 * Send a chunk out on our tunnel.
 */
static void
chat_chunk_send(struct chat_tunnel *tun, u_int32_t idx)
{
	struct kyrka_packet	pkt;
	u_int8_t		*ptr;
	struct chat_xfer	*xfer;
	struct chat_blob	*blob;
	struct vigil_chat_chunk	*chunk;
	size_t			max, off, len;

	PRECOND(tun != NULL);
	PRECOND(tun->xfer != NULL);

	xfer = tun->xfer;
	blob = xfer->msg->blob;
	VERIFY(idx < blob->count);

	ptr = kyrka_packet_databuf(tun->ctx, &pkt, &max);
	if (ptr == NULL)
		fatal("failed to get chat data buffer");

	if (sizeof(*chunk) > max)
		fatal("chat chunk does not fit");

	chunk = (struct vigil_chat_chunk *)ptr;
	memset(chunk, 0, sizeof(*chunk));

	off = idx * VIGIL_CHAT_CHUNK_DATA;
	len = blob->size - off;

	if (len > VIGIL_CHAT_CHUNK_DATA)
		len = VIGIL_CHAT_CHUNK_DATA;

	chunk->id = htobe64(xfer->msg->id);
	chunk->index = htobe32(idx);
	chunk->count = htobe32(blob->count);
	chunk->size = htobe32(blob->size);
	memcpy(chunk->data, blob->data + off, len);

	pkt.length = sizeof(*chunk);

	if (vigil_inet_match(&tun->addr, &tun->cathedral.addr))
		pkt.shroud = KYRKA_PACKET_SHROUD_CATHEDRAL;
	else
		pkt.shroud = KYRKA_PACKET_SHROUD_PEER;

	if (kyrka_heaven_input(tun->ctx, &pkt) == -1 &&
	    kyrka_last_error(tun->ctx) != KYRKA_ERROR_NO_TX_KEY) {
		vigil_log(LOG_NOTICE, "[chat] kyrka_heaven_input: %d",
		    kyrka_last_error(tun->ctx));
	}

	xfer->sent[idx] = vigil->now;

	if (xfer->msg->sent == 0) {
		if (chat_event_queue(CHAT_EVENT_SENT,
		    tun->peer, xfer->msg->ref, NULL, 0) != -1)
			xfer->msg->sent = 1;
	}
}

/*
 * We have received a chunk of data from our peer.
 */
static void
chat_chunk_recv(struct chat_tunnel *tun, const void *data)
{
	u_int64_t			id;
	size_t				len;
	struct chat_rxfer		*xfer;
	struct chat_seen		*seen;
	const struct vigil_chat_chunk	*chunk;
	u_int32_t			idx, size, count, index;

	PRECOND(tun != NULL);
	PRECOND(data != NULL);

	chunk = data;

	id = be64toh(chunk->id);
	size = be32toh(chunk->size);
	index = be32toh(chunk->index);
	count = be32toh(chunk->count);

	if (id == 0)
		return;

	if (count == 0 || count > CHAT_CHUNKS_MAX)
		return;

	if (size <= sizeof(struct vigil_chat_data_head))
		return;

	if (size > CHAT_WIRE_MAX)
		return;

	if (size > count * VIGIL_CHAT_CHUNK_DATA)
		return;

	if (size <= ((count - 1) * VIGIL_CHAT_CHUNK_DATA))
		return;

	if (index >= count)
		return;

	for (idx = 0; idx < CHAT_SEEN_MAX; idx++) {
		seen = &tun->seen[idx];
		if (seen->id == id && seen->size == size)
			break;
	}

	if (idx < CHAT_SEEN_MAX) {
		if (seen->token == 0) {
			chat_ack_queue(tun, id);
			chat_data_ack(tun, id, NULL, count);
			return;
		}

		if ((vigil->now - seen->age) < CHAT_DATA_WAIT) {
			chat_data_ack(tun, id, NULL, count);
			return;
		}

		seen->id = 0;
		seen->token = 0;
	}

	xfer = tun->rxfer;

	if (xfer != NULL) {
		if (xfer->id != id || xfer->count != count ||
		    xfer->size != size) {
			chat_rxfer_free(tun);
			xfer = NULL;
		}
	}

	if (xfer == NULL) {
		if (receiving >= CHAT_RXFERS_MAX)
			return;

		xfer = chat_rxfer_new(tun, id, count, size);
	}

	len = VIGIL_CHAT_CHUNK_DATA;
	if (index == count - 1)
		len = size - (index * VIGIL_CHAT_CHUNK_DATA);

	if (!chat_bits_test(xfer->bits, index)) {
		memcpy(&xfer->data[index * VIGIL_CHAT_CHUNK_DATA],
		    chunk->data, len);
		chat_bits_set(xfer->bits, index);
		xfer->got++;
	}

	xfer->unacked++;
	xfer->latest = index;
	xfer->last = vigil->now;

	if (xfer->ack_due == 0)
		xfer->ack_due = vigil->now + CHAT_ACK_DELAY;

	while (xfer->base < count && chat_bits_test(xfer->bits, xfer->base))
		xfer->base++;

	if (xfer->got < count) {
		if (xfer->unacked >= CHAT_ACK_CHUNKS)
			chat_data_ack(tun, id, xfer, count);
		return;
	}

	if (chat_data_arrived(tun, xfer) == -1) {
		chat_rxfer_free(tun);
		return;
	}

	chat_data_ack(tun, id, NULL, count);
	chat_rxfer_free(tun);
}

/*
 * Prepare to receive an xfer from a peer.
 */
static struct chat_rxfer *
chat_rxfer_new(struct chat_tunnel *tun, u_int64_t id, u_int32_t count,
    u_int32_t size)
{
	struct chat_rxfer	*xfer;

	PRECOND(tun != NULL);
	PRECOND(tun->rxfer == NULL);
	PRECOND(count > 0 && count <= CHAT_CHUNKS_MAX);
	PRECOND(size > 0 && size <= CHAT_WIRE_MAX);

	xfer = calloc(1, sizeof(*xfer));
	if (xfer == NULL)
		fatal("calloc: failed to allocate data transfer");

	xfer->bits = calloc(1, (count + 7) / 8);
	if (xfer->bits == NULL)
		fatal("calloc: failed to allocate data transfer bits");

	xfer->data = calloc(1, size);
	if (xfer->data == NULL)
		fatal("calloc: failed to allocate data transfer data");

	xfer->id = id;
	xfer->size = size;
	xfer->count = count;

	tun->rxfer = xfer;

	receiving++;
	transfers++;

	vigil_log(LOG_INFO,
	    "[chat] %02x is sending us data of %u bytes", tun->peer->id, size);

	return (xfer);
}

/*
 * Drop the data we are taking on the given tunnel, if any.
 */
static void
chat_rxfer_free(struct chat_tunnel *tun)
{
	struct chat_rxfer	*xfer;

	PRECOND(tun != NULL);

	if (tun->rxfer == NULL)
		return;

	xfer = tun->rxfer;

	if (xfer->data != NULL) {
		nyfe_mem_zero(xfer->data, xfer->size);
		free(xfer->data);
	}

	free(xfer->bits);
	free(xfer);

	tun->rxfer = NULL;

	receiving--;
	transfers--;
}

/*
 * Send the ack for the data we are receiving on the given tunnel when it is
 * due, drop the data if our peer stopped sending it.
 */
static void
chat_rxfer_check(struct chat_tunnel *tun)
{
	struct chat_rxfer	*xfer;

	PRECOND(tun != NULL);

	if (tun->rxfer == NULL)
		return;

	xfer = tun->rxfer;

	if ((vigil->now - xfer->last) > CHAT_DATA_STALL) {
		vigil_log(LOG_NOTICE, "[chat] %02x stopped sending its data",
		    tun->peer->id);
		chat_rxfer_free(tun);
		return;
	}

	if (xfer->ack_due == 0)
		return;

	if (vigil->now >= xfer->ack_due)
		chat_data_ack(tun, xfer->id, xfer, xfer->count);
}

/*
 * Queue an ack for the data with the given id telling our peer which chunks
 * we have, all of them if we no longer have a transfer for it.
 */
static void
chat_data_ack(struct chat_tunnel *tun, u_int64_t id, struct chat_rxfer *xfer,
    u_int32_t count)
{
	struct chat_msg			*msg;
	struct vigil_chat_data_ack	*ack;
	u_int32_t			idx, chunk;

	PRECOND(tun != NULL);
	PRECOND(count > 0);

	msg = calloc(1, sizeof(*msg));
	if (msg == NULL)
		fatal("calloc: failed to allocate data ack");

	msg->frame.id = htobe64(id);
	msg->frame.type = VIGIL_CHAT_TYPE_DATA_ACK;
	msg->frame.len = htobe16(sizeof(*ack));

	ack = (struct vigil_chat_data_ack *)msg->frame.data;

	if (xfer == NULL) {
		ack->base = htobe32(count);
		ack->latest = htobe32(count - 1);
	} else {
		ack->base = htobe32(xfer->base);
		ack->latest = htobe32(xfer->latest);

		for (idx = 0; idx < sizeof(ack->bits) * 8; idx++) {
			chunk = xfer->base + 1 + idx;
			if (chunk >= count)
				break;

			if (chat_bits_test(xfer->bits, chunk))
				ack->bits[idx / 8] |= 1 << (idx % 8);
		}

		xfer->ack_due = 0;
		xfer->unacked = 0;
	}

	msg->pending = 1;
	TAILQ_INSERT_HEAD(&tun->sendq, msg, slist);
}

/*
 * The peer of the given tunnel cannot take data, it gets a message telling
 * it so instead.
 */
static void
chat_data_gospel(struct chat_tunnel *tun, struct chat_msg *msg)
{
	PRECOND(tun != NULL);
	PRECOND(msg != NULL);
	PRECOND(msg->blob != NULL);
	PRECOND(msg->pending == 0);

	vigil_log(LOG_INFO, "[chat] %02x cannot take parcels", tun->peer->id);

	chat_blob_release(msg->blob);
	msg->blob = NULL;

	msg->pending = 1;
	TAILQ_INSERT_TAIL(&tun->sendq, msg, slist);
}

/*
 * We have all of the data on the given xfer, hand it to the platform and
 * remember it so we ack it once the platform stored it. Data we cannot use
 * is acked right away so our peer stops sending it.
 *
 * Returns -1 if we cannot hold the data right now, our peer will send it
 * again.
 */
static int
chat_data_arrived(struct chat_tunnel *tun, struct chat_rxfer *xfer)
{
	size_t				off;
	u_int8_t			kind;
	u_int64_t			held;
	u_int64_t			topic;
	struct chat_group		group;
	struct chat_seen		*seen;
	struct vigil_chat_data_head	*head;
	struct chat_parcel		*parcel;

	PRECOND(tun != NULL);
	PRECOND(xfer != NULL);
	PRECOND(xfer->data != NULL);
	PRECOND(xfer->size > sizeof(*head));

	head = (struct vigil_chat_data_head *)xfer->data;
	kind = head->kind & ~CHAT_DATA_FLAGS;

	if (chat_data_tags(xfer, kind, &off, &group, &topic) == -1) {
		vigil_log(LOG_NOTICE,
		    "[chat] %02x sent data in a bad topic or group",
		    tun->peer->id);
		chat_data_discard(tun, xfer);
		return (0);
	}

	if (chat_data_kind_valid(kind) == -1) {
		vigil_log(LOG_NOTICE, "[chat] %02x sent unknown data",
		    tun->peer->id);
		chat_data_discard(tun, xfer);
		return (0);
	}

	if (head->len > sizeof(head->name)) {
		vigil_log(LOG_NOTICE, "[chat] %02x sent data with a bad name",
		    tun->peer->id);
		chat_data_discard(tun, xfer);
		return (0);
	}

	chat_utf8_normalize(head->name, head->len);

	if (chat_data_name_valid(head->name, head->len) == -1) {
		vigil_log(LOG_NOTICE, "[chat] %02x sent data with a bad name",
		    tun->peer->id);
		chat_data_discard(tun, xfer);
		return (0);
	}

	if (kind == VIGIL_CHAT_DATA_AVATAR) {
		if (xfer->size > sizeof(*head) + VIGIL_CHAT_AVATAR_MAX) {
			vigil_log(LOG_NOTICE,
			    "[chat] %02x sent an avatar that is too large",
			    tun->peer->id);
			chat_data_discard(tun, xfer);
			return (0);
		}
	}

	vigil_lock(&lock);

	if ((parcel_bytes + xfer->size) > CHAT_PARCELS_MAX) {
		vigil_unlock(&lock);
		vigil_log(LOG_NOTICE, "[chat] %02x data has to wait",
		    tun->peer->id);
		return (-1);
	}

	vigil_unlock(&lock);

	held = 0;

	if (kind == VIGIL_CHAT_DATA_AVATAR) {
		held = chat_avatar_hash(xfer->data + sizeof(*head),
		    xfer->size - sizeof(*head));
	}

	parcel = calloc(1, sizeof(*parcel));
	if (parcel == NULL)
		fatal("calloc: failed to allocate data");

	parcel->off = off;
	parcel->topic = topic;
	parcel->data = xfer->data;
	parcel->size = xfer->size;
	parcel->age = vigil->now;
	parcel->token = ++tokens;
	memcpy(&parcel->group, &group, sizeof(group));

	xfer->data = NULL;

	if (chat_data_report(tun, parcel) == -1) {
		chat_parcel_free(parcel);
		return (-1);
	}

	vigil_lock(&lock);
	parcel_bytes += parcel->size;
	TAILQ_INSERT_TAIL(&parcels, parcel, list);
	vigil_unlock(&lock);

	seen = &tun->seen[tun->seen_next++ % CHAT_SEEN_MAX];

	seen->hash = 0;
	seen->avatar = held;
	seen->id = xfer->id;
	seen->size = xfer->size;
	seen->token = parcel->token;
	seen->age = vigil->now;

	if (kind != VIGIL_CHAT_DATA_AVATAR)
		tun->peer->active = vigil->now;

	vigil_log(LOG_INFO, "[chat] %02x sent us data of %zu bytes",
	    tun->peer->id, parcel->size);

	return (0);
}

/*
 * Take the group and topic the data on the given xfer is in and where the
 * data starts, returns -1 if they are not ones we can use.
 */
static int
chat_data_tags(struct chat_rxfer *xfer, u_int8_t kind, size_t *off,
    struct chat_group *group, u_int64_t *topic)
{
	u_int64_t			id;
	u_int8_t			flags;
	struct vigil_chat_data_head	*head;
	u_int32_t			serial;

	PRECOND(xfer != NULL);
	PRECOND(xfer->data != NULL);
	PRECOND(off != NULL);
	PRECOND(group != NULL);
	PRECOND(topic != NULL);

	memset(group, 0, sizeof(*group));

	*topic = 0;
	*off = sizeof(*head);

	head = (struct vigil_chat_data_head *)xfer->data;
	flags = head->kind & CHAT_DATA_FLAGS;

	if (flags == 0)
		return (0);

	if (kind == VIGIL_CHAT_DATA_AVATAR)
		return (-1);

	if (flags & VIGIL_CHAT_DATA_GROUP) {
		if (xfer->size <= *off + CHAT_DATA_GROUP)
			return (-1);

		memcpy(&id, xfer->data + *off + 1, sizeof(id));
		memcpy(&serial, xfer->data + *off + 1 + sizeof(id),
		    sizeof(serial));

		group->id = be64toh(id);
		group->serial = be32toh(serial);
		group->owner = xfer->data[*off];
		group->type = VIGIL_CHAT_TYPE_TEXT;

		*off += CHAT_DATA_GROUP;

		if (group->id == 0)
			return (-1);

		if (group->owner == 0)
			return (-1);

		if (group->owner >= KYRKA_PEERS_PER_FLOCK)
			return (-1);
	}

	if (flags & VIGIL_CHAT_DATA_TOPIC) {
		if (xfer->size <= *off + sizeof(*topic))
			return (-1);

		memcpy(topic, xfer->data + *off, sizeof(*topic));
		*topic = be64toh(*topic);
		*off += sizeof(*topic);

		if (*topic == 0)
			return (-1);
	}

	return (0);
}

/*
 * Remember the data on the given xfer as done and ack it without handing it
 * to the platform.
 */
static void
chat_data_discard(struct chat_tunnel *tun, struct chat_rxfer *xfer)
{
	struct chat_seen	*seen;

	PRECOND(tun != NULL);
	PRECOND(xfer != NULL);

	seen = &tun->seen[tun->seen_next++ % CHAT_SEEN_MAX];

	seen->hash = 0;
	seen->token = 0;
	seen->avatar = 0;
	seen->id = xfer->id;
	seen->size = xfer->size;
	seen->age = vigil->now;

	chat_ack_queue(tun, xfer->id);
}

/*
 * Tell the platform data arrived under the given token, returns -1
 * if there are too many events.
 */
static int
chat_data_report(struct chat_tunnel *tun, struct chat_parcel *parcel)
{
	struct chat_event		*evt;
	struct vigil_chat_data_head	*head;

	PRECOND(tun != NULL);
	PRECOND(parcel != NULL);
	PRECOND(parcel->size > sizeof(*head));

	head = (struct vigil_chat_data_head *)parcel->data;

	evt = calloc(1, sizeof(*evt));
	if (evt == NULL)
		fatal("calloc: failed to allocate chat event");

	evt->peer = tun->peer->id;
	evt->value = parcel->token;
	evt->type = CHAT_EVENT_DATA;
	evt->topic.id = parcel->topic;
	evt->size = parcel->size - parcel->off;
	evt->kind = head->kind & ~CHAT_DATA_FLAGS;

	memcpy(&evt->group, &parcel->group, sizeof(parcel->group));

	if (evt->kind == VIGIL_CHAT_DATA_AVATAR)
		evt->type = CHAT_EVENT_AVATAR;

	memcpy(evt->text, head->name, head->len);
	memcpy(evt->name, tun->peer->name, sizeof(evt->name));
	memcpy(evt->nick, tun->peer->nick, sizeof(evt->nick));

	return (chat_event_insert(evt));
}

/*
 * Returns -1 if the given kind of data is not one we know.
 */
static int
chat_data_kind_valid(int kind)
{
	switch (kind) {
	case VIGIL_CHAT_DATA_IMAGE:
	case VIGIL_CHAT_DATA_FILE:
	case VIGIL_CHAT_DATA_VOICE:
	case VIGIL_CHAT_DATA_AVATAR:
		return (0);
	}

	return (-1);
}

/*
 * Returns the name the platform knows the given kind of data by.
 */
static const char *
chat_data_kind_name(int kind)
{
	switch (kind) {
	case VIGIL_CHAT_DATA_IMAGE:
		return ("image");
	case VIGIL_CHAT_DATA_VOICE:
		return ("voice");
	}

	return ("file");
}

/*
 * Check if the given name is a plain file name we accept for data, returns
 * -1 if not.
 */
static int
chat_data_name_valid(const u_int8_t *name, size_t len)
{
	size_t		idx;

	PRECOND(name != NULL);

	if (len == 0)
		return (-1);

	if (len > VIGIL_CHAT_DATA_NAME_MAX)
		return (-1);

	if (name[0] == '.')
		return (-1);

	for (idx = 0; idx < len; idx++) {
		if (name[idx] < 0x20)
			return (-1);

		if (name[idx] == 0x7f)
			return (-1);

		if (name[idx] == '/')
			return (-1);

		if (name[idx] == '\\')
			return (-1);
	}

	return (0);
}

/*
 * Free the given data that arrived.
 */
static void
chat_parcel_free(struct chat_parcel *parcel)
{
	PRECOND(parcel != NULL);

	nyfe_mem_zero(parcel->data, parcel->size);
	free(parcel->data);

	nyfe_mem_zero(parcel, sizeof(*parcel));
	free(parcel);
}

/*
 * Drop the parcels that the platform did not take in time.
 */
static void
chat_parcels_expire(void)
{
	struct chat_parcel	*parcel, *next;

	vigil_lock(&lock);

	for (parcel = TAILQ_FIRST(&parcels); parcel != NULL; parcel = next) {
		next = TAILQ_NEXT(parcel, list);

		if ((vigil->now - parcel->age) < CHAT_DATA_WAIT)
			continue;

		TAILQ_REMOVE(&parcels, parcel, list);
		parcel_bytes -= parcel->size;
		chat_parcel_free(parcel);
	}

	vigil_unlock(&lock);
}

/*
 * Drop all parcels that arrived.
 */
static void
chat_parcels_free(void)
{
	struct chat_parcel	*parcel;

	vigil_lock(&lock);

	while ((parcel = TAILQ_FIRST(&parcels)) != NULL) {
		TAILQ_REMOVE(&parcels, parcel, list);
		chat_parcel_free(parcel);
	}

	parcel_bytes = 0;
	vigil_unlock(&lock);
}

/*
 * Returns 1 if the bit for the given chunk is set.
 */
static int
chat_bits_test(const u_int8_t *bits, u_int32_t idx)
{
	PRECOND(bits != NULL);

	return ((bits[idx / 8] >> (idx % 8)) & 1);
}

/*
 * Set the bit for the given chunk.
 */
static void
chat_bits_set(u_int8_t *bits, u_int32_t idx)
{
	PRECOND(bits != NULL);

	bits[idx / 8] |= 1 << (idx % 8);
}
