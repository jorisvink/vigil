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
#include <sys/statvfs.h>

#include <arpa/inet.h>
#include <netinet/in.h>

#include <ctype.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "vigil.h"

#include <libkyrka/libnyfe.h>

/*
 * The version of the sync between a phone and its desktop. Bump it
 * whenever anything in the companion sync changes, a phone and its
 * desktop only sync when they run the same version.
 */
#define VIGIL_COMPANION_VERSION		3

/* The magic in front of the version, older builds do not have it. */
#define VIGIL_COMPANION_MAGIC		0x5643

/* The commands the platform gives us. */
#define CHAT_COMMAND_SEND		1
#define CHAT_COMMAND_FORGET		2
#define CHAT_COMMAND_STORED		3
#define CHAT_COMMAND_DATA		4
#define CHAT_COMMAND_DROP		5
#define CHAT_COMMAND_CALL		6
#define CHAT_COMMAND_SYNC		7
#define CHAT_COMMAND_REQUEST		8
#define CHAT_COMMAND_INPUT		9

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
#define CHAT_EVENT_RETRACT		11
#define CHAT_EVENT_ECHO			12
#define CHAT_EVENT_MIRROR		13
#define CHAT_EVENT_DESKTOP		14
#define CHAT_EVENT_SAFETY		15
#define CHAT_EVENT_ASK			16

/* The kinds of desktop event, unsent and queued are for a desktop. */
#define CHAT_DESKTOP_RESYNC		1
#define CHAT_DESKTOP_ASK		2
#define CHAT_DESKTOP_UNSENT		3
#define CHAT_DESKTOP_QUEUED		4

/* Time between heartbeats we send to our peers. */
#define CHAT_HEARTBEAT			(15 * 1000)

/* Time before we re-send an un-acked message. */
#define CHAT_RESEND			(5 * 1000)

/* Time we keep a one-to-one tunnel alive. */
#define CHAT_IDLE			(60 * 1000)

/* Time we keep signaling a peer for a group message we just queued. */
#define CHAT_RETRY_EAGER		(5 * 60 * 1000)

/* Time we signal a peer for a group message each time we try again. */
#define CHAT_RETRY_WINDOW		(60 * 1000)

/* The first and the longest time between two tries for a group message. */
#define CHAT_RETRY_START		(5 * 60 * 1000)
#define CHAT_RETRY_MAX			(60 * 60 * 1000)

/* Time until we consider a peer offline. */
#define CHAT_RX_TIMEOUT			(40 * 1000)

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

/* Max size of a parcel we put on the wire, a mirror with data is largest. */
#define CHAT_WIRE_MAX			\
    (VIGIL_CHAT_DATA_MAX + sizeof(struct vigil_chat_data_head) + \
    CHAT_PARCEL_HEAD + CHAT_COMPANION_BATCH)

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

/* The name our avatar goes under. */
#define CHAT_AVATAR_NAME		"avatar"

/* The name of the data we send over our companion tunnel. */
#define CHAT_COMPANION_NAME		"companion"

/* The bit in the ref of a parcel to our phone that holds a request. */
#define CHAT_REQUEST_REF		(1ULL << 63)

/* The most parcels and bytes we hold in memory for our companion. */
#define CHAT_COMPANION_ITEMS		4096
#define CHAT_COMPANION_BYTES		(32 * 1024 * 1024)

/* The most bytes our journal holds, and its part of the free space. */
#define CHAT_JOURNAL_MAX		(1024ULL * 1024 * 1024)
#define CHAT_JOURNAL_SHARE		10

/* Time between checks of the free space on our disk. */
#define CHAT_JOURNAL_MEASURE		(60 * 60 * 1000)

/* The bytes an entry needs in our journal on top of its JSON and data. */
#define CHAT_JOURNAL_EXTRA		64

/* What our platform hands us for our desktop, an entry or a mark. */
#define CHAT_SYNC_ENTRY			0
#define CHAT_SYNC_RESET			1
#define CHAT_SYNC_DONE			2

/* The marks our platform puts around a full copy of our chats. */
#define CHAT_SYNC_RESET_JSON		"{\"op\":\"reset\"}"
#define CHAT_SYNC_DONE_JSON		"{\"op\":\"synced\"}"

/* The most bytes of events we send our desktop in one go. */
#define CHAT_COMPANION_BATCH		(32 * 1024)

/* The count, data size and last seq in front of a journal parcel. */
#define CHAT_PARCEL_HEAD		\
    (2 * sizeof(u_int32_t) + sizeof(u_int64_t))

/* The seq and JSON length in front of each entry in a journal parcel. */
#define CHAT_ENTRY_HEAD			(sizeof(u_int64_t) + sizeof(u_int32_t))

/* Time before we try to resolve the cathedral of our companion again. */
#define CHAT_COMPANION_RESOLVE		(30 * 1000)

/* Time between heartbeats to our companion while a call rings or runs. */
#define CHAT_COMPANION_CALL_HEARTBEAT	(1 * 1000)

/*
 * A data block we could be sending.
 */
struct chat_blob {
	int			held;
	u_int64_t		flock;
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
 * there is no reply and an author of 0 is us. For a retract the hash is
 * that of the data being retracted.
 */
struct chat_quote {
	int			retract;
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

	u_int64_t		retry;
	u_int64_t		signal;
	u_int64_t		backoff;

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
 * A message we received and handed to the platform. For a journal parcel
 * from our phone, the seq it runs up to and if we are synced once it is
 * stored.
 */
struct chat_seen {
	u_int64_t		id;
	u_int64_t		age;
	u_int64_t		hash;
	u_int64_t		token;
	u_int64_t		avatar;
	u_int64_t		journal;
	u_int8_t		synced;
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

	int			safety;
	KYRKA			*ctx;
	struct chat_peer	*peer;
	struct vigil_config	*cfg;

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
 * A chat peer, in the flock of the Gospel configuration in its slot. The
 * other side of our companion tunnel has no slot.
 */
struct chat_peer {
	u_int8_t		id;
	u_int8_t		slot;
	u_int64_t		flock;
	u_int64_t		active;
	u_int64_t		pushed;
	u_int64_t		avatar_held;
	int			asked;
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
 * The signaling liturgy of one of our Gospel configurations.
 */
struct chat_liturgy {
	struct vigil_io		io;

	u_int8_t		slot;
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
 * The chats of one of our Gospel configurations. We send our instance to
 * peers once its flock signed it.
 */
struct chat_gospel {
	int			running;
	int			named;
	u_int8_t		local;
	u_int64_t		flock;
	u_int64_t		session;
	struct chat_liturgy	signaling;
	u_int8_t		sent[KYRKA_PEERS_PER_FLOCK];
	struct chat_peer	peers[KYRKA_PEERS_PER_FLOCK];
};

/*
 * The peers of one of our Gospel configurations that we accept or ignore,
 * whose chat is open and those we type to, set from any thread.
 */
struct chat_marks {
	volatile u_int8_t	opened[KYRKA_PEERS_PER_FLOCK];
	volatile u_int8_t	ignored[KYRKA_PEERS_PER_FLOCK];
	volatile u_int8_t	accepted[KYRKA_PEERS_PER_FLOCK];
	volatile u_int8_t	typing[KYRKA_PEERS_PER_FLOCK];
};

/*
 * A command the platform sent to us.
 */
struct chat_command {
	int				type;
	u_int64_t			flock;
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
	u_int64_t			flock;
	u_int8_t			peer;
	u_int64_t			value;
	u_int64_t			token;
	int				mention;

	u_int8_t			*json;
	size_t				json_len;

	struct chat_quote		quote;
	struct chat_topic		topic;
	struct chat_group		group;
	char				name[VIGIL_INSTANCE_MAX];
	char				nick[VIGIL_CHAT_NICK_MAX + 1];
	char				text[VIGIL_CHAT_MESSAGE_MAX + 1];

	TAILQ_ENTRY(chat_event)		list;
};

TAILQ_HEAD(chat_event_list, chat_event);

static void	chat_update(struct chat_gospel *);

static void	chat_liturgy_read(void *);
static void	chat_liturgy_rebind(struct chat_liturgy *);
static void	chat_liturgy_cleanup(struct chat_liturgy *);
static void	chat_liturgy_signal(struct chat_gospel *);
static int	chat_liturgy_init(struct chat_liturgy *, u_int8_t, u_int16_t);
static void	chat_liturgy_cathedral_check(struct chat_liturgy *);
static void	chat_liturgy_event(KYRKA *, union kyrka_event *, void *);
static void	chat_liturgy_send(struct kyrka_packet *, u_int64_t, void *);

static void	chat_retract_frame(struct chat_tunnel *,
		    struct vigil_chat_frame *, const struct chat_msg *);
static void	chat_retract_recv(struct chat_tunnel *, u_int64_t,
		    const struct chat_group *, u_int8_t *, u_int16_t);

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
static int	chat_tunnel_accepted(struct chat_tunnel *);
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
static void	chat_ask_check(struct chat_peer *);
static void	chat_name_report(struct chat_peer *);
static int	chat_signal_check(struct chat_peer *);
static void	chat_utf8_normalize(u_int8_t *, size_t);
static void	chat_ambry_report(struct chat_tunnel *, u_int32_t);
static int	chat_typed(struct chat_peer *);
static void	chat_typing_state(struct chat_peer *, u_int8_t *, u_int64_t *,
		    u_int64_t *);
static void	chat_heaven_input(struct kyrka_packet *, u_int64_t, void *);
static void	chat_hb_recv(struct chat_tunnel *, struct vigil_chat_frame *);
static void	chat_purgatory_input(struct kyrka_packet *, u_int64_t, void *);
static void	chat_hb_name(struct chat_peer *, const struct vigil_chat_hb *);
static void	chat_text_recv(struct chat_tunnel *, u_int64_t,
		    const struct chat_quote *, const struct chat_topic *,
		    const struct chat_group *, u_int8_t *, u_int16_t);
static int	chat_text_empty(const struct chat_quote *,
		    const struct chat_topic *, const struct chat_group *);
static int	chat_text_report(struct chat_tunnel *, u_int64_t,
		    const struct chat_quote *, const struct chat_topic *,
		    const struct chat_group *, const u_int8_t *, size_t);

static void	chat_ack_recv(struct chat_tunnel *, u_int64_t);
static void	chat_ack_queue(struct chat_tunnel *, u_int64_t);

static u_int64_t	chat_frame_hash(const u_int8_t *, size_t);
static void		chat_frame_send(struct chat_tunnel *,
			    struct chat_msg *);

static void	chat_msg_free(struct chat_msg *);
static int	chat_msg_signal(struct chat_msg *);
static void	chat_msg_backoff(struct chat_msg *);
static void	chat_msg_resend(struct chat_tunnel *);
static void	chat_msg_queue(struct chat_tunnel *, struct chat_msg *);
static int	chat_msg_waits(struct chat_tunnel *, struct chat_msg *);

static void	chat_outbox_free(struct chat_peer *);
static int	chat_outbox_signal(struct chat_peer *);
static void	chat_outbox_drop(struct chat_peer *, u_int64_t);
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
static int	chat_command_blob(int, u_int64_t, const char *, size_t,
		    const void *, size_t);
static int	chat_command_queue(int, u_int64_t, u_int8_t, u_int64_t,
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
static int	chat_blob_room(u_int64_t, u_int8_t, u_int64_t, size_t);

static void	chat_parcels_free(void);
static void	chat_parcels_expire(void);
static void	chat_parcel_free(struct chat_parcel *);
static void	chat_parcel_hold(struct chat_parcel *);

static int	chat_data_kind_valid(int);
static int	chat_data_name_valid(const u_int8_t *, size_t);
static int	chat_data_arrived(struct chat_tunnel *, struct chat_rxfer *);
static void	chat_data_discard(struct chat_tunnel *, struct chat_rxfer *);
static void	chat_data_hold(struct chat_tunnel *, struct chat_rxfer *,
		    u_int64_t);
static int	chat_data_report(struct chat_tunnel *, struct chat_parcel *);
static void	chat_data_ack(struct chat_tunnel *, u_int64_t,
		    struct chat_rxfer *, u_int32_t);
static int	chat_data_queue(u_int64_t, u_int8_t, u_int64_t,
		    const struct chat_group *, u_int64_t, int, const char *,
		    const void *, size_t);
static int	chat_data_tags(const u_int8_t *, size_t, size_t *,
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

static int	chat_events_room(void);
static void	chat_events_free(void);
static void	chat_event_free(struct chat_event *);
static int	chat_event_insert(struct chat_event *);
static size_t	chat_event_need(const struct chat_event *);
static void	chat_events_take(struct chat_event_list *, size_t);
static void	chat_event_echo(struct vigil_buf *, struct chat_event *);
static void	chat_event_desktop(struct vigil_buf *, struct chat_event *);
static void	chat_event_members(struct vigil_buf *, const u_int8_t *);
static void	chat_event_render(struct vigil_buf *, struct chat_event *, int);
static int	chat_event_queue(int, struct chat_peer *, u_int64_t,
		    const u_int8_t *, size_t);

static void	chat_companion_load(void);
static void	chat_companion_free(void);
static int	chat_companion_journal(void);
static void	chat_companion_check(void);
static int	chat_companion_room(size_t);
static void	chat_companion_push(struct chat_blob *);
static void	chat_companion_status(struct vigil_buf *);
static int	chat_companion_mismatch(void);
static void	chat_companion_version(const u_int8_t *);
static void	chat_companion_peers(struct vigil_buf *);
static void	chat_companion_hb_recv(struct chat_tunnel *,
		    const struct vigil_chat_frame *);
static void	chat_companion_heartbeat(struct chat_tunnel *,
		    struct vigil_chat_frame *);

static struct chat_blob	*chat_companion_blob(size_t);

static void	chat_phone_pump(void);
static void	chat_phone_self(void);
static void	chat_phone_limit(void);
static void	chat_phone_resync(void);
static int	chat_phone_reserve(size_t);
static void	chat_phone_release(size_t);
static void	chat_phone_acked(u_int64_t);
static size_t	chat_phone_need(size_t, size_t);
static void	chat_phone_sync(struct chat_command *);
static int	chat_phone_changed(void);
static void	chat_phone_state(u_int8_t, struct vigil_companion_phone *);
static void	chat_phone_call(const struct vigil_chat_request *);
static int	chat_phone_entry(const u_int8_t *, size_t, u_int32_t *);
static void	chat_phone_desktop(const struct vigil_companion_desktop *);
static int	chat_phone_valid(const struct vigil_chat_request *, size_t);
static int	chat_phone_request(struct chat_tunnel *, struct chat_rxfer *);
static int	chat_phone_tell(struct chat_event *, const u_int8_t *, size_t);
static int	chat_phone_ask(const struct vigil_chat_request *,
		    const u_int8_t *, size_t, u_int64_t);
static int	chat_phone_send(const struct vigil_chat_request *,
		    const u_int8_t *, size_t, u_int64_t);
static void	chat_phone_entry_add(u_int8_t *, size_t *, u_int64_t,
		    const u_int8_t *, u_int32_t);
static int	chat_phone_write(const char *, size_t, const u_int8_t *,
		    size_t);
static int	chat_phone_queue(const char *, size_t, const u_int8_t *,
		    size_t);

static void	chat_desktop_told(void);
static void	chat_desktop_bit(u_int8_t *, u_int64_t, u_int8_t, int);
static void	chat_desktop_pump(void);
static void	chat_desktop_head(void);
static void	chat_desktop_recall(void);
static void	chat_desktop_unsync(void);
static void	chat_desktop_acked(u_int64_t);
static int	chat_desktop_done(const u_int8_t *, size_t);
static void	chat_desktop_stored(u_int64_t, u_int8_t);
static int	chat_desktop_is(const u_int8_t *, size_t, const char *);
static u_int8_t	chat_desktop_mark(u_int8_t, const u_int8_t *, size_t);
static void	chat_desktop_forward(struct chat_command *);
static void	chat_desktop_state(struct vigil_companion_desktop *);
static void	chat_desktop_unsent(const struct vigil_chat_request *);
static void	chat_desktop_phone(const struct vigil_companion_phone *);
static int	chat_desktop_mirror(struct chat_tunnel *, struct chat_rxfer *);
static void	chat_desktop_request(const struct vigil_chat_request *,
		    const u_int8_t *, size_t);
static int	chat_desktop_queue(const struct vigil_chat_request *,
		    const void *, size_t);
static int	chat_desktop_hold(const struct vigil_chat_request *,
		    const u_int8_t *, size_t);
static void	chat_desktop_queued(const struct vigil_chat_request *,
		    const u_int8_t *, size_t);

/* Are we alive? */
static int				running = 0;

/* Each time we start we increase the session counter. */
static u_int64_t			session = 0;

/* If we are accepting commands or not. */
static volatile int			accepting = 0;

/* Incremental token id for the messages we receive. */
static u_int64_t			tokens = 0;

/* The chats of each of our Gospel configurations. */
static struct chat_gospel		gospels[VIGIL_GOSPEL_MAX];

/* The marks on the peers of each of our Gospel configurations. */
static struct chat_marks		marks[VIGIL_GOSPEL_MAX];

/* Protects our commands, events, parcels and blobs. */
static pthread_mutex_t			lock = PTHREAD_MUTEX_INITIALIZER;

/* Our nickname, empty if we have none. */
static char				nick[VIGIL_CHAT_NICK_MAX + 1];

/*
 * The group and topic we are typing in, an id of 0 means none. Only the
 * owner and members of that group in its flock learn about it.
 */
static u_int8_t				typing_owner = 0;
static u_int64_t			typing_flock = 0;
static u_int64_t			typing_group = 0;
static u_int64_t			typing_topic = 0;
static u_int8_t				typing_members[VIGIL_CHAT_GROUP_ROSTER];

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
 * The other side of our companion tunnel, its configuration and if we are
 * the phone or its desktop, 0 without a companion.
 */
static struct chat_peer			companion;
static u_int8_t				companion_role = 0;
static struct vigil_config		*companion_cfg = NULL;

/* The companion configuration the platform gave us, and its serial. */
static size_t				companion_len = 0;
static u_int32_t			companion_serial = 0;
static u_int32_t			companion_loaded = 0;
static char				companion_text[VIGIL_CONFIG_MAX];

/* The cathedral of our companion and when we may resolve it again. */
static int				companion_resolved = 0;
static u_int64_t			companion_resolve = 0;
static struct sockaddr_in		companion_addr;

/* The ref of our last parcel for our companion. */
static u_int64_t			companion_refs = 0;

/* Set once we sent our desktop who we are, with the nickname we sent. */
static int				companion_told = 0;
static char				companion_nick[VIGIL_CHAT_NICK_MAX + 1];

/* The serial of the avatar we last sent our desktop. */
static u_int32_t			companion_avatar = 0;

/* The ref of the last blob our platform handed us for our companion. */
static volatile u_int64_t		companion_blobs = 0;

/*
 * The journal of what we hold for our desktop as a phone, or the seq up to
 * which we stored what our phone sent us as a desktop. As a phone we sent
 * up to journal_sent and send nothing until we heard from our desktop
 * since our tunnel to it came up.
 */
static struct vigil_journal		journal;
static u_int64_t			journal_sent = 0;
static int				journal_heard = 0;

/*
 * As a desktop, the sends and asks we hold for our phone until it acks
 * them. We sent up to requests_sent, and tell our platform about the sends
 * we held when we started, up to requests_held.
 */
static struct vigil_journal		requests;
static u_int64_t			requests_sent = 0;
static u_int64_t			requests_told = 0;
static u_int64_t			requests_held = 0;

/*
 * As a desktop, the seq and ref of the request we sent our phone last, and
 * that seq once our phone sent us the record of what it did with it.
 */
static u_int64_t			requests_head = 0;
static u_int64_t			requests_ref = 0;
static u_int64_t			requests_done = 0;

/*
 * As a phone, the seqs of the last reset and synced marks we wrote, if we
 * lost an entry for our desktop and if we asked our platform for a resync.
 */
static int				companion_dirty = 0;
static int				companion_asked = 0;
static u_int64_t			companion_reset = 0;
static u_int64_t			companion_synced = 0;

/* As a desktop, the seq of the last entry in the journal of our phone. */
static u_int64_t			companion_last = 0;

/* The sync version of our companion and if we heard it yet. */
static u_int16_t			companion_other = 0;
static int				companion_known = 0;

/*
 * As a phone, the bytes in our journal, the bytes our platform handed us
 * that are not in our journal yet, the most bytes our journal may hold and
 * when we check the free space on our disk again.
 */
static volatile u_int64_t		companion_journal = 0;
static volatile u_int64_t		companion_queued = 0;
static volatile u_int64_t		companion_limit = 0;
static u_int64_t			companion_measure = 0;

/*
 * The last state of our phone and of our desktop, sent or received. As a
 * phone we also keep the last state we sent for each slot and the slot our
 * next heartbeat is for, as a desktop the last state for each flock.
 */
static struct vigil_companion_phone	companion_phone;
static struct vigil_companion_desktop	companion_desktop;
static struct vigil_companion_phone	companion_phones[VIGIL_GOSPEL_MAX];
static u_int8_t				companion_turn = 0;

/* Bumped when a Gospel configuration comes or goes, and what we told. */
static u_int64_t			attached = 0;
static u_int64_t			companion_attached = 0;

/*
 * As a desktop, the flock of the chat our platform has open or types in
 * and which peers in it, for our phone.
 */
static u_int64_t			desktop_flock = 0;
static u_int8_t				desktop_opened[VIGIL_CHAT_GROUP_ROSTER];
static u_int8_t				desktop_typing[VIGIL_CHAT_GROUP_ROSTER];

/*
 * Setup the chat part of gospel before the first Gospel configuration is
 * attached, or only our companion tunnel on a desktop that runs without
 * gospel.
 */
void
vigil_chat_init(void)
{
	PRECOND(running == 0);

	memset(&companion, 0, sizeof(companion));
	TAILQ_INIT(&companion.outbox);

	companion.slot = VIGIL_GOSPEL_MAX;
	companion_role = 0;
	companion_cfg = NULL;
	companion_loaded = 0;

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

	session++;
	running = 1;

	vigil_atomic_write(&accepting, 1);
	vigil_log(LOG_INFO, "chats running");
}

/*
 * Set up the chats of the Gospel configuration in the given slot.
 */
void
vigil_chat_attach(u_int8_t slot)
{
	int			idx;
	struct chat_gospel	*gospel;
	struct vigil_config	*cfg;

	PRECOND(slot < VIGIL_GOSPEL_MAX);
	PRECOND(vigil->gospels[slot] != NULL);

	if (!running)
		return;

	cfg = vigil->gospels[slot];
	gospel = &gospels[slot];

	VERIFY(gospel->running == 0);

	memset(gospel, 0, sizeof(*gospel));

	gospel->local = cfg->spi & 0xff;
	gospel->flock = cfg->cathedral_flock;

	for (idx = 0; idx < KYRKA_PEERS_PER_FLOCK; idx++) {
		TAILQ_INIT(&gospel->peers[idx].outbox);
		gospel->peers[idx].id = idx;
		gospel->peers[idx].slot = slot;
		gospel->peers[idx].flock = gospel->flock;

		vigil_atomic_write(&marks[slot].opened[idx], 0);
		vigil_atomic_write(&marks[slot].ignored[idx], 0);
		vigil_atomic_write(&marks[slot].accepted[idx], 0);
		vigil_atomic_write(&marks[slot].typing[idx], 0);
	}

	if (cfg->keys & VIGIL_CONFIG_KEY_NAME_SIGNATURE) {
		if (vigil_config_name_verify(cfg, gospel->local,
		    cfg->instance, cfg->name_signature,
		    sizeof(cfg->name_signature)) == 0) {
			gospel->named = 1;
		} else {
			vigil_log(LOG_NOTICE, "[chat] our instance in %"
			    PRIx64 " does not match its signature",
			    gospel->flock);
		}
	}

	if (chat_liturgy_init(&gospel->signaling, slot,
	    VIGIL_CHAT_SIGNAL_GROUP) == -1) {
		vigil_log(LOG_NOTICE, "chats are not available for %" PRIx64,
		    gospel->flock);
		return;
	}

	gospel->session = ++session;
	gospel->running = 1;

	attached++;

	vigil_log(LOG_INFO, "chats running for %" PRIx64, gospel->flock);
}

/*
 * Tear down the chats of the Gospel configuration in the given slot.
 */
void
vigil_chat_detach(u_int8_t slot)
{
	int			idx;
	struct chat_gospel	*gospel;

	PRECOND(slot < VIGIL_GOSPEL_MAX);

	gospel = &gospels[slot];

	if (!gospel->running)
		return;

	for (idx = 0; idx < KYRKA_PEERS_PER_FLOCK; idx++) {
		if (gospel->peers[idx].tunnel != NULL)
			chat_tunnel_free(gospel->peers[idx].tunnel);
		chat_outbox_free(&gospel->peers[idx]);
	}

	chat_liturgy_cleanup(&gospel->signaling);

	if (typing_flock == gospel->flock)
		vigil_chat_group_typing(0, 0, 0, 0, NULL);

	gospel->running = 0;
	attached++;
}

/*
 * Cleanup any resources allocated by the chat.
 */
void
vigil_chat_cleanup(void)
{
	u_int8_t	slot;

	if (!running)
		return;

	vigil_atomic_write(&accepting, 0);

	for (slot = 0; slot < VIGIL_GOSPEL_MAX; slot++)
		vigil_chat_detach(slot);

	if (portrait != NULL) {
		chat_blob_release(portrait);
		portrait = NULL;
		portrait_hash = 0;
	}

	chat_companion_free();
	vigil_safety_cleanup();

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
	u_int8_t	slot;

	if (!running)
		return;

	chat_companion_load();
	chat_commands_run();
	chat_avatar_update();
	chat_parcels_expire();
	chat_companion_check();

	if (vigil->config->mode == VIGIL_MODE_COMPANION)
		return;

	vigil_safety_update();

	for (slot = 0; slot < VIGIL_GOSPEL_MAX; slot++) {
		if (gospels[slot].running)
			chat_update(&gospels[slot]);
	}
}

/*
 * Manage the peers and the signaling of the given Gospel configuration.
 */
static void
chat_update(struct chat_gospel *gospel)
{
	int			idx;
	struct chat_peer	*peer;

	PRECOND(gospel != NULL);
	PRECOND(gospel->running);

	chat_liturgy_cathedral_check(&gospel->signaling);

	for (idx = 1; idx < KYRKA_PEERS_PER_FLOCK; idx++) {
		if (idx == gospel->local)
			continue;

		peer = &gospel->peers[idx];

		if (peer->present &&
		    (vigil->now - peer->pushed) >= VIGIL_CALL_PRESENT_EXPIRE) {
			vigil_log(LOG_INFO, "[chat] %02x is gone", idx);
			peer->asked = 0;
			peer->present = 0;
		}

		chat_ask_check(peer);
		chat_tunnel_check(peer);
	}

	chat_liturgy_signal(gospel);
}

/*
 * Rebind everything, network shifted below us.
 */
void
vigil_chat_rebind(void)
{
	int			idx;
	u_int8_t		slot;
	struct chat_gospel	*gospel;

	if (!running)
		return;

	vigil_safety_rebind();

	for (slot = 0; slot < VIGIL_GOSPEL_MAX; slot++) {
		gospel = &gospels[slot];

		if (!gospel->running)
			continue;

		chat_liturgy_rebind(&gospel->signaling);

		for (idx = 0; idx < KYRKA_PEERS_PER_FLOCK; idx++) {
			if (gospel->peers[idx].tunnel != NULL)
				chat_tunnel_rebind(gospel->peers[idx].tunnel);
		}
	}

	if (companion.tunnel != NULL)
		chat_tunnel_rebind(companion.tunnel);
}

/*
 * Notify all of our cathedrals.
 */
void
vigil_chat_poke(void)
{
	int			idx;
	u_int8_t		slot;
	struct chat_gospel	*gospel;

	if (!running)
		return;

	for (slot = 0; slot < VIGIL_GOSPEL_MAX; slot++) {
		gospel = &gospels[slot];

		if (!gospel->running)
			continue;

		gospel->signaling.notify = 0;

		for (idx = 0; idx < KYRKA_PEERS_PER_FLOCK; idx++) {
			if (gospel->peers[idx].tunnel != NULL)
				chat_tunnel_poke(gospel->peers[idx].tunnel);
		}
	}

	if (companion.tunnel != NULL)
		chat_tunnel_poke(companion.tunnel);
}

/*
 * Queue a message for the given peer to be sent.
 */
int
vigil_chat_send(u_int64_t flock, u_int8_t peer, u_int64_t ref, const char *text,
    size_t len)
{
	int		ret;

	PRECOND(peer > 0 && peer < KYRKA_PEERS_PER_FLOCK);
	PRECOND(text != NULL);
	PRECOND(len > 0 && len <= VIGIL_CHAT_MESSAGE_MAX);

	ret = chat_command_queue(CHAT_COMMAND_SEND,
	    flock, peer, ref, NULL, NULL, NULL, text, len);

	return (ret);
}

/*
 * Queue a reply for the given peer to be sent, author is who wrote the
 * message we reply to (0 for us) and hash is the hash of that message.
 */
int
vigil_chat_reply(u_int64_t flock, u_int8_t peer, u_int64_t ref,
    u_int8_t author, u_int64_t hash, const char *text, size_t len)
{
	int			ret;
	struct chat_quote	quote;

	PRECOND(peer > 0 && peer < KYRKA_PEERS_PER_FLOCK);
	PRECOND(hash != 0);
	PRECOND(text != NULL);
	PRECOND(len > 0 && len <= VIGIL_CHAT_REPLY_MAX);

	memset(&quote, 0, sizeof(quote));

	quote.hash = hash;
	quote.author = author;

	ret = chat_command_queue(CHAT_COMMAND_SEND,
	    flock, peer, ref, &quote, NULL, NULL, text, len);

	return (ret);
}

/*
 * Queue the given topic for the given peer, no name removes the topic.
 */
int
vigil_chat_topic(u_int64_t flock, u_int8_t peer, u_int64_t ref, u_int64_t id,
    u_int32_t color, const char *name, size_t len)
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
	    flock, peer, ref, NULL, &topic, NULL, name, len);

	return (ret);
}

/*
 * Queue a message in the given topic for the given peer, author and hash
 * are as in vigil_chat_reply() and a hash of 0 means it replies to nothing.
 */
int
vigil_chat_topic_send(u_int64_t flock, u_int8_t peer, u_int64_t ref,
    u_int64_t id, u_int8_t author, u_int64_t hash, const char *text,
    size_t len)
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
	    flock, peer, ref, &quote, &topic, NULL, text, len);

	return (ret);
}

/*
 * Queue the roster of our group with the given id for the given peer, a bit
 * in members for every member. A peer that is not in it is no longer.
 */
int
vigil_chat_group_roster(u_int64_t flock, u_int8_t peer, u_int64_t ref,
    u_int64_t id, u_int32_t serial, int state, const u_int8_t *members,
    const char *name, size_t len)
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
	group.type = VIGIL_CHAT_TYPE_ROSTER;
	memcpy(group.members, members, sizeof(group.members));

	ret = chat_command_queue(CHAT_COMMAND_SEND,
	    flock, peer, ref, NULL, NULL, &group, name, len);

	return (ret);
}

/*
 * Queue a leave for the group with the given id that the given peer owns.
 */
int
vigil_chat_group_leave(u_int64_t flock, u_int8_t owner, u_int64_t ref,
    u_int64_t id)
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
	    flock, owner, ref, NULL, NULL, &group, NULL, 0);

	return (ret);
}

/*
 * Queue a message in the given group for the given peer, in the given topic
 * if it is not 0, serial is that of the roster we know. Author and hash are
 * as in vigil_chat_reply() and a hash of 0 means it replies to nothing.
 */
int
vigil_chat_group_send(u_int64_t flock, u_int8_t peer, u_int64_t ref,
    u_int8_t owner, u_int64_t id, u_int32_t serial, u_int64_t tid,
    u_int8_t author, u_int64_t hash, const char *text, size_t len)
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
	    flock, peer, ref, &quote, &topic, &group, text, len);

	return (ret);
}

/*
 * Queue the given topic in the group with the given id that we own for
 * the given peer, no name removes the topic. Serial is that of our roster.
 */
int
vigil_chat_group_topic(u_int64_t flock, u_int8_t peer, u_int64_t ref,
    u_int64_t id, u_int32_t serial, u_int64_t tid, u_int32_t color,
    const char *name, size_t len)
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

	topic.id = tid;
	topic.set = 1;
	topic.color = color & 0xffffff;

	ret = chat_command_queue(CHAT_COMMAND_SEND,
	    flock, peer, ref, NULL, &topic, &group, name, len);

	return (ret);
}

/*
 * Queue a retract for the data with the given hash for the given peer.
 */
int
vigil_chat_retract(u_int64_t flock, u_int8_t peer, u_int64_t ref,
    u_int64_t tid, u_int64_t hash)
{
	int			ret;
	struct chat_quote	quote;
	struct chat_topic	topic;

	PRECOND(peer > 0 && peer < KYRKA_PEERS_PER_FLOCK);
	PRECOND(hash != 0);

	memset(&quote, 0, sizeof(quote));
	memset(&topic, 0, sizeof(topic));

	topic.id = tid;
	quote.hash = hash;
	quote.retract = 1;

	ret = chat_command_queue(CHAT_COMMAND_SEND,
	    flock, peer, ref, &quote, &topic, NULL, NULL, 0);

	return (ret);
}

/*
 * Queue a retract for the data with the given hash in the given group.
 */
int
vigil_chat_group_retract(u_int64_t flock, u_int8_t peer, u_int64_t ref,
    u_int8_t owner, u_int64_t id, u_int32_t serial, u_int64_t tid,
    u_int64_t hash)
{
	int			ret;
	struct chat_quote	quote;
	struct chat_group	group;
	struct chat_topic	topic;

	PRECOND(peer > 0 && peer < KYRKA_PEERS_PER_FLOCK);
	PRECOND(owner > 0 && owner < KYRKA_PEERS_PER_FLOCK);
	PRECOND(id != 0);
	PRECOND(hash != 0);

	memset(&quote, 0, sizeof(quote));
	memset(&group, 0, sizeof(group));
	memset(&topic, 0, sizeof(topic));

	topic.id = tid;
	quote.hash = hash;
	quote.retract = 1;

	group.id = id;
	group.owner = owner;
	group.serial = serial;
	group.type = VIGIL_CHAT_TYPE_TEXT;

	ret = chat_command_queue(CHAT_COMMAND_SEND,
	    flock, peer, ref, &quote, &topic, &group, NULL, 0);

	return (ret);
}

/*
 * Tell the owner and the given members of the given group in the given
 * flock we are typing in it and the given topic, an id of 0 means we are
 * not typing in any group and a topic of 0 in none of its topics.
 */
void
vigil_chat_group_typing(u_int64_t flock, u_int8_t owner, u_int64_t id,
    u_int64_t topic, const u_int8_t *members)
{
	PRECOND(id == 0 || (owner > 0 && owner < KYRKA_PEERS_PER_FLOCK));
	PRECOND(id == 0 || members != NULL);

	if (id == 0) {
		flock = 0;
		owner = 0;
		topic = 0;
	}

	vigil_lock(&lock);

	typing_owner = owner;
	typing_flock = flock;
	typing_group = id;
	typing_topic = topic;

	if (id == 0) {
		memset(typing_members, 0, sizeof(typing_members));
	} else {
		memcpy(typing_members, members, sizeof(typing_members));
	}

	vigil_unlock(&lock);
	vigil_wake_signal();
}

/*
 * Send data to our peer.
 */
int
vigil_chat_data_send(u_int64_t flock, u_int8_t peer, u_int64_t ref, int kind,
    const char *name, const void *data, size_t len)
{
	int		ret;

	PRECOND(peer > 0 && peer < KYRKA_PEERS_PER_FLOCK);
	PRECOND(name != NULL);
	PRECOND(data != NULL);
	PRECOND(len > 0 && len <= VIGIL_CHAT_DATA_MAX);

	ret = chat_data_queue(flock, peer, ref, NULL, 0, kind, name, data, len);

	return (ret);
}

/*
 * Send data to our peer in the given group, in the given topic if it is
 * not 0, serial is that of the roster we know.
 */
int
vigil_chat_group_data_send(u_int64_t flock, u_int8_t peer, u_int64_t ref,
    u_int8_t owner, u_int64_t id, u_int32_t serial, u_int64_t topic, int kind,
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

	ret = chat_data_queue(flock,
	    peer, ref, &group, topic, kind, name, data, len);

	return (ret);
}

/*
 * Send data to our peer in the given topic.
 */
int
vigil_chat_topic_data_send(u_int64_t flock, u_int8_t peer, u_int64_t ref,
    u_int64_t id, int kind, const char *name, const void *data, size_t len)
{
	int		ret;

	PRECOND(peer > 0 && peer < KYRKA_PEERS_PER_FLOCK);
	PRECOND(id != 0);
	PRECOND(name != NULL);
	PRECOND(data != NULL);
	PRECOND(len > 0 && len <= VIGIL_CHAT_DATA_MAX);

	ret = chat_data_queue(flock,
	    peer, ref, NULL, id, kind, name, data, len);

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
vigil_chat_forget(u_int64_t flock, u_int8_t peer)
{
	int		ret;

	PRECOND(peer > 0 && peer < KYRKA_PEERS_PER_FLOCK);

	if (!vigil_atomic_read(&accepting))
		return (0);

	ret = chat_command_queue(CHAT_COMMAND_FORGET,
	    flock, peer, 0, NULL, NULL, NULL, NULL, 0);

	return (ret);
}

/*
 * Tell the platform the safety code exchange with the given peer in the
 * given slot gave us its input, the platform obtains the input with
 * vigil_safety_get().
 */
void
vigil_chat_safety(u_int8_t slot, u_int8_t peer)
{
	struct chat_peer	*who;

	PRECOND(slot < VIGIL_GOSPEL_MAX);
	PRECOND(peer > 0 && peer < KYRKA_PEERS_PER_FLOCK);

	if (!gospels[slot].running)
		return;

	who = &gospels[slot].peers[peer];

	if (chat_event_queue(CHAT_EVENT_SAFETY, who, 0, NULL, 0) == -1) {
		vigil_log(LOG_NOTICE, "[chat] %02x no room to tell our "
		    "platform about the safety code", peer);
	}
}

/*
 * Our additional kdf input for the given peer in the given flock changed,
 * tear down our tunnel with it.
 */
void
vigil_chat_additional_input(u_int64_t flock, u_int8_t peer)
{
	int		ret;

	PRECOND(peer > 0 && peer < KYRKA_PEERS_PER_FLOCK);

	if (!vigil_atomic_read(&accepting))
		return;

	ret = chat_command_queue(CHAT_COMMAND_INPUT,
	    flock, peer, 0, NULL, NULL, NULL, NULL, 0);
	if (ret == -1) {
		vigil_log(LOG_NOTICE,
		    "[chat] %02x no room, tunnel keeps the old input", peer);
	}
}

/*
 * Drop the message with the given ref we hold for the given peer.
 */
int
vigil_chat_drop(u_int64_t flock, u_int8_t peer, u_int64_t ref)
{
	int		ret;

	PRECOND(peer > 0 && peer < KYRKA_PEERS_PER_FLOCK);

	if (!vigil_atomic_read(&accepting))
		return (0);

	ret = chat_command_queue(CHAT_COMMAND_DROP,
	    flock, peer, ref, NULL, NULL, NULL, NULL, 0);

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
	    0, 0, token, NULL, NULL, NULL, NULL, 0);

	return (ret);
}

/*
 * Mark the chat with the given peer in the given flock as open or closed,
 * we keep signaling the peer while it is open.
 */
void
vigil_chat_open(u_int64_t flock, u_int8_t peer, int on)
{
	u_int8_t	slot;

	PRECOND(peer > 0 && peer < KYRKA_PEERS_PER_FLOCK);
	PRECOND(on == 0 || on == 1);

	if (vigil_gospel_slot(flock, &slot) == -1) {
		chat_desktop_bit(desktop_opened, flock, peer, on);
		vigil_wake_signal();
		return;
	}

	vigil_atomic_write(&marks[slot].opened[peer], on);
	vigil_gospel_release();

	vigil_wake_signal();
}

/*
 * Accept the given peer in the given flock or not, we ignore a peer we did
 * not accept.
 */
void
vigil_chat_accept(u_int64_t flock, u_int8_t peer, int on)
{
	u_int8_t	slot;

	PRECOND(peer > 0 && peer < KYRKA_PEERS_PER_FLOCK);
	PRECOND(on == 0 || on == 1);

	if (vigil_gospel_slot(flock, &slot) == -1)
		return;

	vigil_atomic_write(&marks[slot].accepted[peer], on);
	vigil_gospel_release();

	vigil_wake_signal();
}

/*
 * Ignore the given peer in the given flock or not. We never talk to a peer
 * we ignore and never ask our platform about it, even if we accepted it.
 */
void
vigil_chat_ignore(u_int64_t flock, u_int8_t peer, int on)
{
	u_int8_t	slot;

	PRECOND(peer > 0 && peer < KYRKA_PEERS_PER_FLOCK);
	PRECOND(on == 0 || on == 1);

	if (vigil_gospel_slot(flock, &slot) == -1)
		return;

	vigil_atomic_write(&marks[slot].ignored[peer], on);
	vigil_gospel_release();

	vigil_wake_signal();
}

/*
 * Returns 1 if we accepted the given peer in the given slot, may be called
 * from any thread.
 */
int
vigil_chat_accepted(u_int8_t slot, u_int8_t peer)
{
	PRECOND(slot < VIGIL_GOSPEL_MAX);
	PRECOND(peer > 0 && peer < KYRKA_PEERS_PER_FLOCK);

	if (vigil_atomic_read(&marks[slot].ignored[peer]))
		return (0);

	return (vigil_atomic_read(&marks[slot].accepted[peer]));
}

/*
 * Tell the given peer in the given flock if we are typing, in the given
 * topic if it is not 0.
 */
void
vigil_chat_typing(u_int64_t flock, u_int8_t peer, u_int64_t topic, int on)
{
	u_int8_t	slot;

	PRECOND(peer > 0 && peer < KYRKA_PEERS_PER_FLOCK);
	PRECOND(on == 0 || on == 1);

	if (on == 0)
		topic = 0;

	if (vigil_gospel_slot(flock, &slot) == -1) {
		vigil_lock(&lock);
		typing_topic = topic;
		vigil_unlock(&lock);

		chat_desktop_bit(desktop_typing, flock, peer, on);
		vigil_wake_signal();
		return;
	}

	vigil_lock(&lock);
	typing_topic = topic;
	vigil_unlock(&lock);

	vigil_atomic_write(&marks[slot].typing[peer], on);
	vigil_gospel_release();

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
 * As the desktop of a phone, have our phone dial, answer, decline or hang
 * up a call with the given peer.
 */
int
vigil_chat_call(int op, u_int64_t flock, u_int8_t peer)
{
	int		ret;

	PRECOND(op == VIGIL_COMPANION_DIAL ||
	    op == VIGIL_COMPANION_ANSWER ||
	    op == VIGIL_COMPANION_DECLINE ||
	    op == VIGIL_COMPANION_HANGUP);
	PRECOND(peer > 0 && peer < KYRKA_PEERS_PER_FLOCK);

	ret = chat_command_queue(CHAT_COMMAND_CALL,
	    flock, peer, op, NULL, NULL, NULL, NULL, 0);

	return (ret);
}

/*
 * As a phone, send the given JSON from our platform to our desktop with
 * the given data, if any. Returns -1 if we are not a phone or our journal
 * has no room for it.
 */
int
vigil_chat_companion_sync(const char *json, size_t len, const void *data,
    size_t size)
{
	int		ret;
	size_t		need;

	PRECOND(json != NULL);
	PRECOND(len > 0 && len <= VIGIL_COMPANION_JSON_MAX);
	PRECOND(data != NULL || size == 0);
	PRECOND(size <= VIGIL_CHAT_DATA_MAX);

	if (companion_role != VIGIL_COMPANION_PHONE)
		return (-1);

	need = chat_phone_need(len, size);

	if (chat_phone_reserve(need) == -1)
		return (-1);

	ret = chat_command_blob(CHAT_COMMAND_SYNC,
	    CHAT_SYNC_ENTRY, json, len, data, size);

	return (ret);
}

/*
 * As a phone, mark the start of a full copy of our chats for our desktop,
 * or its end when done is 1. Returns -1 if we are not a phone.
 */
int
vigil_chat_companion_resync(int done)
{
	int		ret;

	PRECOND(done == 0 || done == 1);

	if (companion_role != VIGIL_COMPANION_PHONE)
		return (-1);

	ret = chat_command_queue(CHAT_COMMAND_SYNC, 0, 0,
	    done ? CHAT_SYNC_DONE : CHAT_SYNC_RESET, NULL, NULL, NULL, NULL, 0);

	return (ret);
}

/*
 * As a desktop, forget that we are synced so our phone sends us a full
 * resync. Returns -1 if we are not a desktop.
 */
int
vigil_chat_companion_unsync(void)
{
	int		ret;

	if (companion_role != VIGIL_COMPANION_DESKTOP)
		return (-1);

	ret = chat_command_queue(CHAT_COMMAND_SYNC, 0, 0, CHAT_SYNC_RESET,
	    NULL, NULL, NULL, NULL, 0);

	return (ret);
}

/*
 * As the desktop of a phone, ask the platform of our phone to do what the
 * given JSON says with the given data, if any. The ref is the id of the
 * ask, our phone runs an ask with a ref only once.
 */
int
vigil_chat_companion_ask(u_int64_t ref, const char *json, size_t len,
    const void *data, size_t size)
{
	int				ret;
	struct vigil_chat_request	req;
	u_int8_t			*body;

	PRECOND(ref & VIGIL_CHAT_COMPANION_REF);
	PRECOND(json != NULL);
	PRECOND(len > 0 && len <= VIGIL_CHAT_MESSAGE_MAX);
	PRECOND(data != NULL || size == 0);
	PRECOND(size <= VIGIL_CHAT_DATA_MAX);

	memset(&req, 0, sizeof(req));

	req.ref = ref;
	req.ask = len;
	req.op = VIGIL_COMPANION_ASK;

	if ((body = malloc(len + size)) == NULL)
		fatal("malloc: failed to allocate ask");

	memcpy(body, json, len);

	if (size > 0)
		memcpy(body + len, data, size);

	ret = chat_desktop_queue(&req, body, len + size);

	nyfe_mem_zero(body, len + size);
	free(body);

	return (ret);
}

/*
 * As the desktop of a phone, have our phone send the text or data as the
 * given request in host byte order says, to a peer or to every member the
 * request names in one go.
 */
int
vigil_chat_companion_send(const struct vigil_chat_request *req,
    const void *data, size_t len)
{
	int		ret;

	PRECOND(req != NULL);
	PRECOND(req->op == VIGIL_COMPANION_SEND);
	PRECOND(req->ref & VIGIL_CHAT_COMPANION_REF);
	PRECOND(req->len <= sizeof(req->name));
	PRECOND(data != NULL);
	PRECOND(len > 0 && len <= VIGIL_CHAT_DATA_MAX);

	ret = chat_desktop_queue(req, data, len);

	return (ret);
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
	chat_events_take(&taken, len - 2);

	vigil_buf_appendf(&out, "[");

	while ((evt = TAILQ_FIRST(&taken)) != NULL) {
		TAILQ_REMOVE(&taken, evt, list);

		chat_event_render(&out, evt, first);
		first = 0;

		chat_event_free(evt);
	}

	vigil_buf_appendf(&out, "]");

	if (out.flags & VIGIL_BUF_FULL)
		return (0);

	return (out.offset);
}

/*
 * Append the state of our chats to the status, this includes the chats of
 * each Gospel configuration, our peers and our tunnels.
 */
void
vigil_chat_status(struct vigil_buf *buf)
{
	int			idx;
	u_int8_t		slot;
	struct chat_tunnel	*tun;
	struct chat_msg		*msg;
	struct chat_peer	*peer;
	const char		*link;
	struct chat_gospel	*gospel;
	struct vigil_config	*cfg;
	size_t			queued;
	int			first, online, p2p;
	char			name[VIGIL_CHAT_NICK_MAX + 1];

	PRECOND(buf != NULL);

	if (!running)
		return;

	vigil_buf_appendf(buf, ",\"chat\":{\"session\":%" PRIu64, session);

	if (vigil->config->mode == VIGIL_MODE_COMPANION) {
		chat_companion_status(buf);
		vigil_buf_appendf(buf, "}");
		return;
	}

	vigil_lock(&lock);
	memcpy(name, nick, sizeof(name));
	vigil_unlock(&lock);

	vigil_buf_appendf(buf, ",\"nick\":");
	vigil_buf_quote(buf, name);
	vigil_safety_status(buf);

	first = 1;
	vigil_buf_appendf(buf, ",\"gospels\":[");

	for (slot = 0; slot < VIGIL_GOSPEL_MAX; slot++) {
		gospel = &gospels[slot];
		cfg = vigil->gospels[slot];

		if (!gospel->running)
			continue;

		vigil_buf_appendf(buf,
		    "%s{\"flock\":\"%" PRIx64 "\",\"session\":%" PRIu64
		    ",\"alive\":%s,\"id\":\"%" PRIx64 "-%02x\",\"signed\":",
		    first ? "" : ",", gospel->flock, gospel->session,
		    gospel->signaling.last != 0 ? "true" : "false",
		    gospel->flock, gospel->local);
		vigil_buf_quote(buf, gospel->named ? cfg->instance : "");

		vigil_buf_appendf(buf, ",\"liturgies\":[");
		vigil_status_liturgy(buf, "chat", gospel->signaling.group,
		    &gospel->signaling.cathedral, gospel->signaling.rx,
		    gospel->signaling.tx);
		vigil_buf_appendf(buf, "]}");

		first = 0;
	}

	first = 1;
	vigil_buf_appendf(buf, "],\"peers\":[");

	for (slot = 0; slot < VIGIL_GOSPEL_MAX; slot++) {
		gospel = &gospels[slot];

		if (!gospel->running)
			continue;

		for (idx = 1; idx < KYRKA_PEERS_PER_FLOCK; idx++) {
			peer = &gospel->peers[idx];
			tun = peer->tunnel;

			queued = 0;
			TAILQ_FOREACH(msg, &peer->outbox, wlist)
				queued++;

			if (peer->present == 0 && tun == NULL && queued == 0)
				continue;

			chat_tunnel_state(tun, &link, &online, &p2p);
			vigil_buf_appendf(buf,
			    "%s{\"id\":\"%" PRIx64 "-%02x\",\"present\":%s"
			    ",\"online\":%s", first ? "" : ",", gospel->flock,
			    idx, peer->present ? "true" : "false",
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
				vigil_buf_appendf(buf,
				    ",\"typing_owner\":\"%02x\""
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
	}

	vigil_buf_appendf(buf, "]");
	chat_companion_status(buf);
	vigil_buf_appendf(buf, "}");
}

/*
 * Setup a liturgy for the Gospel configuration in the given slot.
 */
static int
chat_liturgy_init(struct chat_liturgy *lit, u_int8_t slot, u_int16_t id)
{
	struct kyrka_cathedral_cfg	cfg;
	struct vigil_config		*gospel;

	PRECOND(lit != NULL);
	PRECOND(lit->ctx == NULL);
	PRECOND(slot < VIGIL_GOSPEL_MAX);
	PRECOND(vigil->gospels[slot] != NULL);

	gospel = vigil->gospels[slot];
	memset(lit, 0, sizeof(*lit));

	lit->slot = slot;

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

	vigil_kyrka_cathedral(lit->ctx, &cfg, gospel, 0, gospel->mtu);

	return (0);
}

/*
 * Let the cathedral of the given Gospel configuration know which peers we
 * are interested in.
 */
static void
chat_liturgy_signal(struct chat_gospel *gospel)
{
	int			idx;
	struct chat_liturgy	*lit;
	u_int8_t		want[KYRKA_PEERS_PER_FLOCK];

	PRECOND(gospel != NULL);

	lit = &gospel->signaling;

	if (lit->cathedral.addr.sin_port == 0)
		return;

	for (idx = 0; idx < KYRKA_PEERS_PER_FLOCK; idx++)
		want[idx] = chat_signal_check(&gospel->peers[idx]);

	if (lit->notify != 0 && vigil->now < lit->notify) {
		if (!memcmp(want, gospel->sent, sizeof(want)))
			return;
		if ((vigil->now - lit->updated) < VIGIL_CALL_NOTIFY_MIN)
			return;
	}

	lit->updated = vigil->now;
	lit->notify = vigil->now + VIGIL_CALL_NOTIFY;
	memcpy(gospel->sent, want, sizeof(gospel->sent));

	if (kyrka_cathedral_liturgy(lit->ctx, want, sizeof(want)) == -1) {
		vigil_log(LOG_NOTICE, "[chat] kyrka_cathedral_liturgy: %d",
		    kyrka_last_error(lit->ctx));
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
		if (vigil_call_cathedral_pick(lit->slot, &lit->cathedral) == -1)
			return;

		if (lit->group == VIGIL_CHAT_SIGNAL_GROUP)
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

	if (lit->group == VIGIL_CHAT_SIGNAL_GROUP) {
		for (idx = 0; idx < KYRKA_PEERS_PER_FLOCK; idx++)
			gospels[lit->slot].peers[idx].present = 0;
	}

	if (vigil_call_cathedral_pick(lit->slot, &lit->cathedral) == -1) {
		lit->cathedral.last = vigil->now;
		return;
	}

	if (lit->group == VIGIL_CHAT_SIGNAL_GROUP)
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
	struct chat_peer	*peer;
	struct chat_gospel	*gospel;

	PRECOND(ctx != NULL);
	PRECOND(evt != NULL);
	PRECOND(udata != NULL);

	lit = udata;
	gospel = &gospels[lit->slot];

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
			if (idx == gospel->local)
				continue;

			/*
			 * The cathedral pushes us each peer that signals us
			 * on its own, peers expire in chat_update().
			 */
			if (evt->liturgy.peers[idx] == 0)
				continue;

			peer = &gospel->peers[idx];

			if (peer->present == 0) {
				vigil_log(LOG_INFO,
				    "[chat] %02x is around", idx);
			}

			peer->present = 1;
			peer->pushed = vigil->now;
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

	if (!vigil_chat_accepted(peer->slot, peer->id))
		want = 0;

	if (want == 0) {
		if (tun != NULL)
			chat_tunnel_free(tun);
		return;
	}

	if (tun == NULL) {
		memset(&cat, 0, sizeof(cat));

		if (vigil_call_cathedral_pick(peer->slot, &cat) == -1)
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
	struct vigil_config		*gospel;

	PRECOND(peer != NULL);
	PRECOND(cathedral != NULL);
	PRECOND(peer->tunnel == NULL);

	if (peer == &companion)
		gospel = companion_cfg;
	else
		gospel = vigil->gospels[peer->slot];

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
	tun->cfg = gospel;
	tun->io.handle = chat_tunnel_read;

	memcpy(&tun->cathedral, cathedral, sizeof(*cathedral));
	memcpy(&tun->addr, &cathedral->addr, sizeof(cathedral->addr));

	tun->ctx = kyrka_ctx_alloc(chat_tunnel_event, tun);
	if (tun->ctx == NULL)
		fatal("failed to create chat context");

	if (peer != &companion) {
		tun->safety = vigil_kyrka_additional_input(tun->ctx,
		    peer->flock, peer->id);
	}

	nyfe_random_bytes(&tun->local_uid, sizeof(tun->local_uid));

	memset(&cfg, 0, sizeof(cfg));

	cfg.udata = tun;
	cfg.send = chat_cathedral_send;
	cfg.identity = gospel->cathedral_id;

	if (peer == &companion) {
		cfg.tunnel = gospel->spi;
		cfg.flock_src = gospel->cathedral_flock;
	} else {
		cfg.tunnel = ((gospel->spi & 0xff) << 8) | peer->id;
		cfg.flock_src = gospel->cathedral_flock |
		    VIGIL_CHAT_FLOCK_DOMAIN;
	}

	cfg.flock_dst = cfg.flock_src;

	if (gospel->flags & VIGIL_CONFIG_FLAG_COMMIXTION)
		cfg.hops = VIGIL_COMMIXTION_HOPS;

	vigil_kyrka_cathedral(tun->ctx, &cfg, gospel, 1, gospel->mtu);

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

	if (tun->peer == &companion) {
		tun->cathedral.last = vigil->now;
		tun->cathedral.timeout = VIGIL_CATHEDRAL_TIMEOUT_INIT;
		tun->next_notify = 0;
		return;
	}

	if (vigil_call_cathedral_pick(tun->peer->slot,
	    &tun->cathedral) == -1) {
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

		if ((tun->cfg->flags & VIGIL_CONFIG_FLAG_P2P) &&
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

	typed = chat_typed(tun->peer);
	chat_typing_state(tun->peer, &owner, &group, &topic);

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

		if (tun->safety) {
			vigil_log(LOG_INFO,
			    "[chat] %02x online with safety code",
			    tun->peer->id);
		} else {
			vigil_log(LOG_INFO,
			    "[chat] %02x online", tun->peer->id);
		}

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

		if (!(tun->cfg->flags & VIGIL_CONFIG_FLAG_P2P))
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

	if (tun->peer == &companion && pkt->length == VIGIL_VOICE_PAYLOAD) {
		if (companion_role == VIGIL_COMPANION_PHONE) {
			vigil_voice_relay_input(kyrka_packet_data(pkt),
			    pkt->length);
		} else {
			vigil_voice_relay_play(kyrka_packet_data(pkt),
			    pkt->length);
		}
		return;
	}

	if (!chat_tunnel_accepted(tun))
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

	if (tun->peer == &companion &&
	    frame->type != VIGIL_CHAT_TYPE_ACK &&
	    frame->type != VIGIL_CHAT_TYPE_HEARTBEAT &&
	    frame->type != VIGIL_CHAT_TYPE_DATA_ACK)
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
	case VIGIL_CHAT_TYPE_RETRACT:
		chat_retract_recv(tun, id, NULL, frame->data, len);
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
		sin.sin_port = htons(tun->cfg->cathedral_nat_port);

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

	if (!(tun->cfg->flags & VIGIL_CONFIG_FLAG_P2P))
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
 * and again when ours changes.
 */
static void
chat_avatar_push(struct chat_tunnel *tun)
{
	struct chat_msg		*msg;

	PRECOND(tun != NULL);

	if (tun->peer == &companion)
		return;

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
		} else if (msg->quote.retract) {
			chat_retract_frame(tun, frame, msg);
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
		    VIGIL_CHAT_FEATURE_GROUP | VIGIL_CHAT_FEATURE_RETRACT;

		if (tun->peer != &companion &&
		    gospels[tun->peer->slot].named) {
			hb->features |= VIGIL_CHAT_FEATURE_NAME;
			memcpy(hb->instance, tun->cfg->instance,
			    sizeof(hb->instance));
			memcpy(hb->signature, tun->cfg->name_signature,
			    sizeof(hb->signature));
		}

		if (portrait != NULL)
			hb->features |= VIGIL_CHAT_FEATURE_AVATAR;

		hb->avatar_have = htobe64(tun->peer->avatar_held);
		hb->typing = chat_typed(tun->peer);

		chat_typing_state(tun->peer, &tun->typed_owner,
		    &tun->typed_group, &tun->typed_topic);
		hb->typing_owner = tun->typed_owner;
		hb->typing_group = htobe64(tun->typed_group);
		hb->typing_topic = htobe64(tun->typed_topic);

		vigil_lock(&lock);
		memcpy(hb->name, nick, sizeof(hb->name));
		vigil_unlock(&lock);

		if (tun->peer == &companion)
			chat_companion_heartbeat(tun, frame);

		tun->greeted = 1;
		tun->typed = hb->typing;

		if (vigil->now < tun->holepunch) {
			tun->next_heartbeat = vigil->now +
			    CHAT_HOLEPUNCH_INTERVAL;
		} else if (tun->peer == &companion &&
		    companion_role == VIGIL_COMPANION_DESKTOP) {
			tun->next_heartbeat = UINT64_MAX;
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

	if (tun->peer == &companion && companion_role == VIGIL_COMPANION_PHONE)
		chat_phone_acked(msg->ref);

	if (tun->peer == &companion &&
	    companion_role == VIGIL_COMPANION_DESKTOP &&
	    (msg->ref & CHAT_REQUEST_REF))
		chat_desktop_acked(msg->ref & ~CHAT_REQUEST_REF);

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

	if (peer != &companion) {
		if (strcmp(name, peer->nick)) {
			vigil_log(LOG_INFO, "[chat] %02x is now known as '%s'",
			    peer->id, name);
			memcpy(peer->nick, name, sizeof(name));
			peer->reported = 0;
		}

		chat_hb_name(peer, hb);
		chat_name_report(peer);
		chat_avatar_check(peer, hb->features);
	}

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

	if (peer == &companion)
		chat_companion_hb_recv(tun, frame);

	if (tun->remote_uid == hb->uid)
		return;

	if (peer == &companion && companion_role == VIGIL_COMPANION_PHONE) {
		memset(&companion_phone, 0, sizeof(companion_phone));
		memset(companion_phones, 0, sizeof(companion_phones));
	}

	if (peer == &companion && companion_role == VIGIL_COMPANION_DESKTOP)
		memset(&companion_desktop, 0, sizeof(companion_desktop));

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

	if (vigil_config_name_verify(vigil->gospels[peer->slot], peer->id, name,
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

	if (len == 0 && chat_text_empty(quote, topic, group)) {
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
	seen->journal = 0;
	seen->token = tokens;
	seen->age = vigil->now;

	tun->peer->active = vigil->now;
}

/*
 * Returns 1 if an empty text with the given topic and group is one we drop.
 * A topic without a name was removed, a roster, leave or retract may carry
 * no text.
 */
static int
chat_text_empty(const struct chat_quote *quote,
    const struct chat_topic *topic, const struct chat_group *group)
{
	if (quote != NULL && quote->retract)
		return (0);

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

	memset(&quote, 0, sizeof(quote));

	quote.author = reply->author;
	quote.hash = be64toh(reply->hash);

	if (quote.author == (tun->cfg->spi & 0xff))
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

	if (quote.author == (tun->cfg->spi & 0xff))
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
	case VIGIL_CHAT_TYPE_RETRACT:
		group.type = VIGIL_CHAT_TYPE_TEXT;
		chat_retract_recv(tun, id, &group, wire->data, len);
		break;
	case VIGIL_CHAT_TYPE_ROSTER:
		chat_group_roster(tun, id, &group, wire->data, len);
		break;
	case VIGIL_CHAT_TYPE_LEAVE:
		if (group.owner != (tun->cfg->spi & 0xff)) {
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
	struct vigil_chat_retract	*retract;
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
		if (msg->quote.retract) {
			retract = (struct vigil_chat_retract *)wire->data;

			retract->topic = htobe64(msg->topic.id);
			retract->hash = htobe64(msg->quote.hash);

			wire->type = VIGIL_CHAT_TYPE_RETRACT;
			off = sizeof(*retract);
			break;
		}

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
 * Our peer retracted data it sent us, in the given group if any.
 */
static void
chat_retract_recv(struct chat_tunnel *tun, u_int64_t id,
    const struct chat_group *group, u_int8_t *data, u_int16_t len)
{
	struct chat_quote		quote;
	struct chat_topic		topic;
	struct vigil_chat_retract	*wire;

	PRECOND(tun != NULL);
	PRECOND(id != 0);
	PRECOND(data != NULL);
	PRECOND(len <= VIGIL_CHAT_MESSAGE_MAX);

	if (len < sizeof(*wire))
		return;

	wire = (struct vigil_chat_retract *)data;

	memset(&quote, 0, sizeof(quote));
	memset(&topic, 0, sizeof(topic));

	quote.retract = 1;
	quote.author = tun->peer->id;
	quote.hash = be64toh(wire->hash);
	topic.id = be64toh(wire->topic);

	if (quote.hash == 0) {
		chat_ack_queue(tun, id);
		return;
	}

	chat_text_recv(tun, id, &quote, &topic, group, data, 0);
}

/*
 * Turn the given frame into a retract, for peers that take them.
 */
static void
chat_retract_frame(struct chat_tunnel *tun, struct vigil_chat_frame *frame,
    const struct chat_msg *msg)
{
	struct vigil_chat_retract	*retract;

	PRECOND(tun != NULL);
	PRECOND(frame != NULL);
	PRECOND(msg != NULL);
	PRECOND(msg->quote.retract);

	memset(frame->data, 0, sizeof(frame->data));

	if (!(tun->features & VIGIL_CHAT_FEATURE_RETRACT)) {
		frame->len = 0;
		return;
	}

	retract = (struct vigil_chat_retract *)frame->data;
	retract->topic = htobe64(msg->topic.id);
	retract->hash = htobe64(msg->quote.hash);

	frame->type = VIGIL_CHAT_TYPE_RETRACT;
	frame->len = htobe16(sizeof(*retract));
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
	evt->flock = tun->peer->flock;
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

	if (evt->quote.retract)
		evt->type = CHAT_EVENT_RETRACT;

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

	if (!vigil_chat_accepted(peer->slot, peer->id))
		return (0);

	if (chat_outbox_signal(peer))
		return (1);

	if (vigil_atomic_read(&marks[peer->slot].opened[peer->id]))
		return (1);

	if (peer->active != 0 && (vigil->now - peer->active) < CHAT_IDLE)
		return (1);

	if (peer->present && !chat_tunnel_online(peer->tunnel))
		return (1);

	return (0);
}

/*
 * Returns 1 if the given tunnel goes to our companion or a peer we accepted.
 */
static int
chat_tunnel_accepted(struct chat_tunnel *tun)
{
	PRECOND(tun != NULL);

	if (tun->peer == &companion)
		return (1);

	return (vigil_chat_accepted(tun->peer->slot, tun->peer->id));
}

/*
 * Tell the platform that a peer we did not accept tries to reach us, once
 * for each time the peer shows up or we stop accepting it.
 */
static void
chat_ask_check(struct chat_peer *peer)
{
	PRECOND(peer != NULL);

	if (vigil_atomic_read(&marks[peer->slot].ignored[peer->id]) ||
	    vigil_atomic_read(&marks[peer->slot].accepted[peer->id])) {
		peer->asked = 0;
		return;
	}

	if (!peer->present || peer->asked)
		return;

	if (chat_event_queue(CHAT_EVENT_ASK, peer, 0, NULL, 0) == -1)
		return;

	vigil_log(LOG_INFO, "[chat] %02x tries to reach us, asking our "
	    "platform to accept it", peer->id);

	peer->asked = 1;
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
	PRECOND(len > 0 || topic->set || quote->retract ||
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

	if (group->id != 0)
		chat_msg_backoff(msg);

	TAILQ_INSERT_TAIL(&peer->outbox, msg, wlist);

	if (peer->tunnel != NULL)
		chat_msg_queue(peer->tunnel, msg);
}

/*
 * Returns 1 if a message on the outbox of the given peer wants us to signal
 * the peer right now. A message for a group is tried again with a backoff,
 * the others want it until they are acked, and so does anything on a tunnel
 * that is online or for a peer that discovery sees online.
 */
static int
chat_outbox_signal(struct chat_peer *peer)
{
	struct chat_msg		*msg;

	PRECOND(peer != NULL);

	if (TAILQ_EMPTY(&peer->outbox))
		return (0);

	if (chat_tunnel_online(peer->tunnel))
		return (1);

	if (vigil_call_peer(peer->slot, peer->id) & VIGIL_CALL_PEER_ONLINE)
		return (1);

	TAILQ_FOREACH(msg, &peer->outbox, wlist) {
		if (chat_msg_signal(msg))
			return (1);
	}

	return (0);
}

/*
 * Drop the message with the given ref from the outbox of the given peer.
 */
static void
chat_outbox_drop(struct chat_peer *peer, u_int64_t ref)
{
	struct chat_msg		*msg;

	PRECOND(peer != NULL);

	TAILQ_FOREACH(msg, &peer->outbox, wlist) {
		if (msg->ref == ref)
			break;
	}

	if (msg == NULL)
		return;

	if (msg->pending)
		TAILQ_REMOVE(&peer->tunnel->sendq, msg, slist);

	if (peer->tunnel != NULL && peer->tunnel->xfer != NULL &&
	    peer->tunnel->xfer->msg == msg)
		chat_xfer_free(peer->tunnel);

	TAILQ_REMOVE(&peer->outbox, msg, wlist);
	chat_msg_free(msg);
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
	int			idx;
	u_int8_t		slot;
	struct chat_gospel	*gospel;

	if (token == 0)
		return;

	for (slot = 0; slot < VIGIL_GOSPEL_MAX; slot++) {
		gospel = &gospels[slot];

		if (!gospel->running)
			continue;

		for (idx = 0; idx < KYRKA_PEERS_PER_FLOCK; idx++) {
			if (chat_tunnel_ack(gospel->peers[idx].tunnel,
			    token) != -1)
				return;
		}
	}

	(void)chat_tunnel_ack(companion.tunnel, token);
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

		if (tun->peer == &companion && tun->seen[slot].journal != 0) {
			chat_desktop_stored(tun->seen[slot].journal,
			    tun->seen[slot].synced);
		}

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
chat_command_queue(int type, u_int64_t flock, u_int8_t peer, u_int64_t value,
    const struct chat_quote *quote, const struct chat_topic *topic,
    const struct chat_group *group, const void *data, size_t len)
{
	struct chat_command	*cmd;

	PRECOND(type == CHAT_COMMAND_SEND ||
	    type == CHAT_COMMAND_FORGET ||
	    type == CHAT_COMMAND_DROP ||
	    type == CHAT_COMMAND_STORED ||
	    type == CHAT_COMMAND_CALL ||
	    type == CHAT_COMMAND_SYNC ||
	    type == CHAT_COMMAND_INPUT);
	PRECOND(flock != 0 || type == CHAT_COMMAND_STORED ||
	    type == CHAT_COMMAND_SYNC);
	PRECOND(data != NULL || len == 0);
	PRECOND(len <= VIGIL_CHAT_MESSAGE_MAX);

	cmd = calloc(1, sizeof(*cmd));
	if (cmd == NULL)
		fatal("calloc: failed to allocate chat command");

	cmd->len = len;
	cmd->type = type;
	cmd->peer = peer;
	cmd->flock = flock;
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
 * Queue a command for the event loop that carries the given JSON with the
 * given data after it in a blob, returns -1 if we are not taking any or
 * have no room for it.
 */
static int
chat_command_blob(int type, u_int64_t value, const char *json, size_t len,
    const void *data, size_t size)
{
	struct chat_command	*cmd;
	struct chat_blob	*blob;

	PRECOND(type == CHAT_COMMAND_SYNC || type == CHAT_COMMAND_REQUEST);
	PRECOND(json != NULL);
	PRECOND(data != NULL || size == 0);

	if ((blob = calloc(1, sizeof(*blob))) == NULL)
		fatal("calloc: failed to allocate command blob");

	blob->refs = 1;
	blob->size = len + size;
	blob->ref = vigil_atomic_add(&companion_blobs, 1) + 1;

	if ((blob->data = malloc(blob->size)) == NULL)
		fatal("malloc: failed to allocate command blob data");

	memcpy(blob->data, json, len);

	if (size > 0)
		memcpy(blob->data + len, data, size);

	if ((cmd = calloc(1, sizeof(*cmd))) == NULL)
		fatal("calloc: failed to allocate chat command");

	cmd->len = len;
	cmd->blob = blob;
	cmd->type = type;
	cmd->value = value;

	return (chat_command_insert(cmd));
}

/*
 * Queue a command for the event loop that carries a copy of the given
 * data, returns -1 if we are not taking any or have no room for it.
 * Data we already hold is not copied again.
 */
static int
chat_data_queue(u_int64_t flock, u_int8_t peer, u_int64_t value,
    const struct chat_group *group, u_int64_t topic, int kind,
    const char *name, const void *data, size_t len)
{
	int				ret;
	u_int64_t			id;
	size_t				off;
	size_t				size;
	u_int32_t			serial;
	struct chat_command		*cmd;
	struct vigil_chat_data_head	head;
	struct chat_blob		*blob;

	PRECOND(flock != 0);
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
	ret = chat_blob_room(flock, peer, value, size);
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
	blob->flock = flock;
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
	cmd->flock = flock;
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
		ret = chat_blob_room(cmd->blob->flock, cmd->blob->peer,
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
	size_t		need;

	PRECOND(cmd != NULL);

	if (cmd->type == CHAT_COMMAND_SYNC && cmd->value == CHAT_SYNC_ENTRY) {
		need = chat_phone_need(cmd->len, cmd->blob->size - cmd->len);
		chat_phone_release(need);
	}

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
	u_int8_t		slot;
	struct chat_peer	*peer;

	for (;;) {
		chat_command_next(&cmd);
		if (cmd == NULL)
			break;

		/* Without gospel we are a desktop, all goes via our phone. */
		if (vigil->config->mode == VIGIL_MODE_COMPANION) {
			if (cmd->type == CHAT_COMMAND_STORED)
				chat_token_ack(cmd->value);
			else if (cmd->type == CHAT_COMMAND_SYNC)
				chat_desktop_unsync();
			else if (companion_role == VIGIL_COMPANION_DESKTOP)
				chat_desktop_forward(cmd);

			chat_command_free(cmd);
			continue;
		}

		peer = NULL;
		local = 0;

		if (cmd->flock != 0) {
			if (vigil_gospel_find(cmd->flock, &slot) == -1 ||
			    !gospels[slot].running) {
				vigil_log(LOG_NOTICE, "[chat] dropped a command"
				    " for %" PRIx64 ", it does not run",
				    cmd->flock);
				chat_command_free(cmd);
				continue;
			}

			local = gospels[slot].local;
			peer = &gospels[slot].peers[cmd->peer];

			if (cmd->group.id != 0 && cmd->group.owner == 0)
				cmd->group.owner = local;
		}

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
		case CHAT_COMMAND_DROP:
			chat_outbox_drop(peer, cmd->value);
			break;
		case CHAT_COMMAND_INPUT:
			if (peer->tunnel != NULL)
				chat_tunnel_free(peer->tunnel);
			break;
		case CHAT_COMMAND_STORED:
			chat_token_ack(cmd->value);
			break;
		case CHAT_COMMAND_DATA:
			if (cmd->peer != local)
				chat_outbox_data(peer, cmd->value, cmd->blob);
			break;
		case CHAT_COMMAND_SYNC:
			if (companion_role == VIGIL_COMPANION_PHONE)
				chat_phone_sync(cmd);
			break;
		case CHAT_COMMAND_REQUEST:
		case CHAT_COMMAND_CALL:
			fatal("chat command %d is only for a desktop", cmd->type);
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
	    type == CHAT_EVENT_AMBRY ||
	    type == CHAT_EVENT_SAFETY ||
	    type == CHAT_EVENT_ASK);
	PRECOND(peer != NULL);
	PRECOND(data != NULL || len == 0);
	PRECOND(len <= VIGIL_CHAT_MESSAGE_MAX);

	if (peer == &companion)
		return (0);

	evt = calloc(1, sizeof(*evt));
	if (evt == NULL)
		fatal("calloc: failed to allocate chat event");

	evt->type = type;
	evt->value = value;
	evt->peer = peer->id;
	evt->flock = peer->flock;

	memcpy(evt->name, peer->name, sizeof(evt->name));
	memcpy(evt->nick, peer->nick, sizeof(evt->nick));

	if (len > 0)
		memcpy(evt->text, data, len);

	if (type == CHAT_EVENT_AMBRY)
		return (chat_event_insert(evt));

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
 * Returns 1 if we are typing to the given peer.
 */
static int
chat_typed(struct chat_peer *peer)
{
	PRECOND(peer != NULL);

	if (peer->slot == VIGIL_GOSPEL_MAX)
		return (0);

	return (vigil_atomic_read(&marks[peer->slot].typing[peer->id]));
}

/*
 * Take the group and topic we are typing in that the given peer may know
 * about, an id of 0 means none. A peer outside the group or its flock
 * learns nothing, a topic outside a group only goes to the peer we type to.
 */
static void
chat_typing_state(struct chat_peer *peer, u_int8_t *owner, u_int64_t *id,
    u_int64_t *topic)
{
	u_int8_t	who;

	PRECOND(peer != NULL);
	PRECOND(owner != NULL);
	PRECOND(id != NULL);
	PRECOND(topic != NULL);

	who = peer->id;

	vigil_lock(&lock);
	*id = typing_group;
	*topic = typing_topic;
	*owner = typing_owner;

	if (*id != 0 && peer->flock != typing_flock) {
		*id = 0;
		*owner = 0;
		*topic = 0;
	}

	if (*id != 0 && who != *owner &&
	    !(typing_members[who / 8] & (1 << (who % 8)))) {
		*id = 0;
		*owner = 0;
		*topic = 0;
	}
	vigil_unlock(&lock);

	if (*id == 0 && chat_typed(peer) == 0)
		*topic = 0;
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
		chat_event_free(evt);
		return (-1);
	}

	event_count++;
	TAILQ_INSERT_TAIL(&events, evt, list);

	vigil_unlock(&lock);

	return (0);
}

/*
 * Returns -1 if the platform has no room for another event.
 */
static int
chat_events_room(void)
{
	int		ret;

	ret = 0;

	vigil_lock(&lock);

	if (event_count >= CHAT_EVENTS_MAX)
		ret = -1;

	vigil_unlock(&lock);

	return (ret);
}

/*
 * Returns how many bytes the given event takes at most as JSON, quoting
 * can turn every byte of a string into six.
 */
static size_t
chat_event_need(const struct chat_event *evt)
{
	size_t		need;

	PRECOND(evt != NULL);

	need = CHAT_EVENT_JSON_MAX + evt->json_len;
	need += 5 * strlen(evt->text);
	need += 5 * strlen(evt->nick);
	need += 5 * strlen(evt->name);

	return (need);
}

/*
 * Move the events for the platform onto the given list for as long as
 * they fit in room bytes of JSON.
 */
static void
chat_events_take(struct chat_event_list *list, size_t room)
{
	struct chat_event	*evt;
	size_t			need;

	PRECOND(list != NULL);

	vigil_lock(&lock);

	while ((evt = TAILQ_FIRST(&events)) != NULL) {
		need = chat_event_need(evt);
		if (need > room)
			break;

		TAILQ_REMOVE(&events, evt, list);
		TAILQ_INSERT_TAIL(list, evt, list);

		event_count--;
		room -= need;
	}

	vigil_unlock(&lock);
}

/*
 * Append the given event as a JSON object to the given buffer.
 */
static void
chat_event_render(struct vigil_buf *buf, struct chat_event *evt, int first)
{
	PRECOND(buf != NULL);
	PRECOND(evt != NULL);

	vigil_buf_appendf(buf, "%s{\"flock\":\"%" PRIx64 "\",\"peer\":\"%02x\"",
	    first ? "" : ",", evt->flock, evt->peer);

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
		    ",\"ended\":%s", evt->value,
		    evt->group.state == VIGIL_CHAT_GROUP_ENDED ?
		    "true" : "false");

		chat_event_members(buf, evt->group.members);

		vigil_buf_appendf(buf, ",\"name\":");
		vigil_buf_quote(buf, evt->text);
		break;
	case CHAT_EVENT_LEAVE:
		vigil_buf_appendf(buf,
		    ",\"type\":\"leave\",\"token\":%" PRIu64, evt->value);
		break;
	case CHAT_EVENT_RETRACT:
		vigil_buf_appendf(buf,
		    ",\"type\":\"retract\",\"token\":%" PRIu64
		    ",\"hash\":\"%016" PRIx64 "\"", evt->value,
		    evt->quote.hash);

		if (evt->topic.id != 0) {
			vigil_buf_appendf(buf, ",\"topic\":\"%016" PRIx64 "\"",
			    evt->topic.id);
		}
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
	case CHAT_EVENT_SAFETY:
		vigil_buf_appendf(buf, ",\"type\":\"safety\"");
		break;
	case CHAT_EVENT_ASK:
		vigil_buf_appendf(buf, ",\"type\":\"ask\"");
		break;
	case CHAT_EVENT_ECHO:
		chat_event_echo(buf, evt);
		break;
	case CHAT_EVENT_MIRROR:
		vigil_buf_appendf(buf, ",\"type\":\"mirror\",\"token\":%" PRIu64
		    ",\"size\":%zu,\"events\":[%.*s]", evt->value, evt->size,
		    (int)evt->json_len, (const char *)evt->json);
		break;
	case CHAT_EVENT_DESKTOP:
		chat_event_desktop(buf, evt);
		break;
	}

	vigil_buf_appendf(buf, "}");
}

/*
 * Append the fields of the given echo event.
 */
static void
chat_event_echo(struct vigil_buf *buf, struct chat_event *evt)
{
	PRECOND(buf != NULL);
	PRECOND(evt != NULL);
	PRECOND(evt->type == CHAT_EVENT_ECHO);

	vigil_buf_appendf(buf, ",\"type\":\"echo\",\"ref\":%" PRIu64,
	    evt->value);

	if (evt->token != 0)
		vigil_buf_appendf(buf, ",\"token\":%" PRIu64, evt->token);

	if (evt->kind != 0) {
		vigil_buf_appendf(buf, ",\"what\":\"data\",\"kind\":\"%s\""
		    ",\"size\":%zu,\"name\":", chat_data_kind_name(evt->kind),
		    evt->size);
		vigil_buf_quote(buf, evt->text);
	} else if (evt->topic.set) {
		vigil_buf_appendf(buf, ",\"what\":\"topic\",\"color\":\"%06x\""
		    ",\"name\":", evt->topic.color);
		vigil_buf_quote(buf, evt->text);
	} else if (evt->group.type == VIGIL_CHAT_TYPE_ROSTER) {
		vigil_buf_appendf(buf, ",\"what\":\"roster\",\"ended\":%s"
		    ",\"name\":", evt->group.state == VIGIL_CHAT_GROUP_ENDED ?
		    "true" : "false");
		vigil_buf_quote(buf, evt->text);
	} else if (evt->group.type == VIGIL_CHAT_TYPE_LEAVE) {
		vigil_buf_appendf(buf, ",\"what\":\"leave\"");
	} else if (evt->quote.retract) {
		vigil_buf_appendf(buf, ",\"what\":\"retract\""
		    ",\"hash\":\"%016" PRIx64 "\"", evt->quote.hash);
	} else {
		vigil_buf_appendf(buf, ",\"what\":\"text\",\"text\":");
		vigil_buf_quote(buf, evt->text);

		if (evt->quote.hash != 0) {
			vigil_buf_appendf(buf,
			    ",\"author\":\"%02x\",\"hash\":\"%016" PRIx64 "\"",
			    evt->quote.author, evt->quote.hash);
		}
	}

	if (evt->topic.id != 0) {
		vigil_buf_appendf(buf, ",\"topic\":\"%016" PRIx64 "\"",
		    evt->topic.id);
	}

	if (evt->group.id != 0 && evt->group.type != VIGIL_CHAT_TYPE_LEAVE)
		chat_event_members(buf, evt->group.members);
}

/*
 * Append the fields of the given desktop event.
 */
static void
chat_event_desktop(struct vigil_buf *buf, struct chat_event *evt)
{
	PRECOND(buf != NULL);
	PRECOND(evt != NULL);
	PRECOND(evt->type == CHAT_EVENT_DESKTOP);

	vigil_buf_appendf(buf, ",\"type\":\"desktop\"");

	switch (evt->kind) {
	case CHAT_DESKTOP_RESYNC:
		vigil_buf_appendf(buf, ",\"what\":\"resync\"");
		break;
	case CHAT_DESKTOP_ASK:
		vigil_buf_appendf(buf, ",\"what\":\"ask\",\"ask\":");
		vigil_buf_quote(buf, evt->text);

		if (evt->token != 0) {
			vigil_buf_appendf(buf, ",\"token\":%" PRIu64
			    ",\"size\":%zu", evt->token, evt->size);
		}
		break;
	case CHAT_DESKTOP_UNSENT:
		vigil_buf_appendf(buf, ",\"what\":\"unsent\",\"ref\":%" PRIu64,
		    evt->value);
		break;
	case CHAT_DESKTOP_QUEUED:
		vigil_buf_appendf(buf, ",\"what\":\"queued\",\"ref\":%" PRIu64,
		    evt->value);

		if (evt->size > 0) {
			vigil_buf_appendf(buf, ",\"kind\":\"%s\",\"size\":%zu"
			    ",\"name\":", evt->name, evt->size);
		} else {
			vigil_buf_appendf(buf, ",\"text\":");
		}

		vigil_buf_quote(buf, evt->text);

		if (evt->topic.id != 0) {
			vigil_buf_appendf(buf, ",\"topic\":\"%016" PRIx64 "\"",
			    evt->topic.id);
		}

		if (evt->quote.hash != 0) {
			vigil_buf_appendf(buf,
			    ",\"author\":\"%02x\",\"hash\":\"%016" PRIx64 "\"",
			    evt->quote.author, evt->quote.hash);
		}
		break;
	default:
		fatal("unknown desktop event %u", evt->kind);
	}
}

/*
 * Append the members set in the given bits as a JSON array.
 */
static void
chat_event_members(struct vigil_buf *buf, const u_int8_t *members)
{
	int		idx, more;

	PRECOND(buf != NULL);
	PRECOND(members != NULL);

	more = 0;
	vigil_buf_appendf(buf, ",\"members\":[");

	for (idx = 1; idx < KYRKA_PEERS_PER_FLOCK; idx++) {
		if (!(members[idx / 8] & (1 << (idx % 8))))
			continue;

		vigil_buf_appendf(buf, "%s\"%02x\"", more ? "," : "", idx);
		more = 1;
	}

	vigil_buf_appendf(buf, "]");
}

/*
 * Free the given event.
 */
static void
chat_event_free(struct chat_event *evt)
{
	PRECOND(evt != NULL);

	if (evt->json != NULL) {
		nyfe_mem_zero(evt->json, evt->json_len);
		free(evt->json);
	}

	nyfe_mem_zero(evt, sizeof(*evt));
	free(evt);
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
		chat_event_free(evt);
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
	u_int8_t	slot;
	char		id[3];
	char		name[VIGIL_CHAT_NICK_MAX + 1];

	PRECOND(evt != NULL);

	if (evt->group.id == 0)
		return;

	if (vigil_gospel_find(evt->flock, &slot) == -1)
		return;

	vigil_lock(&lock);
	memcpy(name, nick, sizeof(name));
	vigil_unlock(&lock);

	(void)snprintf(id, sizeof(id), "%02x", gospels[slot].local);

	if (chat_mention_name(evt->text, name) ||
	    chat_mention_name(evt->text, id))
		evt->mention = 1;

	if (gospels[slot].named &&
	    chat_mention_name(evt->text, vigil->gospels[slot]->instance))
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
 * Have the given message signal its peer with a backoff, eagerly at first
 * and after that for a short window at a time with ever longer pauses.
 */
static void
chat_msg_backoff(struct chat_msg *msg)
{
	PRECOND(msg != NULL);

	msg->backoff = CHAT_RETRY_START;
	msg->signal = vigil->now + CHAT_RETRY_EAGER;
	msg->retry = msg->signal + msg->backoff;
}

/*
 * Returns 1 if the given message wants us to signal its peer right now,
 * a message without a backoff always does.
 */
static int
chat_msg_signal(struct chat_msg *msg)
{
	PRECOND(msg != NULL);

	if (msg->backoff == 0)
		return (1);

	if (vigil->now < msg->signal)
		return (1);

	if (vigil->now < msg->retry)
		return (0);

	msg->backoff *= 2;

	if (msg->backoff > CHAT_RETRY_MAX)
		msg->backoff = CHAT_RETRY_MAX;

	msg->signal = vigil->now + CHAT_RETRY_WINDOW;
	msg->retry = vigil->now + msg->backoff;

	return (1);
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
chat_blob_room(u_int64_t flock, u_int8_t peer, u_int64_t ref, size_t size)
{
	struct chat_blob	*blob;

	TAILQ_FOREACH(blob, &blobs, list) {
		if (blob->flock == flock && blob->peer == peer &&
		    blob->ref == ref)
			return (1);
	}

	if ((blob_bytes + size) > CHAT_BLOBS_MAX) {
		vigil_log(LOG_NOTICE, "[chat] no room for more data to send");
		return (-1);
	}

	return (0);
}

/*
 * Create a message for the given data.
 */
static struct chat_msg *
chat_data_msg(u_int64_t ref, struct chat_blob *blob)
{
	struct chat_msg		*msg;

	PRECOND(blob != NULL);
	PRECOND(blob->size > sizeof(struct vigil_chat_data_head));

	msg = calloc(1, sizeof(*msg));
	if (msg == NULL)
		fatal("calloc: failed to allocate chat message");

	blob->refs++;

	msg->ref = ref;
	msg->blob = blob;
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
	struct chat_msg			*msg;
	struct vigil_chat_data_head	*head;

	PRECOND(peer != NULL);
	PRECOND(blob != NULL);

	TAILQ_FOREACH(msg, &peer->outbox, wlist) {
		if (msg->ref == ref)
			return;
	}

	msg = chat_data_msg(ref, blob);
	head = (struct vigil_chat_data_head *)blob->data;

	if (head->kind & VIGIL_CHAT_DATA_GROUP)
		chat_msg_backoff(msg);

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

		if (!(tun->features & VIGIL_CHAT_FEATURE_DATA))
			return;

		TAILQ_FOREACH(msg, &tun->peer->outbox, wlist) {
			if (msg->blob != NULL && msg->id != 0)
				break;
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

	if (index >= count)
		return;

	if (size <= sizeof(struct vigil_chat_data_head))
		return;

	if (size > CHAT_WIRE_MAX)
		return;

	if (size > count * VIGIL_CHAT_CHUNK_DATA)
		return;

	if (size <= ((count - 1) * VIGIL_CHAT_CHUNK_DATA))
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

	if (tun->peer == &companion) {
		if (head->kind != VIGIL_CHAT_DATA_COMPANION) {
			chat_data_discard(tun, xfer);
			return (0);
		}

		if (companion_role == VIGIL_COMPANION_PHONE)
			return (chat_phone_request(tun, xfer));

		return (chat_desktop_mirror(tun, xfer));
	}

	if (chat_data_tags(xfer->data, xfer->size, &off,
	    &group, &topic) == -1) {
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

	chat_parcel_hold(parcel);

	seen = &tun->seen[tun->seen_next++ % CHAT_SEEN_MAX];

	seen->hash = 0;
	seen->journal = 0;
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
 * Take the group and topic the given data is in and where the data starts,
 * returns -1 if they are not ones we can use.
 */
static int
chat_data_tags(const u_int8_t *data, size_t size, size_t *off,
    struct chat_group *group, u_int64_t *topic)
{
	u_int64_t				id;
	u_int8_t				kind;
	u_int8_t				flags;
	u_int32_t				serial;
	const struct vigil_chat_data_head	*head;

	PRECOND(data != NULL);
	PRECOND(size > sizeof(*head));
	PRECOND(off != NULL);
	PRECOND(group != NULL);
	PRECOND(topic != NULL);

	memset(group, 0, sizeof(*group));

	*topic = 0;
	*off = sizeof(*head);

	head = (const struct vigil_chat_data_head *)data;
	kind = head->kind & ~CHAT_DATA_FLAGS;
	flags = head->kind & CHAT_DATA_FLAGS;

	if (flags == 0)
		return (0);

	if (kind == VIGIL_CHAT_DATA_AVATAR)
		return (-1);

	if (flags & VIGIL_CHAT_DATA_GROUP) {
		if (size <= *off + CHAT_DATA_GROUP)
			return (-1);

		memcpy(&id, data + *off + 1, sizeof(id));
		memcpy(&serial, data + *off + 1 + sizeof(id), sizeof(serial));

		group->id = be64toh(id);
		group->serial = be32toh(serial);
		group->owner = data[*off];
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
		if (size <= *off + sizeof(*topic))
			return (-1);

		memcpy(topic, data + *off, sizeof(*topic));
		*topic = be64toh(*topic);
		*off += sizeof(*topic);

		if (*topic == 0)
			return (-1);
	}

	return (0);
}

/*
 * We are done with the data on the given xfer, ack it right away.
 */
static void
chat_data_discard(struct chat_tunnel *tun, struct chat_rxfer *xfer)
{
	PRECOND(tun != NULL);
	PRECOND(xfer != NULL);

	chat_data_hold(tun, xfer, 0);
}

/*
 * Remember the data on the given xfer so we know it when it comes again,
 * and ack it once the platform stored what it got under the given token,
 * or right away without a token.
 */
static void
chat_data_hold(struct chat_tunnel *tun, struct chat_rxfer *xfer,
    u_int64_t token)
{
	struct chat_seen	*seen;

	PRECOND(tun != NULL);
	PRECOND(xfer != NULL);

	seen = &tun->seen[tun->seen_next++ % CHAT_SEEN_MAX];

	seen->hash = 0;
	seen->avatar = 0;
	seen->journal = 0;
	seen->id = xfer->id;
	seen->token = token;
	seen->size = xfer->size;
	seen->age = vigil->now;

	if (token == 0)
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
	evt->flock = tun->peer->flock;
	evt->value = parcel->token;
	evt->type = CHAT_EVENT_DATA;
	evt->topic.id = parcel->topic;
	evt->size = parcel->size - parcel->off;
	evt->kind = head->kind & ~CHAT_DATA_FLAGS;

	memcpy(&evt->group, &parcel->group, sizeof(parcel->group));

	memcpy(evt->text, head->name, head->len);
	memcpy(evt->name, tun->peer->name, sizeof(evt->name));
	memcpy(evt->nick, tun->peer->nick, sizeof(evt->nick));

	if (evt->kind == VIGIL_CHAT_DATA_AVATAR)
		evt->type = CHAT_EVENT_AVATAR;

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
 * Hold the given parcel until the platform takes its data.
 */
static void
chat_parcel_hold(struct chat_parcel *parcel)
{
	PRECOND(parcel != NULL);

	vigil_lock(&lock);
	parcel_bytes += parcel->size;
	TAILQ_INSERT_TAIL(&parcels, parcel, list);
	vigil_unlock(&lock);
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

/*
 * Set the configuration of the cathedral tunnel to our phone or to our
 * desktop, an empty one drops it. May be called from any thread, returns
 * -1 if the configuration is too large.
 */
int
vigil_chat_companion(const char *cfg, size_t len)
{
	PRECOND(cfg != NULL || len == 0);

	if (len > sizeof(companion_text))
		return (-1);

	vigil_lock(&lock);

	nyfe_mem_zero(companion_text, sizeof(companion_text));

	if (len > 0)
		memcpy(companion_text, cfg, len);

	companion_len = len;
	companion_serial++;

	vigil_unlock(&lock);

	vigil_wake_signal();

	return (0);
}

/*
 * Pick up the companion configuration the platform gave us if it changed.
 * Next to gospel we are the phone, on our own we are the desktop.
 */
static void
chat_companion_load(void)
{
	size_t			len;
	u_int8_t		role;
	struct vigil_config	*cfg;
	u_int32_t		serial;
	char			err[128];
	char			text[VIGIL_CONFIG_MAX];

	vigil_lock(&lock);

	if (companion_serial == companion_loaded) {
		vigil_unlock(&lock);
		return;
	}

	len = companion_len;
	serial = companion_serial;
	memcpy(text, companion_text, len);

	vigil_unlock(&lock);

	companion_loaded = serial;
	chat_companion_free();

	if (len == 0)
		return;

	cfg = vigil_config_parse(text, len, err, sizeof(err));
	nyfe_mem_zero(text, sizeof(text));

	if (cfg == NULL) {
		vigil_log(LOG_ERR, "[companion] invalid configuration: %s",
		    err);
		return;
	}

	if (cfg->mode != VIGIL_MODE_COMPANION) {
		vigil_log(LOG_ERR, "[companion] not a companion configuration");
		vigil_config_free(cfg);
		return;
	}

	role = cfg->spi >> 8;

	if (vigil->config->mode != VIGIL_MODE_COMPANION &&
	    role != VIGIL_COMPANION_PHONE) {
		vigil_log(LOG_ERR, "[companion] only a phone runs gospel");
		vigil_config_free(cfg);
		return;
	}

	if (vigil->config->mode == VIGIL_MODE_COMPANION &&
	    role != VIGIL_COMPANION_DESKTOP) {
		vigil_log(LOG_ERR, "[companion] only a desktop runs alone");
		vigil_config_free(cfg);
		return;
	}

	companion_cfg = cfg;
	companion_role = role;
	companion.id = cfg->spi & 0xff;

	if (chat_companion_journal() == -1) {
		vigil_config_free(cfg);
		companion_cfg = NULL;
		companion_role = 0;
		return;
	}

	companion_told = 0;
	companion_avatar = 0;
	companion_resolve = 0;
	companion_resolved = 0;

	companion_turn = 0;
	companion_attached = 0;

	memset(companion_nick, 0, sizeof(companion_nick));
	memset(&companion_phone, 0, sizeof(companion_phone));
	memset(companion_phones, 0, sizeof(companion_phones));
	memset(&companion_desktop, 0, sizeof(companion_desktop));

	vigil_log(LOG_INFO, "[companion] we are %s '%s'",
	    role == VIGIL_COMPANION_PHONE ? "the phone" : "the desktop",
	    cfg->instance);
}

/*
 * Open our journal in our data directory. A phone needs one, a desktop
 * without one only remembers what it stored while it runs.
 */
static int
chat_companion_journal(void)
{
	PRECOND(companion_cfg != NULL);

	journal_sent = 0;
	journal_heard = 0;
	memset(&journal, 0, sizeof(journal));

	requests_sent = 0;
	requests_told = 0;
	requests_held = 0;
	requests_head = 0;
	requests_ref = 0;
	requests_done = 0;
	memset(&requests, 0, sizeof(requests));

	if (companion_role == VIGIL_COMPANION_PHONE) {
		if (vigil->datadir == NULL) {
			vigil_log(LOG_ERR, "[companion] no data directory");
			return (-1);
		}

		if (vigil_journal_open(&journal, vigil->datadir, "journal",
		    companion_cfg->kek, sizeof(companion_cfg->kek)) == -1)
			return (-1);

		journal_sent = journal.acked;
		vigil_atomic_write(&companion_journal, journal.bytes);

		companion_measure = 0;
		chat_phone_limit();

		vigil_log(LOG_INFO, "[companion] journal holds %zu entries "
		    "up to %" PRIu64, journal.items, journal.last);

		return (0);
	}

	if (vigil->datadir == NULL)
		return (0);

	if (vigil_journal_open(&journal, vigil->datadir, "journal",
	    NULL, 0) == -1)
		memset(&journal, 0, sizeof(journal));

	if (vigil_journal_open(&requests, vigil->datadir, "requests",
	    companion_cfg->kek, sizeof(companion_cfg->kek)) == -1)
		memset(&requests, 0, sizeof(requests));

	requests_sent = requests.acked;
	requests_told = requests.acked;
	requests_held = requests.last;

	chat_desktop_head();

	vigil_log(LOG_INFO, "[companion] we have up to %" PRIu64 ", we hold "
	    "%zu requests for our phone", journal.acked, requests.items);

	return (0);
}

/*
 * Drop our companion tunnel, all we held for it and its configuration.
 */
static void
chat_companion_free(void)
{
	struct vigil_companion_phone	phone;
	struct vigil_companion_desktop	desktop;

	memset(&phone, 0, sizeof(phone));
	memset(&desktop, 0, sizeof(desktop));

	if (companion_role == VIGIL_COMPANION_PHONE)
		chat_phone_desktop(&desktop);

	if (companion_role == VIGIL_COMPANION_DESKTOP)
		chat_desktop_phone(&phone);

	if (companion.tunnel != NULL)
		chat_tunnel_free(companion.tunnel);

	chat_outbox_free(&companion);

	if (companion_cfg != NULL) {
		vigil_config_free(companion_cfg);
		companion_cfg = NULL;
	}

	companion_role = 0;
	companion_reset = 0;
	companion_synced = 0;
	companion_dirty = 0;
	companion_asked = 0;
	companion_last = 0;
	companion_other = 0;
	companion_known = 0;

	journal_sent = 0;
	journal_heard = 0;
	vigil_journal_close(&journal);

	requests_sent = 0;
	requests_told = 0;
	requests_held = 0;
	requests_head = 0;
	requests_ref = 0;
	requests_done = 0;
	vigil_journal_close(&requests);

	vigil_atomic_write(&companion_limit, 0);
	vigil_atomic_write(&companion_journal, 0);
}

/*
 * Keep the tunnel to our companion up while we have a companion
 * configuration, and send our state when that state changes.
 */
static void
chat_companion_check(void)
{
	struct vigil_cathedral		cat;
	struct chat_tunnel		*tun;
	u_int64_t			next;
	struct vigil_companion_phone	phone;
	int				ringing;
	struct vigil_companion_desktop	desktop;

	if (companion_cfg == NULL)
		return;

	if (!companion_resolved) {
		if (vigil->now < companion_resolve)
			return;

		companion_resolve = vigil->now + CHAT_COMPANION_RESOLVE;

		if (vigil_resolve(companion_cfg->cathedral_host,
		    companion_cfg->cathedral_port, &companion_addr) == -1)
			return;

		companion_resolved = 1;
	}

	tun = companion.tunnel;

	if (tun == NULL) {
		memset(&cat, 0, sizeof(cat));

		cat.last = vigil->now;
		cat.timeout = VIGIL_CATHEDRAL_TIMEOUT_INIT;
		memcpy(&cat.addr, &companion_addr, sizeof(companion_addr));

		tun = chat_tunnel_new(&companion, &cat);
		if (tun == NULL)
			return;
	}

	memset(&phone, 0, sizeof(phone));
	memset(&desktop, 0, sizeof(desktop));

	chat_tunnel_manage(tun);

	if (companion_role == VIGIL_COMPANION_PHONE) {
		chat_phone_limit();
		chat_phone_self();
		chat_phone_pump();
		chat_phone_resync();

		if (!chat_tunnel_online(tun)) {
			journal_heard = 0;
			chat_phone_desktop(&desktop);
			memset(companion_phones, 0, sizeof(companion_phones));
		}

		ringing = chat_phone_changed();

		if (ringing == -1)
			tun->next_heartbeat = vigil->now;

		next = vigil->now + CHAT_COMPANION_CALL_HEARTBEAT;

		if (ringing == 1 && tun->next_heartbeat > next)
			tun->next_heartbeat = next;
	} else {
		chat_desktop_told();
		chat_desktop_pump();
		chat_desktop_state(&desktop);

		/* We can hear our phone before our tunnel counts as online. */
		if ((vigil->now - tun->last_rx) >= CHAT_RX_TIMEOUT)
			chat_desktop_phone(&phone);

		if (memcmp(&desktop, &companion_desktop, sizeof(desktop)))
			tun->next_heartbeat = vigil->now;

		vigil_voice_relay_update();
	}
}

/*
 * Append the state of our companion tunnel to the status.
 */
static void
chat_companion_status(struct vigil_buf *buf)
{
	struct chat_msg		*msg;
	const char		*link;
	const char		*sync;
	size_t			queued;
	int			online, p2p;
	u_int64_t		room, held, limit, behind;

	PRECOND(buf != NULL);

	if (companion_cfg == NULL)
		return;

	queued = 0;
	TAILQ_FOREACH(msg, &companion.outbox, wlist)
		queued++;

	if (companion_role == VIGIL_COMPANION_PHONE)
		queued = journal.last - journal.acked;

	if (companion_role == VIGIL_COMPANION_DESKTOP)
		queued += requests.last - requests.acked;

	chat_tunnel_state(companion.tunnel, &link, &online, &p2p);

	vigil_buf_appendf(buf, ",\"companion\":{\"role\":\"%s\""
	    ",\"id\":\"%02x\",\"online\":%s,\"link\":\"%s\",\"queued\":%zu",
	    companion_role == VIGIL_COMPANION_PHONE ? "phone" : "desktop",
	    companion.id, online ? "true" : "false", link, queued);

	vigil_buf_appendf(buf, ",\"version\":%u", VIGIL_COMPANION_VERSION);

	if (companion_known)
		vigil_buf_appendf(buf, ",\"other\":%u", companion_other);

	if (companion_role == VIGIL_COMPANION_PHONE) {
		room = 0;
		limit = vigil_atomic_read(&companion_limit);
		held = vigil_atomic_read(&companion_journal) +
		    vigil_atomic_read(&companion_queued);

		if (held < limit)
			room = limit - held;

		vigil_buf_appendf(buf, ",\"room\":%" PRIu64, room);
	}

	if (companion_role == VIGIL_COMPANION_DESKTOP) {
		sync = "unsynced";
		if (journal.synced == VIGIL_JOURNAL_SYNCED)
			sync = "synced";
		else if (journal.synced == VIGIL_JOURNAL_SYNCING)
			sync = "syncing";

		behind = 0;
		if (companion_last > journal.acked)
			behind = companion_last - journal.acked;

		vigil_buf_appendf(buf, ",\"sync\":\"%s\",\"behind\":%" PRIu64,
		    sync, behind);
		chat_companion_peers(buf);
	}

	vigil_buf_appendf(buf, "}");
}

/*
 * Returns -1 if we cannot hold size more bytes for our companion.
 */
static int
chat_companion_room(size_t size)
{
	struct chat_msg		*msg;
	size_t			items, bytes;

	items = 0;
	bytes = size;

	TAILQ_FOREACH(msg, &companion.outbox, wlist) {
		items++;

		if (msg->blob != NULL)
			bytes += msg->blob->size;
	}

	if (items >= CHAT_COMPANION_ITEMS || bytes > CHAT_COMPANION_BYTES) {
		vigil_log(LOG_NOTICE,
		    "[companion] our outbox is full, dropped a parcel");
		return (-1);
	}

	return (0);
}

/*
 * Make a new blob for our companion with room for size bytes after its
 * head, the caller fills those in.
 */
static struct chat_blob *
chat_companion_blob(size_t size)
{
	struct vigil_chat_data_head	head;
	struct chat_blob		*blob;

	PRECOND(size > 0);

	memset(&head, 0, sizeof(head));
	head.kind = VIGIL_CHAT_DATA_COMPANION;
	head.len = strlen(CHAT_COMPANION_NAME);
	memcpy(head.name, CHAT_COMPANION_NAME, head.len);

	blob = calloc(1, sizeof(*blob));
	if (blob == NULL)
		fatal("calloc: failed to allocate companion blob");

	blob->refs = 1;
	blob->peer = companion.id;
	blob->ref = ++companion_refs;
	blob->size = sizeof(head) + size;
	blob->count = (blob->size + VIGIL_CHAT_CHUNK_DATA - 1) /
	    VIGIL_CHAT_CHUNK_DATA;

	blob->data = calloc(1, blob->size);
	if (blob->data == NULL)
		fatal("calloc: failed to allocate companion blob data");

	memcpy(blob->data, &head, sizeof(head));

	return (blob);
}

/*
 * Put the given blob on the outbox for our companion and start sending it
 * right away when nothing else goes out, instead of on our next pass.
 */
static void
chat_companion_push(struct chat_blob *blob)
{
	struct chat_tunnel	*tun;
	struct chat_msg		*msg;

	PRECOND(blob != NULL);

	msg = chat_data_msg(blob->ref, blob);
	chat_blob_release(blob);

	TAILQ_INSERT_TAIL(&companion.outbox, msg, wlist);

	if ((tun = companion.tunnel) == NULL)
		return;

	chat_msg_queue(tun, msg);

	if (tun->xfer == NULL && chat_tunnel_online(tun))
		chat_xfer_run(tun);
}

/*
 * Send our desktop who we are once we heard from our desktop, and again
 * when our nickname or avatar changes.
 */
static void
chat_phone_self(void)
{
	struct vigil_buf	out;
	size_t			len;
	u_int8_t		*face;
	u_int32_t		serial;
	struct chat_gospel	*gospel;
	int			ret, first;
	u_int8_t		slot;
	char			json[CHAT_EVENT_JSON_MAX];
	char			name[VIGIL_CHAT_NICK_MAX + 1];

	PRECOND(companion_role == VIGIL_COMPANION_PHONE);

	if (companion.tunnel == NULL || !companion.tunnel->heard)
		return;

	vigil_lock(&lock);
	serial = avatar_serial;
	memcpy(name, nick, sizeof(name));
	vigil_unlock(&lock);

	if (companion_told && !strcmp(name, companion_nick) &&
	    serial == companion_avatar && attached == companion_attached)
		return;

	len = 0;
	face = NULL;

	if (serial != companion_avatar) {
		if ((face = calloc(1, VIGIL_CHAT_AVATAR_MAX)) == NULL)
			fatal("calloc: failed to allocate our avatar");

		vigil_lock(&lock);
		serial = avatar_serial;
		len = avatar_len;
		memcpy(face, avatar, len);
		vigil_unlock(&lock);
	}

	vigil_buf_init(&out, json, sizeof(json));
	vigil_buf_appendf(&out, "{\"op\":\"self\",\"nick\":");
	vigil_buf_quote(&out, name);
	vigil_buf_appendf(&out, ",\"gospels\":[");

	first = 1;

	for (slot = 0; slot < VIGIL_GOSPEL_MAX; slot++) {
		gospel = &gospels[slot];

		if (!gospel->running)
			continue;

		vigil_buf_appendf(&out,
		    "%s{\"flock\":\"%" PRIx64 "\",\"id\":\"%02x\",\"signed\":",
		    first ? "" : ",", gospel->flock, gospel->local);
		vigil_buf_quote(&out,
		    gospel->named ? vigil->gospels[slot]->instance : "");
		vigil_buf_appendf(&out, "}");

		first = 0;
	}

	vigil_buf_appendf(&out, "]");

	if (face != NULL)
		vigil_buf_appendf(&out, ",\"avatar\":%zu", len);

	vigil_buf_appendf(&out, "}");

	ret = 0;

	if (!(out.flags & VIGIL_BUF_FULL))
		ret = chat_phone_write(json, out.offset, face, len);

	if (face != NULL) {
		nyfe_mem_zero(face, VIGIL_CHAT_AVATAR_MAX);
		free(face);
	}

	if (ret == -1)
		return;

	companion_told = 1;
	companion_avatar = serial;
	companion_attached = attached;
	memcpy(companion_nick, name, sizeof(companion_nick));
}

/*
 * Run what our platform handed us for our desktop, an entry or one of the
 * marks around a full copy of our chats.
 */
static void
chat_phone_sync(struct chat_command *cmd)
{
	const u_int8_t		*data;

	PRECOND(cmd != NULL);
	PRECOND(cmd->type == CHAT_COMMAND_SYNC);
	PRECOND(companion_role == VIGIL_COMPANION_PHONE);

	switch (cmd->value) {
	case CHAT_SYNC_ENTRY:
		data = NULL;
		if (cmd->blob->size > cmd->len)
			data = cmd->blob->data + cmd->len;

		(void)chat_phone_queue((const char *)cmd->blob->data, cmd->len,
		    data, cmd->blob->size - cmd->len);
		break;
	case CHAT_SYNC_RESET:
		if (chat_phone_write(CHAT_SYNC_RESET_JSON,
		    sizeof(CHAT_SYNC_RESET_JSON) - 1, NULL, 0) == -1)
			break;

		companion_told = 0;
		companion_dirty = 0;
		companion_asked = 0;
		companion_avatar = 0;
		companion_reset = journal.last;

		vigil_log(LOG_INFO, "[companion] a full resync for our desktop "
		    "starts at entry %" PRIu64, journal.last);
		break;
	case CHAT_SYNC_DONE:
		if (chat_phone_write(CHAT_SYNC_DONE_JSON,
		    sizeof(CHAT_SYNC_DONE_JSON) - 1, NULL, 0) == -1)
			break;

		companion_synced = journal.last;

		vigil_log(LOG_INFO, "[companion] a resync for our desktop "
		    "ends at entry %" PRIu64, journal.last);
		break;
	default:
		fatal("unknown sync %" PRIu64, cmd->value);
	}
}

/*
 * Ask our platform for a full copy of our chats when we lost an entry for
 * our desktop, or when our desktop says it lacks one and no copy is on its
 * way.
 */
static void
chat_phone_resync(void)
{
	struct chat_event	*evt;

	PRECOND(companion_role == VIGIL_COMPANION_PHONE);

	if (companion_asked)
		return;

	if (chat_companion_mismatch())
		return;

	if (!companion_dirty) {
		if (!journal_heard)
			return;

		if (companion_desktop.synced == VIGIL_JOURNAL_SYNCED)
			return;

		if (companion_reset > companion_synced)
			return;

		if (companion_synced > be64toh(companion_desktop.have))
			return;
	}

	evt = calloc(1, sizeof(*evt));
	if (evt == NULL)
		fatal("calloc: failed to allocate desktop event");

	evt->peer = companion.id;
	evt->type = CHAT_EVENT_DESKTOP;
	evt->kind = CHAT_DESKTOP_RESYNC;

	if (chat_event_insert(evt) == -1)
		return;

	vigil_log(LOG_INFO,
	    "[companion] starting a full resync for our desktop");
	companion_asked = 1;
}

/*
 * Set how many bytes our journal may hold from the free space on our disk,
 * once an hour.
 */
static void
chat_phone_limit(void)
{
	struct statvfs		st;
	u_int64_t		limit;

	PRECOND(companion_role == VIGIL_COMPANION_PHONE);
	PRECOND(vigil->datadir != NULL);

	if (vigil->now < companion_measure)
		return;

	companion_measure = vigil->now + CHAT_JOURNAL_MEASURE;

	if (statvfs(vigil->datadir, &st) == -1) {
		vigil_log(LOG_NOTICE, "[companion] statvfs(%s): %s",
		    vigil->datadir, errno_s);
		return;
	}

	limit = (u_int64_t)st.f_bavail * st.f_frsize / CHAT_JOURNAL_SHARE;

	if (limit > CHAT_JOURNAL_MAX)
		limit = CHAT_JOURNAL_MAX;

	vigil_atomic_write(&companion_limit, limit);
}

/*
 * Returns the most bytes an entry with len bytes of JSON and size bytes of
 * data takes in our journal.
 */
static size_t
chat_phone_need(size_t len, size_t size)
{
	return (len + size + CHAT_JOURNAL_EXTRA);
}

/*
 * Hold need bytes in our journal for an entry we did not write yet.
 * Returns -1 if our journal has no room for it.
 */
static int
chat_phone_reserve(size_t need)
{
	u_int64_t	held, limit;

	held = vigil_atomic_add(&companion_queued, need) + need;
	held += vigil_atomic_read(&companion_journal);
	limit = vigil_atomic_read(&companion_limit);

	if (held > limit) {
		chat_phone_release(need);
		return (-1);
	}

	return (0);
}

/*
 * Give back the need bytes we held for an entry in our journal.
 */
static void
chat_phone_release(size_t need)
{
	vigil_atomic_sub(&companion_queued, need);
}

/*
 * Write an entry of our own to our journal for our desktop if it has room
 * for it. Returns -1 if it has no room or we lost the entry.
 */
static int
chat_phone_write(const char *json, size_t len, const u_int8_t *data,
    size_t size)
{
	int		ret;
	size_t		need;

	PRECOND(json != NULL);
	PRECOND(data != NULL || size == 0);

	need = chat_phone_need(len, size);

	if (chat_phone_reserve(need) == -1) {
		vigil_log(LOG_NOTICE,
		    "[companion] no room in our journal, dropped an entry");
		return (-1);
	}

	ret = chat_phone_queue(json, len, data, size);
	chat_phone_release(need);

	return (ret);
}

/*
 * Write the given JSON event with the given data, if any, to our journal
 * for our desktop. An entry is the length of the JSON, the JSON with the
 * time we got the event in at, and the data. Returns -1 if we lost it.
 */
static int
chat_phone_queue(const char *json, size_t len, const u_int8_t *data,
    size_t size)
{
	int		n;
	u_int64_t	ms;
	struct timespec	ts;
	int		ret;
	u_int32_t	have;
	size_t		total;
	char		at[32];
	u_int8_t	*entry;

	PRECOND(json != NULL);
	PRECOND(len > 1 && len <= VIGIL_COMPANION_JSON_MAX);
	PRECOND(json[len - 1] == '}');
	PRECOND(data != NULL || size == 0);
	PRECOND(companion_role == VIGIL_COMPANION_PHONE);

	(void)clock_gettime(CLOCK_REALTIME, &ts);
	ms = (u_int64_t)ts.tv_sec * 1000 + (ts.tv_nsec / 1000000);

	n = snprintf(at, sizeof(at), ",\"at\":%" PRIu64 "}", ms);
	if (n < 0 || (size_t)n >= sizeof(at))
		fatal("snprintf: the time of an event");

	len--;
	total = sizeof(have) + len + n + size;

	if ((entry = malloc(total)) == NULL)
		fatal("malloc: failed to allocate journal entry");

	have = htobe32(len + n);
	memcpy(entry, &have, sizeof(have));
	memcpy(entry + sizeof(have), json, len);
	memcpy(entry + sizeof(have) + len, at, n);

	if (size > 0)
		memcpy(entry + sizeof(have) + len + n, data, size);

	ret = 0;

	if (vigil_journal_append(&journal, entry, total) == 0) {
		vigil_log(LOG_NOTICE, "[companion] could not write to "
		    "our journal, our desktop needs a full resync");
		companion_dirty = 1;
		ret = -1;
	}

	vigil_atomic_write(&companion_journal, journal.bytes);

	nyfe_mem_zero(entry, total);
	free(entry);

	return (ret);
}

/*
 * Send our desktop the next entries of our journal once the parcel before
 * them was acked. Entries without data go together in one parcel for as
 * long as they fit, an entry with data goes on its own.
 */
static void
chat_phone_pump(void)
{
	u_int64_t		seq;
	u_int32_t		jlen;
	struct chat_blob	*blob;
	u_int64_t		first;
	u_int8_t		*entry, *body, *data;
	size_t			len, off, dlen, doff, count;

	PRECOND(companion_role == VIGIL_COMPANION_PHONE);

	if (companion.tunnel == NULL || !chat_tunnel_online(companion.tunnel))
		return;

	if (!journal_heard || !TAILQ_EMPTY(&companion.outbox))
		return;

	if (chat_companion_mismatch())
		return;

	if (journal_sent < journal.acked)
		journal_sent = journal.acked;

	if (journal_sent >= journal.last)
		return;

	if ((body = malloc(CHAT_COMPANION_BATCH)) == NULL)
		fatal("malloc: failed to allocate companion parcel");

	off = 0;
	dlen = 0;
	doff = 0;
	first = 0;
	count = 0;
	data = NULL;

	for (seq = journal_sent + 1; seq <= journal.last; seq++) {
		if ((entry = vigil_journal_read(&journal, seq, &len)) == NULL) {
			vigil_log(LOG_NOTICE,
			    "[companion] could not read journal entry %" PRIu64
			    ", our desktop needs a full resync", seq);
			journal_sent = seq;
			continue;
		}

		if (chat_phone_entry(entry, len, &jlen) == -1) {
			nyfe_mem_zero(entry, len);
			free(entry);
			journal_sent = seq;
			continue;
		}

		if (count > 0 && len > sizeof(jlen) + jlen) {
			nyfe_mem_zero(entry, len);
			free(entry);
			break;
		}

		if (count > 0 && off + CHAT_ENTRY_HEAD + jlen >
		    CHAT_COMPANION_BATCH) {
			nyfe_mem_zero(entry, len);
			free(entry);
			break;
		}

		if (count == 0)
			first = seq;

		chat_phone_entry_add(body, &off, seq, entry + sizeof(jlen),
		    jlen);

		count++;
		journal_sent = seq;

		if (len > sizeof(jlen) + jlen) {
			data = entry;
			dlen = len;
			doff = sizeof(jlen) + jlen;
			break;
		}

		nyfe_mem_zero(entry, len);
		free(entry);
	}

	if (count > 0) {
		blob = chat_companion_blob(CHAT_PARCEL_HEAD +
		    off + dlen - doff);
		blob->ref = journal_sent;

		len = sizeof(struct vigil_chat_data_head);

		jlen = htobe32(count);
		memcpy(blob->data + len, &jlen, sizeof(jlen));
		len += sizeof(jlen);

		jlen = htobe32(dlen - doff);
		memcpy(blob->data + len, &jlen, sizeof(jlen));
		len += sizeof(jlen);

		seq = htobe64(journal.last);
		memcpy(blob->data + len, &seq, sizeof(seq));
		len += sizeof(seq);

		memcpy(blob->data + len, body, off);
		len += off;

		if (data != NULL)
			memcpy(blob->data + len, data + doff, dlen - doff);

		chat_companion_push(blob);

		vigil_log(LOG_INFO, "[companion] sending entries %" PRIu64
		    " to %" PRIu64 " of %" PRIu64 " to our desktop, %zu bytes "
		    "of data", first, journal_sent, journal.last, dlen - doff);
	}

	if (data != NULL) {
		nyfe_mem_zero(data, dlen);
		free(data);
	}

	nyfe_mem_zero(body, CHAT_COMPANION_BATCH);
	free(body);
}

/*
 * Our desktop has the entries of our journal up to the given seq.
 */
static void
chat_phone_acked(u_int64_t seq)
{
	PRECOND(companion_role == VIGIL_COMPANION_PHONE);

	if (seq > journal.last) {
		vigil_log(LOG_NOTICE, "[companion] our journal is missing "
		    "entries, our desktop needs a full resync");
		companion_dirty = 1;
	}

	if (seq > journal.acked) {
		vigil_log(LOG_INFO, "[companion] our desktop has entries up "
		    "to %" PRIu64 " of %" PRIu64, seq, journal.last);
	}

	if (vigil_journal_ack(&journal, seq) == -1) {
		vigil_log(LOG_NOTICE, "[companion] could not save that "
		    "our desktop has up to %" PRIu64, seq);
	}

	vigil_atomic_write(&companion_journal, journal.bytes);
}

/*
 * Check the given journal entry and put the length of its JSON in jlen.
 */
static int
chat_phone_entry(const u_int8_t *entry, size_t len, u_int32_t *jlen)
{
	PRECOND(entry != NULL);
	PRECOND(jlen != NULL);

	if (len < sizeof(*jlen))
		return (-1);

	memcpy(jlen, entry, sizeof(*jlen));
	*jlen = be32toh(*jlen);

	if (*jlen == 0 || *jlen > len - sizeof(*jlen))
		return (-1);

	if (*jlen > CHAT_COMPANION_BATCH - CHAT_ENTRY_HEAD)
		return (-1);

	return (0);
}

/*
 * Append the given JSON as the entry with the given seq to body at off.
 */
static void
chat_phone_entry_add(u_int8_t *body, size_t *off, u_int64_t seq,
    const u_int8_t *json, u_int32_t len)
{
	u_int64_t	be64;
	u_int32_t	be32;

	PRECOND(body != NULL);
	PRECOND(off != NULL);
	PRECOND(json != NULL);
	PRECOND(*off + CHAT_ENTRY_HEAD + len <= CHAT_COMPANION_BATCH);

	be64 = htobe64(seq);
	memcpy(body + *off, &be64, sizeof(be64));
	*off += sizeof(be64);

	be32 = htobe32(len);
	memcpy(body + *off, &be32, sizeof(be32));
	*off += sizeof(be32);

	memcpy(body + *off, json, len);
	*off += len;
}

/*
 * Run the request our desktop sent. We ack a send or ask once our platform
 * stored what it did with it, so an ack means our journal holds the result.
 * Returns -1 if we cannot queue a send right now, our desktop then sends
 * the request again.
 */
static int
chat_phone_request(struct chat_tunnel *tun, struct chat_rxfer *xfer)
{
	struct vigil_chat_request	req;
	size_t				len;
	u_int64_t			token;
	int				ret;
	const u_int8_t			*data;

	PRECOND(tun != NULL);
	PRECOND(xfer != NULL);
	PRECOND(companion_role == VIGIL_COMPANION_PHONE);

	if (chat_companion_mismatch())
		return (-1);

	if (xfer->size < sizeof(struct vigil_chat_data_head) + sizeof(req)) {
		vigil_log(LOG_NOTICE, "[companion] request too short");
		chat_data_discard(tun, xfer);
		return (0);
	}

	memcpy(&req, xfer->data + sizeof(struct vigil_chat_data_head),
	    sizeof(req));

	data = xfer->data + sizeof(struct vigil_chat_data_head) + sizeof(req);
	len = xfer->size - sizeof(struct vigil_chat_data_head) - sizeof(req);

	req.ref = be64toh(req.ref);
	req.ask = be16toh(req.ask);
	req.flock = be64toh(req.flock);
	req.hash = be64toh(req.hash);
	req.topic = be64toh(req.topic);
	req.group = be64toh(req.group);
	req.serial = be32toh(req.serial);

	ret = 0;
	token = ++tokens;

	switch (req.op) {
	case VIGIL_COMPANION_SEND:
		ret = chat_phone_send(&req, data, len, token);
		break;
	case VIGIL_COMPANION_CALL:
		chat_phone_call(&req);
		break;
	case VIGIL_COMPANION_ASK:
		ret = chat_phone_ask(&req, data, len, token);
		break;
	default:
		vigil_log(LOG_NOTICE, "[companion] unknown request %u", req.op);
		break;
	}

	if (ret == -1)
		return (-1);

	if (ret == 1)
		chat_data_hold(tun, xfer, token);
	else
		chat_data_discard(tun, xfer);

	return (0);
}

/*
 * Returns -1 if the given send request of our desktop is bad, len is the
 * length of its text or data.
 */
static int
chat_phone_valid(const struct vigil_chat_request *req, size_t len)
{
	size_t		max;

	PRECOND(req != NULL);

	if (!(req->ref & VIGIL_CHAT_COMPANION_REF))
		return (-1);

	if (req->group != 0) {
		if (req->owner == 0 || req->owner >= KYRKA_PEERS_PER_FLOCK)
			return (-1);
	} else if (req->peer == 0 || req->peer >= KYRKA_PEERS_PER_FLOCK) {
		return (-1);
	}

	if (req->kind != 0) {
		if (req->kind == VIGIL_CHAT_DATA_AVATAR)
			return (-1);

		if (chat_data_kind_valid(req->kind) == -1)
			return (-1);

		if (req->len > sizeof(req->name))
			return (-1);

		if (chat_data_name_valid(req->name, req->len) == -1)
			return (-1);

		if (len == 0 || len > VIGIL_CHAT_DATA_MAX)
			return (-1);

		return (0);
	}

	if (req->group != 0 && req->topic != 0)
		max = VIGIL_CHAT_GROUP_TOPIC_MAX;
	else if (req->group != 0 && req->hash != 0)
		max = VIGIL_CHAT_GROUP_REPLY_MAX;
	else if (req->group != 0)
		max = VIGIL_CHAT_GROUP_MAX;
	else if (req->topic != 0)
		max = VIGIL_CHAT_TOPIC_MAX;
	else if (req->hash != 0)
		max = VIGIL_CHAT_REPLY_MAX;
	else
		max = VIGIL_CHAT_MESSAGE_MAX;

	if (len == 0 || len > max)
		return (-1);

	return (0);
}

/*
 * Queue the text or data of the given request for every peer the request
 * names, and hand our platform an echo event for the send under the given
 * token. Returns 1 if our platform got the echo, 0 if the request is bad
 * and -1 if we cannot take it right now.
 */
static int
chat_phone_send(const struct vigil_chat_request *req, const u_int8_t *data,
    size_t len, u_int64_t token)
{
	int			idx;
	int			ret;
	u_int8_t		slot;
	struct chat_event	*evt;
	u_int8_t		local;
	struct chat_quote	quote;
	struct chat_group	group;
	struct chat_topic	topic;
	u_int8_t		targets[VIGIL_CHAT_GROUP_ROSTER];
	char			name[VIGIL_CHAT_DATA_NAME_MAX + 1];

	PRECOND(req != NULL);
	PRECOND(data != NULL);
	PRECOND(req->op == VIGIL_COMPANION_SEND);

	if (chat_phone_valid(req, len) == -1) {
		vigil_log(LOG_NOTICE, "[companion] bad request %" PRIu64,
		    req->ref);
		return (0);
	}

	if (vigil_gospel_find(req->flock, &slot) == -1 ||
	    !gospels[slot].running) {
		vigil_log(LOG_NOTICE, "[companion] request %" PRIu64
		    " for flock %" PRIx64 " that does not run", req->ref,
		    req->flock);
		return (0);
	}

	local = gospels[slot].local;

	memset(&quote, 0, sizeof(quote));
	memset(&group, 0, sizeof(group));
	memset(&topic, 0, sizeof(topic));
	memset(name, 0, sizeof(name));

	topic.id = req->topic;
	quote.hash = req->hash;
	quote.author = req->author;

	if (req->group != 0) {
		group.id = req->group;
		group.owner = req->owner;
		group.serial = req->serial;
		group.type = VIGIL_CHAT_TYPE_TEXT;
	}

	if (req->group != 0) {
		memcpy(targets, req->members, sizeof(targets));
	} else {
		memset(targets, 0, sizeof(targets));
		targets[req->peer / 8] |= 1 << (req->peer % 8);
	}

	targets[0] &= ~1;
	targets[local / 8] &= ~(1 << (local % 8));

	memcpy(name, req->name, req->len);

	if (chat_events_room() == -1) {
		vigil_log(LOG_NOTICE, "[companion] no room to tell our "
		    "platform about request %" PRIu64 ", our desktop sends "
		    "it again", req->ref);
		return (-1);
	}

	for (idx = 1; idx < KYRKA_PEERS_PER_FLOCK; idx++) {
		if (!(targets[idx / 8] & (1 << (idx % 8))))
			continue;

		if (req->kind != 0) {
			ret = chat_data_queue(req->flock, idx, req->ref,
			    req->group != 0 ? &group : NULL, req->topic,
			    req->kind, name, data, len);
		} else {
			ret = chat_command_queue(CHAT_COMMAND_SEND, req->flock,
			    idx, req->ref, &quote, &topic,
			    req->group != 0 ? &group : NULL, data, len);
		}

		if (ret == -1) {
			vigil_log(LOG_NOTICE, "[companion] no room for request "
			    "%" PRIu64 ", our desktop sends it again",
			    req->ref);
			return (-1);
		}
	}

	evt = calloc(1, sizeof(*evt));
	if (evt == NULL)
		fatal("calloc: failed to allocate echo event");

	evt->token = token;
	evt->peer = req->peer;
	evt->flock = req->flock;
	evt->value = req->ref;
	evt->type = CHAT_EVENT_ECHO;

	memcpy(&evt->quote, &quote, sizeof(quote));
	memcpy(&evt->topic, &topic, sizeof(topic));
	memcpy(&evt->group, &group, sizeof(group));

	if (req->group != 0) {
		evt->peer = group.owner;
		memcpy(evt->group.members, targets, sizeof(targets));
	}

	if (req->kind == 0) {
		memcpy(evt->text, data, len);
	} else {
		evt->size = len;
		evt->kind = req->kind;
		memcpy(evt->text, name, req->len);
	}

	if (req->kind == 0)
		ret = chat_phone_tell(evt, NULL, 0);
	else
		ret = chat_phone_tell(evt, data, len);

	if (ret == -1)
		return (-1);

	return (1);
}

/*
 * Dial, answer, decline or hang up a call as our desktop asks. A call our
 * desktop dials or answers is held by our desktop, we relay its audio.
 */
static void
chat_phone_call(const struct vigil_chat_request *req)
{
	u_int64_t		flock;
	int			state;
	u_int8_t		slot;
	u_int8_t		peer, ringing, st, flags;

	PRECOND(req != NULL);
	PRECOND(req->op == VIGIL_COMPANION_CALL);

	peer = req->peer;
	flock = req->flock;
	if (vigil_gospel_find(flock, &slot) == -1 ||
	    peer == 0 || peer >= KYRKA_PEERS_PER_FLOCK ||
	    peer == (vigil->gospels[slot]->spi & 0xff)) {
		vigil_log(LOG_NOTICE, "[companion] bad call request");
		return;
	}

	switch (req->kind) {
	case VIGIL_COMPANION_DIAL:
		vigil_log(LOG_INFO, "[companion] calling %02x", peer);
		vigil_call_dial(flock, peer);
		vigil_voice_relay_call(flock, peer, VIGIL_VOICE_MODE_CALL);
		break;
	case VIGIL_COMPANION_ANSWER:
		vigil_log(LOG_INFO, "[companion] answering %02x", peer);
		if (vigil_voice_relay_answer(flock, peer) == 0)
			break;
		state = vigil_call_peer(slot, peer) & VIGIL_CALL_PEER_SIGNALING;
		if (state == VIGIL_CALL_PEER_PRESENT) {
			vigil_voice_relay_call(flock, peer,
			    VIGIL_VOICE_MODE_ANSWER);
		}
		break;
	case VIGIL_COMPANION_DECLINE:
		vigil_voice_relay_state(&flock, &ringing, &st, &flags);
		if (flock == req->flock && ringing == peer &&
		    (flags & VIGIL_COMPANION_CALL_RINGING))
			vigil_voice_hangup();
		break;
	case VIGIL_COMPANION_HANGUP:
		vigil_log(LOG_INFO, "[companion] hanging up on %02x", peer);
		vigil_voice_hangup();
		vigil_call_hangup();
		break;
	default:
		vigil_log(LOG_NOTICE, "[companion] unknown call request");
		break;
	}
}

/*
 * Hand our platform the ask in the given request under the given token.
 * Returns 1 if our platform got the ask, 0 if it is bad and -1 if there is
 * no room for it right now.
 */
static int
chat_phone_ask(const struct vigil_chat_request *req, const u_int8_t *data,
    size_t len, u_int64_t token)
{
	struct chat_event	*evt;

	PRECOND(req != NULL);
	PRECOND(data != NULL);
	PRECOND(req->op == VIGIL_COMPANION_ASK);

	if (req->ask == 0 || req->ask > VIGIL_CHAT_MESSAGE_MAX ||
	    req->ask > len) {
		vigil_log(LOG_NOTICE, "[companion] bad ask");
		return (0);
	}

	if (memchr(data, '\0', req->ask) != NULL) {
		vigil_log(LOG_NOTICE, "[companion] bad ask");
		return (0);
	}

	if (len - req->ask > VIGIL_CHAT_DATA_MAX) {
		vigil_log(LOG_NOTICE, "[companion] ask with too much data");
		return (0);
	}

	evt = calloc(1, sizeof(*evt));
	if (evt == NULL)
		fatal("calloc: failed to allocate desktop event");

	evt->token = token;
	evt->peer = companion.id;
	evt->size = len - req->ask;
	evt->type = CHAT_EVENT_DESKTOP;
	evt->kind = CHAT_DESKTOP_ASK;
	memcpy(evt->text, data, req->ask);

	if (chat_phone_tell(evt, data + req->ask, len - req->ask) == -1)
		return (-1);

	return (1);
}

/*
 * Queue the given event for our platform, with the given data for
 * vigil_chat_data_take() under the token of the event, if any. Returns -1
 * if there is no room.
 */
static int
chat_phone_tell(struct chat_event *evt, const u_int8_t *data, size_t len)
{
	struct chat_parcel	*parcel;

	PRECOND(evt != NULL);
	PRECOND(evt->token != 0);
	PRECOND(data != NULL || len == 0);

	parcel = NULL;

	if (len > 0) {
		parcel = calloc(1, sizeof(*parcel));
		if (parcel == NULL)
			fatal("calloc: failed to allocate echo parcel");

		parcel->data = malloc(len);
		if (parcel->data == NULL)
			fatal("malloc: failed to allocate echo parcel data");

		memcpy(parcel->data, data, len);

		parcel->size = len;
		parcel->age = vigil->now;
		parcel->token = evt->token;
	}

	if (chat_event_insert(evt) == -1) {
		if (parcel != NULL)
			chat_parcel_free(parcel);
		return (-1);
	}

	if (parcel != NULL)
		chat_parcel_hold(parcel);

	return (0);
}

/*
 * Apply the state our desktop sent, the peers our desktop types to and the
 * chats our desktop has open, as if our own platform set that state. If
 * our desktop moved to another flock, what it had in the old one is off.
 */
static void
chat_phone_desktop(const struct vigil_companion_desktop *st)
{
	int				idx;
	u_int64_t			flock;
	struct vigil_companion_desktop	was;
	int				on, before;

	PRECOND(st != NULL);
	PRECOND(companion_role == VIGIL_COMPANION_PHONE);

	if (!memcmp(st, &companion_desktop, sizeof(*st)))
		return;

	memcpy(&was, &companion_desktop, sizeof(was));

	flock = be64toh(was.flock);

	if (was.flock != st->flock && flock != 0) {
		for (idx = 1; idx < KYRKA_PEERS_PER_FLOCK; idx++) {
			if ((was.typing[idx / 8] >> (idx % 8)) & 1)
				vigil_chat_typing(flock, idx, 0, 0);

			if ((was.opened[idx / 8] >> (idx % 8)) & 1)
				vigil_chat_open(flock, idx, 0);
		}

		memset(was.typing, 0, sizeof(was.typing));
		memset(was.opened, 0, sizeof(was.opened));
	}

	flock = be64toh(st->flock);

	for (idx = 1; idx < KYRKA_PEERS_PER_FLOCK && flock != 0; idx++) {
		on = (st->typing[idx / 8] >> (idx % 8)) & 1;
		before = (was.typing[idx / 8] >> (idx % 8)) & 1;

		if (on != before || (on && st->topic != was.topic))
			vigil_chat_typing(flock, idx, be64toh(st->topic), on);

		on = (st->opened[idx / 8] >> (idx % 8)) & 1;
		before = (was.opened[idx / 8] >> (idx % 8)) & 1;

		if (on != before)
			vigil_chat_open(flock, idx, on);
	}

	if (st->flock != was.flock || st->group != was.group ||
	    st->owner != was.owner || st->topic != was.topic ||
	    memcmp(st->members, was.members, sizeof(st->members))) {
		if (st->group == 0 || st->owner == 0 ||
		    st->owner >= KYRKA_PEERS_PER_FLOCK) {
			vigil_chat_group_typing(0, 0, 0, 0, NULL);
		} else {
			vigil_chat_group_typing(flock, st->owner,
			    be64toh(st->group), be64toh(st->topic),
			    st->members);
		}
	}

	memcpy(&companion_desktop, st, sizeof(*st));
}

/*
 * Check if the state of any slot changed since we last sent it to our
 * desktop, our next heartbeat then carries the first one that did.
 * Returns -1 if one changed, 1 if a call rings or runs and 0 otherwise.
 */
static int
chat_phone_changed(void)
{
	int				idx, ringing;
	u_int8_t			slot;
	struct vigil_companion_phone	phone;

	PRECOND(companion_role == VIGIL_COMPANION_PHONE);

	ringing = 0;

	for (slot = 0; slot < VIGIL_GOSPEL_MAX; slot++) {
		if (!gospels[slot].running && companion_phones[slot].flock == 0)
			continue;

		chat_phone_state(slot, &phone);

		ringing |= phone.call_peer != 0;
		for (idx = 0; idx < VIGIL_CHAT_GROUP_ROSTER; idx++)
			ringing |= phone.incoming[idx];

		/* A new entry in our journal alone needs no heartbeat now. */
		phone.last = companion_phones[slot].last;

		if (memcmp(&phone, &companion_phones[slot], sizeof(phone))) {
			companion_turn = slot;
			return (-1);
		}
	}

	return (ringing);
}

/*
 * Fill in the state of the given slot we send our desktop with a heartbeat,
 * who is around and the call we run. A slot that no longer runs keeps its
 * flock with nobody around, so our desktop learns its peers are gone.
 */
static void
chat_phone_state(u_int8_t slot, struct vigil_companion_phone *st)
{
	int				idx;
	struct chat_tunnel		*tun;
	int				state;
	u_int64_t			flock;
	u_int64_t			count;
	struct chat_peer		*peer;
	struct chat_gospel		*gospel;
	struct vigil_companion_typed	*typed;

	PRECOND(slot < VIGIL_GOSPEL_MAX);
	PRECOND(st != NULL);
	PRECOND(companion_role == VIGIL_COMPANION_PHONE);

	gospel = &gospels[slot];

	memset(st, 0, sizeof(*st));
	st->magic = htobe16(VIGIL_COMPANION_MAGIC);
	st->version = htobe16(VIGIL_COMPANION_VERSION);
	st->last = htobe64(journal.last);
	st->flock = companion_phones[slot].flock;

	vigil_voice_relay_state(&flock, &st->call_peer, &st->call_state,
	    &st->call_flags);
	st->call_flock = htobe64(flock);

	vigil_voice_ended(&flock, &st->ended_peer, &st->ended_why, &count);
	st->ended_flock = htobe64(flock);
	st->ended = htobe64(count);

	if (!gospel->running)
		return;

	st->flock = htobe64(gospel->flock);

	for (idx = 1; idx < KYRKA_PEERS_PER_FLOCK; idx++) {
		peer = &gospel->peers[idx];
		tun = peer->tunnel;

		state = vigil_call_peer(slot, idx);
		if (peer->present || (state & VIGIL_CALL_PEER_ONLINE))
			st->present[idx / 8] |= 1 << (idx % 8);

		if ((state & VIGIL_CALL_PEER_SIGNALING) ==
		    VIGIL_CALL_PEER_PRESENT)
			st->incoming[idx / 8] |= 1 << (idx % 8);

		if (tun == NULL || !chat_tunnel_online(tun))
			continue;

		st->online[idx / 8] |= 1 << (idx % 8);

		if (tun->typing)
			st->typing[idx / 8] |= 1 << (idx % 8);

		if (tun->typing_group == 0 && tun->typing_topic == 0)
			continue;

		if (st->typed_count == VIGIL_COMPANION_TYPED)
			continue;

		typed = &st->typed[st->typed_count++];
		typed->peer = idx;
		typed->owner = tun->typing_owner;
		typed->group = htobe64(tun->typing_group);
		typed->topic = htobe64(tun->typing_topic);
	}
}

/*
 * Turn a command of our platform into a request for our phone.
 */
static void
chat_desktop_forward(struct chat_command *cmd)
{
	struct vigil_chat_request	req;

	PRECOND(cmd != NULL);
	PRECOND(companion_role == VIGIL_COMPANION_DESKTOP);

	memset(&req, 0, sizeof(req));

	switch (cmd->type) {
	case CHAT_COMMAND_CALL:
		req.peer = cmd->peer;
		req.kind = cmd->value;
		req.op = VIGIL_COMPANION_CALL;
		req.flock = htobe64(cmd->flock);
		chat_desktop_request(&req, NULL, 0);
		break;
	case CHAT_COMMAND_REQUEST:
		memcpy(&req, cmd->blob->data, sizeof(req));
		chat_desktop_request(&req, cmd->blob->data + cmd->len,
		    cmd->blob->size - cmd->len);
		break;
	default:
		fatal("chat command %d does not go via our phone", cmd->type);
	}
}

/*
 * Hand the event loop the given request in host byte order with the given
 * text, data or ask. Returns -1 if it cannot take it right now.
 */
static int
chat_desktop_queue(const struct vigil_chat_request *req, const void *data,
    size_t len)
{
	int				ret;
	struct vigil_chat_request	wire;

	PRECOND(req != NULL);
	PRECOND(data != NULL);
	PRECOND(len > 0);

	memcpy(&wire, req, sizeof(wire));

	wire.ref = htobe64(req->ref);
	wire.ask = htobe16(req->ask);
	wire.flock = htobe64(req->flock);
	wire.hash = htobe64(req->hash);
	wire.topic = htobe64(req->topic);
	wire.group = htobe64(req->group);
	wire.serial = htobe32(req->serial);

	ret = chat_command_blob(CHAT_COMMAND_REQUEST, 0,
	    (const char *)&wire, sizeof(wire), data, len);

	return (ret);
}

/*
 * Put the given call on the outbox for our phone, or hold the given send
 * or ask with its text or data for our phone until it has it. We tell our
 * platform when we can do neither.
 */
static void
chat_desktop_request(const struct vigil_chat_request *req,
    const u_int8_t *data, size_t len)
{
	size_t			off;
	struct chat_blob	*blob;

	PRECOND(req != NULL);
	PRECOND(data != NULL || len == 0);
	PRECOND(companion_role == VIGIL_COMPANION_DESKTOP);

	if (req->op != VIGIL_COMPANION_CALL) {
		if (chat_desktop_hold(req, data, len) == -1)
			chat_desktop_unsent(req);
		return;
	}

	if (chat_companion_room(sizeof(*req) + len) == -1) {
		chat_desktop_unsent(req);
		return;
	}

	blob = chat_companion_blob(sizeof(*req) + len);

	off = sizeof(struct vigil_chat_data_head);
	memcpy(blob->data + off, req, sizeof(*req));

	if (len > 0)
		memcpy(blob->data + off + sizeof(*req), data, len);

	chat_companion_push(blob);
}

/*
 * Write the given request with its text or data to our requests on disk.
 * Returns -1 if we have no room for it or could not write it.
 */
static int
chat_desktop_hold(const struct vigil_chat_request *req,
    const u_int8_t *data, size_t len)
{
	u_int64_t	seq;
	size_t		total;
	u_int8_t	*entry;

	PRECOND(req != NULL);
	PRECOND(data != NULL || len == 0);

	if (!requests.sealed) {
		vigil_log(LOG_NOTICE, "[companion] we keep no requests for "
		    "our phone, a request did not go out");
		return (-1);
	}

	total = sizeof(*req) + len;

	if (requests.bytes + total > CHAT_JOURNAL_MAX) {
		vigil_log(LOG_NOTICE, "[companion] no room for another "
		    "request for our phone, it did not go out");
		return (-1);
	}

	if ((entry = malloc(total)) == NULL)
		fatal("malloc: failed to allocate request");

	memcpy(entry, req, sizeof(*req));

	if (len > 0)
		memcpy(entry + sizeof(*req), data, len);

	seq = vigil_journal_append(&requests, entry, total);

	nyfe_mem_zero(entry, total);
	free(entry);

	if (seq == 0)
		return (-1);

	return (0);
}

/*
 * Send our phone the next request we hold once it acked the one before.
 * After we lost our phone we first take all it wrote to its journal, so
 * we know if it already did what our last request asked.
 */
static void
chat_desktop_pump(void)
{
	size_t				len;
	u_int64_t			seq;
	u_int64_t			last;
	struct chat_msg			*msg;
	struct chat_blob		*blob;
	u_int8_t			*entry;
	struct vigil_chat_request	req;

	PRECOND(companion_role == VIGIL_COMPANION_DESKTOP);

	if (!requests.sealed)
		return;

	if (companion.tunnel == NULL ||
	    !chat_tunnel_online(companion.tunnel)) {
		chat_desktop_recall();
		return;
	}

	if (chat_companion_mismatch())
		return;

	TAILQ_FOREACH(msg, &companion.outbox, wlist) {
		if (msg->ref & CHAT_REQUEST_REF)
			return;
	}

	if (companion_phone.magic == 0)
		return;

	last = be64toh(companion_phone.last);
	if (companion_last > last)
		last = companion_last;

	if (journal.acked < last)
		return;

	if (requests_sent < requests.acked)
		requests_sent = requests.acked;

	for (seq = requests_sent + 1; seq <= requests.last; seq++) {
		requests_sent = seq;

		entry = vigil_journal_read(&requests, seq, &len);
		if (entry == NULL) {
			vigil_log(LOG_NOTICE, "[companion] could not read "
			    "request %" PRIu64 ", it does not go out", seq);
			chat_desktop_acked(seq);
			continue;
		}

		if (len < sizeof(req)) {
			nyfe_mem_zero(entry, len);
			free(entry);
			chat_desktop_acked(seq);
			continue;
		}

		memcpy(&req, entry, sizeof(req));

		requests_head = seq;
		requests_ref = be64toh(req.ref);

		blob = chat_companion_blob(len);
		blob->ref = seq | CHAT_REQUEST_REF;
		memcpy(blob->data + sizeof(struct vigil_chat_data_head),
		    entry, len);

		nyfe_mem_zero(entry, len);
		free(entry);

		chat_companion_push(blob);
		break;
	}
}

/*
 * Remember the seq and ref of the first request our phone did not ack, it
 * is the one our phone may have done without us knowing.
 */
static void
chat_desktop_head(void)
{
	size_t				len;
	u_int8_t			*entry;
	struct vigil_chat_request	req;

	PRECOND(companion_role == VIGIL_COMPANION_DESKTOP);

	requests_head = 0;
	requests_ref = 0;

	if (!requests.sealed || requests.acked >= requests.last)
		return;

	entry = vigil_journal_read(&requests, requests.acked + 1, &len);
	if (entry == NULL)
		return;

	if (len >= sizeof(req)) {
		memcpy(&req, entry, sizeof(req));
		requests_head = requests.acked + 1;
		requests_ref = be64toh(req.ref);
	}

	nyfe_mem_zero(entry, len);
	free(entry);
}

/*
 * Take the request we sent our phone off our outbox, we send it again
 * once we know our phone did not do it yet.
 */
static void
chat_desktop_recall(void)
{
	struct chat_tunnel	*tun;
	struct chat_msg		*msg, *next;

	PRECOND(companion_role == VIGIL_COMPANION_DESKTOP);

	tun = companion.tunnel;

	for (msg = TAILQ_FIRST(&companion.outbox); msg != NULL; msg = next) {
		next = TAILQ_NEXT(msg, wlist);

		if (!(msg->ref & CHAT_REQUEST_REF))
			continue;

		if (tun != NULL && msg->pending)
			TAILQ_REMOVE(&tun->sendq, msg, slist);

		if (tun != NULL && tun->xfer != NULL && tun->xfer->msg == msg)
			chat_xfer_free(tun);

		TAILQ_REMOVE(&companion.outbox, msg, wlist);
		chat_msg_free(msg);
	}

	requests_sent = requests.acked;
}

/*
 * Returns 1 if the given JSON entry of our phone is the record of what it
 * did with the request we sent it last: a record under the ref of a send,
 * or the answer to an ask under its id.
 */
static int
chat_desktop_done(const u_int8_t *json, size_t len)
{
	int		ret;
	const u_int8_t	*at;
	char		ref[48], id[48];

	PRECOND(json != NULL);

	if (requests_head == 0)
		return (0);

	ret = snprintf(ref, sizeof(ref), "\"ref\":%" PRIu64, requests_ref);
	if (ret < 0 || (size_t)ret >= sizeof(ref))
		fatal("snprintf: the ref of a request");

	if ((at = memmem(json, len, ref, ret)) != NULL &&
	    (at + ret == json + len || !isdigit(at[ret])))
		return (1);

	ret = snprintf(id, sizeof(id), "\"id\":%" PRIu64, requests_ref);
	if (ret < 0 || (size_t)ret >= sizeof(id))
		fatal("snprintf: the id of an ask");

	if ((at = memmem(json, len, id, ret)) != NULL &&
	    (at + ret == json + len || !isdigit(at[ret])))
		return (1);

	return (0);
}

/*
 * Our phone has our requests up to the given seq.
 */
static void
chat_desktop_acked(u_int64_t seq)
{
	PRECOND(companion_role == VIGIL_COMPANION_DESKTOP);

	if (seq >= requests_head) {
		requests_head = 0;
		requests_ref = 0;
	}

	if (vigil_journal_ack(&requests, seq) == -1) {
		vigil_log(LOG_NOTICE, "[companion] could not save that our "
		    "phone has our requests up to %" PRIu64, seq);
	}
}

/*
 * Tell our platform about the sends we held for our phone when we started,
 * as room for events allows, so it shows them as waiting.
 */
static void
chat_desktop_told(void)
{
	size_t				len;
	u_int64_t			seq;
	struct vigil_chat_request	req;
	u_int8_t			*entry;

	PRECOND(companion_role == VIGIL_COMPANION_DESKTOP);

	while (requests_told < requests_held) {
		if (chat_events_room() == -1)
			return;

		seq = ++requests_told;

		if (seq <= requests.acked)
			continue;

		if ((entry = vigil_journal_read(&requests, seq, &len)) == NULL)
			continue;

		if (len > sizeof(req)) {
			memcpy(&req, entry, sizeof(req));

			if (req.op == VIGIL_COMPANION_SEND) {
				chat_desktop_queued(&req, entry + sizeof(req),
				    len - sizeof(req));
			}
		}

		nyfe_mem_zero(entry, len);
		free(entry);
	}
}

/*
 * Tell our platform about the given send we hold for our phone.
 */
static void
chat_desktop_queued(const struct vigil_chat_request *req,
    const u_int8_t *data, size_t len)
{
	struct chat_event	*evt;

	PRECOND(req != NULL);
	PRECOND(data != NULL);
	PRECOND(req->op == VIGIL_COMPANION_SEND);

	evt = calloc(1, sizeof(*evt));
	if (evt == NULL)
		fatal("calloc: failed to allocate desktop event");

	evt->type = CHAT_EVENT_DESKTOP;
	evt->kind = CHAT_DESKTOP_QUEUED;

	evt->peer = req->peer;
	evt->flock = be64toh(req->flock);
	evt->value = be64toh(req->ref);
	evt->topic.id = be64toh(req->topic);
	evt->quote.author = req->author;
	evt->quote.hash = be64toh(req->hash);

	if (req->group != 0) {
		evt->peer = req->owner;
		evt->group.owner = req->owner;
		evt->group.id = be64toh(req->group);
		evt->group.serial = be32toh(req->serial);
	}

	if (req->kind != 0) {
		evt->size = len;
		memcpy(evt->text, req->name, req->len);
		(void)snprintf(evt->name, sizeof(evt->name), "%s",
		    chat_data_kind_name(req->kind));
	} else if (len < sizeof(evt->text)) {
		memcpy(evt->text, data, len);
	}

	(void)chat_event_insert(evt);
}

/*
 * Tell our platform the given request never went to our phone, a send
 * names its chat and ref.
 */
static void
chat_desktop_unsent(const struct vigil_chat_request *req)
{
	struct chat_event	*evt;

	PRECOND(req != NULL);
	PRECOND(companion_role == VIGIL_COMPANION_DESKTOP);

	evt = calloc(1, sizeof(*evt));
	if (evt == NULL)
		fatal("calloc: failed to allocate desktop event");

	evt->type = CHAT_EVENT_DESKTOP;
	evt->kind = CHAT_DESKTOP_UNSENT;

	if (req->op == VIGIL_COMPANION_SEND) {
		evt->peer = req->peer;
		evt->flock = be64toh(req->flock);
		evt->value = be64toh(req->ref);

		if (req->group != 0) {
			evt->peer = req->owner;
			evt->group.owner = req->owner;
			evt->group.id = be64toh(req->group);
			evt->group.serial = be32toh(req->serial);
		}
	}

	(void)chat_event_insert(evt);
}

/*
 * As a desktop, set or clear the bit of the given peer in the given flock
 * in the given bits for our phone. A bit in another flock clears what we
 * had, our phone only learns about one flock at a time.
 */
static void
chat_desktop_bit(u_int8_t *bits, u_int64_t flock, u_int8_t peer, int on)
{
	PRECOND(bits == desktop_opened || bits == desktop_typing);
	PRECOND(peer > 0 && peer < KYRKA_PEERS_PER_FLOCK);
	PRECOND(on == 0 || on == 1);

	vigil_lock(&lock);

	if (flock != desktop_flock) {
		if (on == 0) {
			vigil_unlock(&lock);
			return;
		}

		desktop_flock = flock;
		memset(desktop_opened, 0, sizeof(desktop_opened));
		memset(desktop_typing, 0, sizeof(desktop_typing));
	}

	if (on)
		bits[peer / 8] |= 1 << (peer % 8);
	else
		bits[peer / 8] &= ~(1 << (peer % 8));

	vigil_unlock(&lock);
}

/*
 * Fill in the state we send our phone with each heartbeat, who we type to
 * and the chats we have open.
 */
static void
chat_desktop_state(struct vigil_companion_desktop *st)
{
	PRECOND(st != NULL);
	PRECOND(companion_role == VIGIL_COMPANION_DESKTOP);

	memset(st, 0, sizeof(*st));
	st->magic = htobe16(VIGIL_COMPANION_MAGIC);
	st->version = htobe16(VIGIL_COMPANION_VERSION);

	vigil_lock(&lock);

	st->flock = htobe64(desktop_flock);
	memcpy(st->typing, desktop_typing, sizeof(st->typing));
	memcpy(st->opened, desktop_opened, sizeof(st->opened));

	/* Our phone only knows one flock, the group we type in wins. */
	if (typing_group != 0 && typing_flock != desktop_flock) {
		st->flock = htobe64(typing_flock);
		memset(st->typing, 0, sizeof(st->typing));
		memset(st->opened, 0, sizeof(st->opened));
	}

	st->owner = typing_owner;
	st->group = htobe64(typing_group);
	st->topic = htobe64(typing_topic);
	memcpy(st->members, typing_members, sizeof(st->members));

	vigil_unlock(&lock);

	st->have = htobe64(journal.acked);
	st->synced = journal.synced;
}

/*
 * Take the state our phone sent us for one of its flocks, a state without
 * a flock means we lost our phone. Hold the call our phone runs if it is
 * ours, or ring for the call that rings on our phone.
 */
static void
chat_desktop_phone(const struct vigil_companion_phone *st)
{
	int		idx;
	u_int8_t	slot;
	u_int64_t	flock;
	u_int8_t	ring;
	int		sound;

	PRECOND(st != NULL);
	PRECOND(companion_role == VIGIL_COMPANION_DESKTOP);

	memcpy(&companion_phone, st, sizeof(*st));

	if (st->flock == 0) {
		memset(companion_phones, 0, sizeof(companion_phones));
	} else {
		slot = VIGIL_GOSPEL_MAX;

		for (idx = 0; idx < VIGIL_GOSPEL_MAX; idx++) {
			if (companion_phones[idx].flock == st->flock) {
				slot = idx;
				break;
			}

			if (slot == VIGIL_GOSPEL_MAX &&
			    companion_phones[idx].flock == 0)
				slot = idx;
		}

		if (slot < VIGIL_GOSPEL_MAX)
			memcpy(&companion_phones[slot], st, sizeof(*st));
	}

	flock = be64toh(st->call_flock);

	if (st->call_peer != 0 &&
	    (st->call_flags & VIGIL_COMPANION_CALL_RELAY)) {
		if (st->call_state == VIGIL_VOICE_STATE_TALKING)
			sound = VIGIL_VOICE_TONE_NONE;
		else if (st->call_flags & VIGIL_COMPANION_CALL_OUTGOING)
			sound = VIGIL_VOICE_TONE_RINGBACK;
		else
			sound = VIGIL_VOICE_TONE_PIPS;

		vigil_voice_relay(flock, st->call_peer, sound, 1);
		return;
	}

	ring = 0;

	if (st->call_peer != 0) {
		if (st->call_flags & VIGIL_COMPANION_CALL_RINGING)
			ring = st->call_peer;
	} else {
		flock = 0;

		for (slot = 0; slot < VIGIL_GOSPEL_MAX && ring == 0; slot++) {
			for (idx = 1; idx < KYRKA_PEERS_PER_FLOCK; idx++) {
				if (companion_phones[slot].incoming[idx / 8] &
				    (1 << (idx % 8))) {
					ring = idx;
					flock = be64toh(
					    companion_phones[slot].flock);
					break;
				}
			}
		}
	}

	vigil_voice_relay(flock, ring, VIGIL_VOICE_TONE_RINGBACK, 0);
}

/*
 * Append our state to the heartbeat for our companion in the given frame.
 */
static void
chat_companion_heartbeat(struct chat_tunnel *tun,
    struct vigil_chat_frame *frame)
{
	int		idx;
	u_int8_t	slot;
	u_int8_t	*ptr;

	PRECOND(tun != NULL);
	PRECOND(tun->peer == &companion);
	PRECOND(frame != NULL);

	VERIFY(sizeof(struct vigil_chat_hb) +
	    sizeof(struct vigil_companion_phone) <= sizeof(frame->data));
	VERIFY(sizeof(struct vigil_chat_hb) +
	    sizeof(struct vigil_companion_desktop) <= sizeof(frame->data));

	ptr = frame->data + sizeof(struct vigil_chat_hb);

	if (companion_role == VIGIL_COMPANION_PHONE) {
		slot = companion_turn;
		chat_phone_state(slot, &companion_phones[slot]);

		memcpy(&companion_phone, &companion_phones[slot],
		    sizeof(companion_phone));
		memcpy(ptr, &companion_phone, sizeof(companion_phone));

		if (!gospels[slot].running)
			companion_phones[slot].flock = 0;

		for (idx = 1; idx <= VIGIL_GOSPEL_MAX; idx++) {
			slot = (companion_turn + idx) % VIGIL_GOSPEL_MAX;

			if (gospels[slot].running ||
			    companion_phones[slot].flock != 0)
				break;
		}

		companion_turn = slot;
	} else {
		chat_desktop_state(&companion_desktop);
		memcpy(ptr, &companion_desktop, sizeof(companion_desktop));
	}
}

/*
 * Apply the state our companion appended to the heartbeat in the given
 * frame.
 */
static void
chat_companion_hb_recv(struct chat_tunnel *tun,
    const struct vigil_chat_frame *frame)
{
	const u_int8_t			*ptr;
	struct vigil_companion_phone	phone;
	struct vigil_companion_desktop	desktop;

	PRECOND(tun != NULL);
	PRECOND(tun->peer == &companion);
	PRECOND(frame != NULL);

	ptr = frame->data + sizeof(struct vigil_chat_hb);

	chat_companion_version(ptr);

	/* Our phone paces the heartbeats, we answer each one. */
	if (companion_role == VIGIL_COMPANION_DESKTOP)
		tun->next_heartbeat = vigil->now;

	if (chat_companion_mismatch())
		return;

	if (companion_role == VIGIL_COMPANION_PHONE) {
		memcpy(&desktop, ptr, sizeof(desktop));
		chat_phone_desktop(&desktop);
		chat_phone_acked(be64toh(desktop.have));

		if (!journal_heard)
			tun->next_heartbeat = vigil->now;

		journal_heard = 1;
	} else {
		memcpy(&phone, ptr, sizeof(phone));
		if (phone.typed_count > VIGIL_COMPANION_TYPED)
			phone.typed_count = VIGIL_COMPANION_TYPED;
		chat_desktop_phone(&phone);
	}
}

/*
 * Take the sync version of our companion from the start of its state.
 * A companion without our magic runs a build from before we had one.
 */
static void
chat_companion_version(const u_int8_t *ptr)
{
	u_int16_t	magic, version;

	PRECOND(ptr != NULL);

	memcpy(&magic, ptr, sizeof(magic));
	memcpy(&version, ptr + sizeof(magic), sizeof(version));

	version = be16toh(version);

	if (be16toh(magic) != VIGIL_COMPANION_MAGIC)
		version = 0;

	if (companion_known && version == companion_other)
		return;

	companion_known = 1;
	companion_other = version;

	if (version == VIGIL_COMPANION_VERSION)
		return;

	vigil_log(LOG_NOTICE, "[companion] our %s runs sync version %u and "
	    "we run %u, we do not sync with it",
	    companion_role == VIGIL_COMPANION_PHONE ? "desktop" : "phone",
	    version, VIGIL_COMPANION_VERSION);
}

/*
 * Returns 1 if our companion does not run our sync version, or we did not
 * hear its version yet.
 */
static int
chat_companion_mismatch(void)
{
	return (companion_other != VIGIL_COMPANION_VERSION);
}

/*
 * Append the peers of all flocks of our phone, who types where, who calls
 * us, the last call that ended and the call our phone runs to the status.
 */
static void
chat_companion_peers(struct vigil_buf *buf)
{
	u_int8_t				bit;
	u_int8_t				flags;
	u_int64_t				flock;
	const struct vigil_companion_phone	*st;
	const struct vigil_companion_typed	*typed;
	int					idx, first;
	u_int8_t				slot;
	int					present, online, writing;

	PRECOND(buf != NULL);

	first = 1;
	vigil_buf_appendf(buf, ",\"peers\":[");

	for (slot = 0; slot < VIGIL_GOSPEL_MAX; slot++) {
		st = &companion_phones[slot];
		flock = be64toh(st->flock);

		if (flock == 0)
			continue;

		for (idx = 1; idx < KYRKA_PEERS_PER_FLOCK; idx++) {
			bit = 1 << (idx % 8);
			online = st->online[idx / 8] & bit;
			writing = st->typing[idx / 8] & bit;
			present = st->present[idx / 8] & bit;

			if (!present && !online)
				continue;

			vigil_buf_appendf(buf, "%s{\"id\":\"%" PRIx64 "-%02x\""
			    ",\"present\":%s,\"online\":%s,\"typing\":%s}",
			    first ? "" : ",", flock, idx,
			    present ? "true" : "false",
			    online ? "true" : "false",
			    writing ? "true" : "false");

			first = 0;
		}
	}

	first = 1;
	vigil_buf_appendf(buf, "],\"typed\":[");

	for (slot = 0; slot < VIGIL_GOSPEL_MAX; slot++) {
		st = &companion_phones[slot];
		flock = be64toh(st->flock);

		if (flock == 0)
			continue;

		for (idx = 0; idx < st->typed_count; idx++) {
			typed = &st->typed[idx];

			vigil_buf_appendf(buf, "%s{\"id\":\"%" PRIx64 "-%02x\"",
			    first ? "" : ",", flock, typed->peer);

			if (typed->group != 0) {
				vigil_buf_appendf(buf, ",\"owner\":\"%02x\""
				    ",\"group\":\"%016" PRIx64 "\"",
				    typed->owner, be64toh(typed->group));
			}

			if (typed->topic != 0) {
				vigil_buf_appendf(buf,
				    ",\"topic\":\"%016" PRIx64 "\"",
				    be64toh(typed->topic));
			}

			vigil_buf_appendf(buf, "}");
			first = 0;
		}
	}

	first = 1;
	vigil_buf_appendf(buf, "],\"incoming\":[");

	for (slot = 0; slot < VIGIL_GOSPEL_MAX; slot++) {
		st = &companion_phones[slot];
		flock = be64toh(st->flock);

		if (flock == 0)
			continue;

		for (idx = 1; idx < KYRKA_PEERS_PER_FLOCK; idx++) {
			if (!(st->incoming[idx / 8] & (1 << (idx % 8))))
				continue;

			vigil_buf_appendf(buf, "%s\"%" PRIx64 "-%02x\"",
			    first ? "" : ",", flock, idx);
			first = 0;
		}
	}

	st = &companion_phone;

	vigil_buf_appendf(buf, "],\"ended\":{\"count\":%" PRIu64
	    ",\"peer\":\"%" PRIx64 "-%02x\",\"why\":\"%s\"}",
	    be64toh(st->ended), be64toh(st->ended_flock), st->ended_peer,
	    vigil_voice_end_name(st->ended_why));

	if (st->call_peer == 0)
		return;

	flags = st->call_flags;

	vigil_buf_appendf(buf, ",\"call\":{\"peer\":\"%" PRIx64 "-%02x\""
	    ",\"state\":%u,\"relay\":%s,\"ringing\":%s,\"outgoing\":%s}",
	    be64toh(st->call_flock), st->call_peer, st->call_state,
	    (flags & VIGIL_COMPANION_CALL_RELAY) ? "true" : "false",
	    (flags & VIGIL_COMPANION_CALL_RINGING) ? "true" : "false",
	    (flags & VIGIL_COMPANION_CALL_OUTGOING) ? "true" : "false");
}

/*
 * Send the given voice frame to our companion, for the call our desktop
 * holds.
 */
void
vigil_chat_companion_voice(const u_int8_t *data, size_t len)
{
	size_t			max;
	struct kyrka_packet	pkt;
	struct chat_tunnel	*tun;
	u_int8_t		*ptr;

	PRECOND(data != NULL);
	PRECOND(len == VIGIL_VOICE_PAYLOAD);

	tun = companion.tunnel;

	if (tun == NULL || !chat_tunnel_online(tun))
		return;

	ptr = kyrka_packet_databuf(tun->ctx, &pkt, &max);
	if (ptr == NULL)
		fatal("failed to get chat data buffer");

	if (len > max)
		fatal("voice frame does not fit");

	memcpy(ptr, data, len);
	pkt.length = len;

	if (vigil_inet_match(&tun->addr, &tun->cathedral.addr))
		pkt.shroud = KYRKA_PACKET_SHROUD_CATHEDRAL;
	else
		pkt.shroud = KYRKA_PACKET_SHROUD_PEER;

	if (kyrka_heaven_input(tun->ctx, &pkt) == -1 &&
	    kyrka_last_error(tun->ctx) != KYRKA_ERROR_NO_TX_KEY) {
		vigil_log(LOG_NOTICE, "[companion] kyrka_heaven_input: %d",
		    kyrka_last_error(tun->ctx));
	}
}

/*
 * Hand our platform the entries of a journal parcel from our phone that we
 * did not store yet, with the data of the parcel, if any. A gap before the
 * entries means we lost some and are no longer synced, the marks around a
 * full copy of the chats of our phone make us synced again.
 */
static int
chat_desktop_mirror(struct chat_tunnel *tun, struct chat_rxfer *xfer)
{
	u_int64_t		seq;
	struct chat_event	*evt;
	struct chat_seen	*seen;
	u_int64_t		last;
	u_int64_t		token;
	u_int64_t		first;
	u_int8_t		*json;
	u_int64_t		newest;
	u_int8_t		synced;
	struct chat_parcel	*parcel;
	size_t			off, len;
	u_int32_t		count, size, idx, jlen;

	PRECOND(tun != NULL);
	PRECOND(xfer != NULL);
	PRECOND(companion_role == VIGIL_COMPANION_DESKTOP);

	if (chat_companion_mismatch())
		return (-1);

	off = sizeof(struct vigil_chat_data_head);

	if (xfer->size < off + CHAT_PARCEL_HEAD) {
		chat_data_discard(tun, xfer);
		return (0);
	}

	memcpy(&count, xfer->data + off, sizeof(count));
	count = be32toh(count);
	off += sizeof(count);

	memcpy(&size, xfer->data + off, sizeof(size));
	size = be32toh(size);
	off += sizeof(size);

	memcpy(&last, xfer->data + off, sizeof(last));
	last = be64toh(last);
	off += sizeof(last);

	if (count == 0 || size > xfer->size - off) {
		vigil_log(LOG_NOTICE, "[companion] bad parcel from our phone");
		chat_data_discard(tun, xfer);
		return (0);
	}

	vigil_lock(&lock);

	if ((parcel_bytes + xfer->size) > CHAT_PARCELS_MAX) {
		vigil_unlock(&lock);
		vigil_log(LOG_NOTICE, "[companion] no room for a parcel from "
		    "our phone, it sends it again");
		return (-1);
	}

	vigil_unlock(&lock);

	if ((json = malloc(CHAT_COMPANION_BATCH)) == NULL)
		fatal("malloc: failed to allocate mirror events");

	len = 0;
	first = 0;
	newest = 0;
	synced = journal.synced;

	for (idx = 0; idx < count; idx++) {
		if (xfer->size - size - off < CHAT_ENTRY_HEAD)
			break;

		memcpy(&seq, xfer->data + off, sizeof(seq));
		seq = be64toh(seq);
		off += sizeof(seq);

		memcpy(&jlen, xfer->data + off, sizeof(jlen));
		jlen = be32toh(jlen);
		off += sizeof(jlen);

		if (jlen == 0 || jlen > xfer->size - size - off)
			break;

		if (len + jlen + 1 > CHAT_COMPANION_BATCH)
			break;

		if (seq > journal.acked && seq > newest) {
			if (newest == 0 && seq != journal.acked + 1) {
				vigil_log(LOG_NOTICE, "[companion] entries %"
				    PRIu64 " to %" PRIu64 " never reached us",
				    journal.acked + 1, seq - 1);
				synced = VIGIL_JOURNAL_UNSYNCED;
			}

			if (newest == 0)
				first = seq;

			synced = chat_desktop_mark(synced,
			    xfer->data + off, jlen);

			if (chat_desktop_done(xfer->data + off, jlen))
				requests_done = requests_head;

			if (len > 0)
				json[len++] = ',';
			memcpy(json + len, xfer->data + off, jlen);
			len += jlen;
			newest = seq;
		}

		off += jlen;
	}

	if (idx != count || off + size != xfer->size) {
		vigil_log(LOG_NOTICE, "[companion] bad parcel from our phone");
		nyfe_mem_zero(json, CHAT_COMPANION_BATCH);
		free(json);
		chat_data_discard(tun, xfer);
		return (0);
	}

	if (newest == 0) {
		nyfe_mem_zero(json, CHAT_COMPANION_BATCH);
		free(json);
		chat_data_discard(tun, xfer);
		return (0);
	}

	companion_last = last;

	vigil_log(LOG_INFO, "[companion] got entries %" PRIu64 "-%" PRIu64
	    "/%" PRIu64 " from our phone, %u bytes of data", first, newest,
	    companion_last, size);

	evt = calloc(1, sizeof(*evt));
	if (evt == NULL)
		fatal("calloc: failed to allocate mirror event");

	evt->json = json;
	evt->json_len = len;
	evt->value = ++tokens;
	evt->peer = companion.id;
	evt->type = CHAT_EVENT_MIRROR;
	evt->size = size;

	parcel = NULL;
	token = evt->value;

	if (size > 0) {
		parcel = calloc(1, sizeof(*parcel));
		if (parcel == NULL)
			fatal("calloc: failed to allocate mirror data");

		parcel->off = off;
		parcel->data = xfer->data;
		parcel->size = xfer->size;
		parcel->age = vigil->now;
		parcel->token = token;

		xfer->data = NULL;
	}

	if (chat_event_insert(evt) == -1) {
		if (parcel != NULL)
			chat_parcel_free(parcel);
		return (-1);
	}

	if (parcel != NULL)
		chat_parcel_hold(parcel);

	seen = &tun->seen[tun->seen_next++ % CHAT_SEEN_MAX];

	seen->hash = 0;
	seen->avatar = 0;
	seen->id = xfer->id;
	seen->token = token;
	seen->synced = synced;
	seen->journal = newest;
	seen->age = vigil->now;
	seen->size = xfer->size;

	return (0);
}

/*
 * Forget that we are synced, our phone then sends us a full resync.
 */
static void
chat_desktop_unsync(void)
{
	if (companion_role != VIGIL_COMPANION_DESKTOP)
		return;

	vigil_log(LOG_INFO, "[companion] asking our phone for a full resync");

	if (vigil_journal_synced(&journal, VIGIL_JOURNAL_UNSYNCED) == -1) {
		vigil_log(LOG_NOTICE,
		    "[companion] could not save if we are synced");
	}
}

/*
 * Returns if we are synced after the given entry from our phone, given if
 * we were synced before it.
 */
static u_int8_t
chat_desktop_mark(u_int8_t synced, const u_int8_t *json, size_t len)
{
	PRECOND(json != NULL);

	if (chat_desktop_is(json, len, CHAT_SYNC_RESET_JSON))
		return (VIGIL_JOURNAL_SYNCING);

	if (synced == VIGIL_JOURNAL_SYNCING &&
	    chat_desktop_is(json, len, CHAT_SYNC_DONE_JSON))
		return (VIGIL_JOURNAL_SYNCED);

	return (synced);
}

/*
 * Returns 1 if the given entry from our phone is the given mark, our phone
 * only adds the time it wrote the entry to it.
 */
static int
chat_desktop_is(const u_int8_t *json, size_t len, const char *mark)
{
	size_t		mlen;

	PRECOND(json != NULL);
	PRECOND(mark != NULL);

	mlen = strlen(mark) - 1;

	if (len <= mlen)
		return (0);

	if (memcmp(json, mark, mlen) || json[mlen] != ',')
		return (0);

	return (1);
}

/*
 * Our platform stored the entries of our phone up to the given seq, after
 * which we are synced or not.
 */
static void
chat_desktop_stored(u_int64_t seq, u_int8_t synced)
{
	PRECOND(companion_role == VIGIL_COMPANION_DESKTOP);

	/* Done before we move past the record, so we never send it again. */
	if (requests_done != 0) {
		vigil_log(LOG_INFO, "[companion] our phone did request %"
		    PRIu64 ", it does not go out again", requests_done);
		chat_desktop_acked(requests_done);
		chat_desktop_recall();
		requests_done = 0;
	}

	if (vigil_journal_ack(&journal, seq) == -1) {
		vigil_log(LOG_NOTICE, "[companion] could not save that "
		    "we stored up to %" PRIu64, seq);
		return;
	}

	vigil_log(LOG_INFO, "[companion] stored entries %" PRIu64
	    "/%" PRIu64, seq, companion_last);

	if (synced != journal.synced) {
		vigil_log(LOG_INFO, "[companion] we are %s",
		    synced == VIGIL_JOURNAL_SYNCED ? "synced" :
		    synced == VIGIL_JOURNAL_SYNCING ? "syncing" : "not synced");
	}

	if (vigil_journal_synced(&journal, synced) == -1)
		vigil_log(LOG_NOTICE,
		    "[companion] could not save if we are synced");
}
