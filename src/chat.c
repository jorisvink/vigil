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

#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

#include "vigil.h"

#include <libkyrka/libnyfe.h>

/* The commands the platform gives us. */
#define CHAT_COMMAND_SEND		1
#define CHAT_COMMAND_FORGET		2
#define CHAT_COMMAND_STORED		3
#define CHAT_COMMAND_GROUP		4
#define CHAT_COMMAND_GROUP_SEND		5
#define CHAT_COMMAND_DATA		6
#define CHAT_COMMAND_GROUP_DATA		7

/* The events we have for the platform. */
#define CHAT_EVENT_MESSAGE		1
#define CHAT_EVENT_SENT			2
#define CHAT_EVENT_DELIVERED		3
#define CHAT_EVENT_ACKS			4
#define CHAT_EVENT_JOINED		5
#define CHAT_EVENT_LEFT			6
#define CHAT_EVENT_DATA			7

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

/* Max size of a packet we put on the wire. */
#define CHAT_WIRE_MAX			\
    (VIGIL_CHAT_DATA_MAX + sizeof(struct vigil_chat_data_head))

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
 * A message we can send to the peer. These wait in the outbox of our peer
 * until they are ack'd and potentially on the sendq of the active tunnel
 * for said peer.
 *
 * Group messages just sit on the waitq, and ack's are added to the tally.
 */
struct chat_msg {
	u_int64_t		id;
	u_int64_t		ref;
	u_int64_t		age;
	int			sent;
	int			pending;

	struct chat_blob	*blob;
	struct chat_quote	quote;
	struct chat_tally	*tally;

	struct vigil_chat_frame	frame;

	TAILQ_ENTRY(chat_msg)	wlist;
	TAILQ_ENTRY(chat_msg)	slist;
};

TAILQ_HEAD(chat_msg_list, chat_msg);

/*
 * Keeps track of how many members of our group acked a message we sent and
 * how many we are still waiting for. Members that leave before acking are
 * no longer pending.
 */
struct chat_tally {
	u_int64_t		ref;
	u_int16_t		group;
	u_int32_t		acked;
	u_int32_t		pending;
	int			report;

	TAILQ_ENTRY(chat_tally)	list;
};

TAILQ_HEAD(chat_tally_list, chat_tally);

/*
 * A message we received and handed to the platform.
 */
struct chat_seen {
	u_int64_t		id;
	u_int64_t		age;
	u_int64_t		hash;
	u_int64_t		token;
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
	u_int64_t		age;
	u_int64_t		token;

	size_t			size;
	u_int8_t		*data;

	TAILQ_ENTRY(chat_parcel)	list;
};

TAILQ_HEAD(chat_parcel_list, chat_parcel);

/*
 * A tunnel to a peer (one-to-one or a peer in a group).
 */
struct chat_tunnel {
	struct vigil_io		io;

	KYRKA			*ctx;
	u_int16_t		group;
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
	struct chat_msg_list	waitq;

	struct chat_xfer	*xfer;
	struct chat_rxfer	*rxfer;
};

/*
 * A chat peer.
 */
struct chat_peer {
	u_int8_t		id;
	u_int64_t		active;
	u_int64_t		pushed;
	int			member;
	int			present;

	struct chat_msg_list	outbox;
	struct chat_tunnel	*group;
	struct chat_tunnel	*tunnel;

	char			nick[VIGIL_CHAT_NICK_MAX + 1];
};

/*
 * A liturgy, either the signaling one or the one for our group.
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
	u_int16_t			group;
	u_int64_t			value;
	u_int32_t			acked;
	u_int32_t			pending;
	struct chat_quote		quote;

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

static void	chat_group_leave(void);
static void	chat_group_update(void);
static void	chat_group_join(u_int16_t);
static void	chat_group_data(u_int64_t, struct chat_blob *);
static void	chat_group_send(u_int64_t, const struct chat_quote *,
		    const u_int8_t *, size_t);

static void	chat_tallies_free(void);
static void	chat_tallies_report(void);

static int	chat_tally_report(struct chat_tally *);
static void	chat_tally_update(struct chat_tally *, int);

static void	chat_tunnel_read(void *);
static void	chat_tunnel_free(struct chat_tunnel *);
static void	chat_tunnel_poke(struct chat_tunnel *);
static void	chat_tunnel_manage(struct chat_tunnel *);
static void	chat_tunnel_rebind(struct chat_tunnel *);
static int	chat_tunnel_ack(struct chat_tunnel *, u_int64_t);
static int	chat_tunnel_online(struct chat_tunnel *);
static void	chat_tunnel_check(struct chat_peer *, u_int16_t);
static void	chat_tunnel_event(KYRKA *, union kyrka_event *, void *);
static void	chat_tunnel_state(struct chat_tunnel *,
		    const char **, int *, int *);

static struct chat_tunnel	*chat_tunnel_new(struct chat_peer *,
				    struct vigil_cathedral *, u_int16_t);

static void	chat_cathedral_alive(struct chat_tunnel *);
static void	chat_cathedral_check(struct chat_tunnel *);
static void	chat_cathedral_send(struct kyrka_packet *, u_int64_t, void *);

static void	chat_p2p_fail(struct chat_tunnel *);
static void	chat_p2p_follow(struct chat_tunnel *);
static void	chat_p2p_abandon(struct chat_tunnel *);

static void	chat_token_ack(u_int64_t);
static int	chat_nick_valid(const char *);
static int	chat_signal_check(struct chat_peer *);
static void	chat_utf8_normalize(u_int8_t *, size_t);
static void	chat_member_report(struct chat_peer *, u_int16_t, int);
static void	chat_heaven_input(struct kyrka_packet *, u_int64_t, void *);
static void	chat_hb_recv(struct chat_tunnel *, struct vigil_chat_frame *);
static void	chat_purgatory_input(struct kyrka_packet *, u_int64_t, void *);
static void	chat_text_recv(struct chat_tunnel *, u_int64_t,
		    const struct chat_quote *, u_int8_t *,
		    u_int16_t);

static void	chat_ack_recv(struct chat_tunnel *, u_int64_t);
static void	chat_ack_queue(struct chat_tunnel *, u_int64_t);

static u_int64_t	chat_frame_hash(const u_int8_t *, size_t);
static void		chat_frame_send(struct chat_tunnel *,
			    struct chat_msg *);

static void	chat_msg_free(struct chat_msg *);
static void	chat_msg_resend(struct chat_tunnel *);
static void	chat_msg_queue(struct chat_tunnel *, struct chat_msg *);

static void	chat_outbox_free(struct chat_peer *);
static void	chat_outbox_add(struct chat_peer *, u_int64_t,
		    const struct chat_quote *, const u_int8_t *, size_t);
static void	chat_outbox_lookup(struct chat_tunnel *,
		    struct chat_msg_list **);
static void	chat_outbox_data(struct chat_peer *, u_int64_t,
		    struct chat_blob *);

static void	chat_commands_run(void);
static void	chat_commands_free(void);
static void	chat_command_free(struct chat_command *);
static void	chat_command_next(struct chat_command **);
static int	chat_command_insert(struct chat_command *);
static int	chat_command_queue(int, u_int8_t, u_int64_t,
		    const struct chat_quote *, const void *, size_t);

static void	chat_reply_frame(struct vigil_chat_frame *,
		    const struct chat_msg *);
static void	chat_reply_recv(struct chat_tunnel *, u_int64_t,
		    u_int8_t *, u_int16_t);
static int	chat_reply_report(struct chat_tunnel *, u_int64_t,
		    const struct chat_quote *, const u_int8_t *, size_t);

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
static int	chat_data_queue(int, u_int8_t, u_int64_t, int, const char *,
		    const void *, size_t);

static struct chat_msg	*chat_data_msg(u_int64_t, struct chat_blob *);

static const char	*chat_data_kind_name(int);

static void	chat_xfer_run(struct chat_tunnel *);
static void	chat_xfer_free(struct chat_tunnel *);
static void	chat_xfer_pump(struct chat_tunnel *);
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

static void	chat_lock(void);
static void	chat_unlock(void);

static void	chat_events_free(void);
static int	chat_event_insert(struct chat_event *);
static void	chat_events_take(struct chat_event_list *, size_t);
static void	chat_event_render(struct vigil_buf *, struct chat_event *, int);
static int	chat_event_queue(int, struct chat_peer *, u_int16_t, u_int64_t,
		    const u_int8_t *, size_t);

/* Are we alive? */
static int				running = 0;

/* Each time we start we increase the session counter. */
static u_int64_t			session = 0;

/* If we are accepting commands or not. */
static volatile int			accepting = 0;

/* Incremental token id for the messages we receive. */
static u_int64_t			tokens = 0;

/* The liturgies for the chat. */
static struct chat_liturgy		signaling;
static struct chat_liturgy		discovery;

/* What we last told the cathedral in the signaling liturgy. */
static u_int8_t				sent[KYRKA_PEERS_PER_FLOCK];

/* All peers we can chat with, populated from discovery. */
static struct chat_peer			peers[KYRKA_PEERS_PER_FLOCK];

/* The peers whose chat is open and those we type to. */
static volatile u_int8_t		opened[KYRKA_PEERS_PER_FLOCK];
static volatile u_int8_t		typing[KYRKA_PEERS_PER_FLOCK];

/* If we are currently typing in a group. */
static volatile int			group_typing = 0;

/* Mutex to protect against concurrent access for our commands and events. */
static pthread_mutex_t			lock = PTHREAD_MUTEX_INITIALIZER;

/* Our nickname, empty if we have none. */
static char				nick[VIGIL_CHAT_NICK_MAX + 1];

/* The commands for the event loop. */
static struct chat_command_list		commands;
static size_t				command_count = 0;

/* The tallies for our group messages. */
static struct chat_tally_list		tallies;

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
	memset(&discovery, 0, sizeof(discovery));

	for (idx = 0; idx < KYRKA_PEERS_PER_FLOCK; idx++) {
		memset(&peers[idx], 0, sizeof(peers[idx]));
		TAILQ_INIT(&peers[idx].outbox);
		peers[idx].id = idx;
	}

	chat_lock();

	transfers = 0;
	blob_bytes = 0;
	event_count = 0;
	command_count = 0;

	TAILQ_INIT(&blobs);
	TAILQ_INIT(&events);
	TAILQ_INIT(&tallies);
	TAILQ_INIT(&parcels);
	TAILQ_INIT(&commands);

	chat_unlock();

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
	chat_group_leave();

	for (idx = 0; idx < KYRKA_PEERS_PER_FLOCK; idx++) {
		if (peers[idx].tunnel != NULL)
			chat_tunnel_free(peers[idx].tunnel);
		chat_outbox_free(&peers[idx]);
	}

	chat_liturgy_cleanup(&signaling);
	chat_commands_free();
	chat_parcels_free();
	chat_events_free();
	chat_tallies_free();

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
	chat_parcels_expire();

	local = vigil->gospel->spi & 0xff;

	chat_liturgy_cathedral_check(&signaling);
	chat_liturgy_cathedral_check(&discovery);

	for (idx = 1; idx < KYRKA_PEERS_PER_FLOCK; idx++) {
		if (idx == local)
			continue;

		if (peers[idx].present &&
		    (vigil->now - peers[idx].pushed) >=
		    VIGIL_CALL_PRESENT_EXPIRE) {
			vigil_log(LOG_INFO, "[chat] %02x is gone", idx);
			peers[idx].present = 0;
		}

		chat_tunnel_check(&peers[idx], VIGIL_CHAT_SIGNAL_GROUP);

		if (discovery.group != 0)
			chat_tunnel_check(&peers[idx], discovery.group);
	}

	chat_liturgy_signal();
	chat_group_update();
	chat_tallies_report();
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
	chat_liturgy_rebind(&discovery);

	for (idx = 0; idx < KYRKA_PEERS_PER_FLOCK; idx++) {
		if (peers[idx].tunnel != NULL)
			chat_tunnel_rebind(peers[idx].tunnel);
		if (peers[idx].group != NULL)
			chat_tunnel_rebind(peers[idx].group);
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
	discovery.notify = 0;

	for (idx = 0; idx < KYRKA_PEERS_PER_FLOCK; idx++) {
		if (peers[idx].tunnel != NULL)
			chat_tunnel_poke(peers[idx].tunnel);
		if (peers[idx].group != NULL)
			chat_tunnel_poke(peers[idx].group);
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
	    peer, ref, NULL, text, len);

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
	    peer, ref, &quote, text, len);

	return (ret);
}

/*
 * Join a group and leave current group (if any).
 */
int
vigil_chat_group(u_int16_t id)
{
	return (chat_command_queue(CHAT_COMMAND_GROUP, 0, id, NULL, NULL, 0));
}

/*
 * Tell group members we're busy typing bullshit.
 */
void
vigil_chat_group_typing(int on)
{
	PRECOND(on == 0 || on == 1);

	vigil_atomic_write(&group_typing, on);
	vigil_wake_signal();
}

/*
 * Queue a message for our group, sent to every online member.
 */
int
vigil_chat_group_send(u_int64_t ref, const char *text, size_t len)
{
	int		ret;

	PRECOND(text != NULL);
	PRECOND(len > 0 && len <= VIGIL_CHAT_MESSAGE_MAX);

	ret = chat_command_queue(CHAT_COMMAND_GROUP_SEND,
	    0, ref, NULL, text, len);

	return (ret);
}

/*
 * Queue a reply for our group, see vigil_chat_reply().
 */
int
vigil_chat_group_reply(u_int64_t ref, u_int8_t author, u_int64_t hash,
    const char *text, size_t len)
{
	int			ret;
	struct chat_quote	quote;

	PRECOND(hash != 0);
	PRECOND(text != NULL);
	PRECOND(len > 0 && len <= VIGIL_CHAT_REPLY_MAX);

	quote.hash = hash;
	quote.author = author;

	ret = chat_command_queue(CHAT_COMMAND_GROUP_SEND,
	    0, ref, &quote, text, len);

	return (ret);
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

	ret = chat_data_queue(CHAT_COMMAND_DATA,
	    peer, ref, kind, name, data, len);

	return (ret);
}

/*
 * Send data to all peers in our group.
 */
int
vigil_chat_group_data_send(u_int64_t ref, int kind, const char *name,
    const void *data, size_t len)
{
	int		ret;

	PRECOND(name != NULL);
	PRECOND(data != NULL);
	PRECOND(len > 0 && len <= VIGIL_CHAT_DATA_MAX);

	ret = chat_data_queue(CHAT_COMMAND_GROUP_DATA,
	    0, ref, kind, name, data, len);

	return (ret);
}

/*
 * Copy the data that arrived into the given buffer.
 */
ssize_t
vigil_chat_data_take(u_int64_t token, void *buf, size_t len)
{
	size_t			left;
	struct chat_parcel	*parcel;

	PRECOND(buf != NULL);

	chat_lock();

	TAILQ_FOREACH(parcel, &parcels, list) {
		if (parcel->token == token)
			break;
	}

	if (parcel == NULL) {
		chat_unlock();
		return (-1);
	}

	if (parcel->size < sizeof(struct vigil_chat_data_head)) {
		chat_unlock();
		return (-1);
	}

	left = parcel->size - sizeof(struct vigil_chat_data_head);

	if (left > len) {
		chat_unlock();
		return (-1);
	}

	TAILQ_REMOVE(&parcels, parcel, list);
	parcel_bytes -= parcel->size;
	chat_unlock();

	memcpy(buf, parcel->data + sizeof(struct vigil_chat_data_head), left);
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

	ret = chat_command_queue(CHAT_COMMAND_FORGET, peer, 0, NULL, NULL, 0);

	return (ret);
}

/*
 * The platform stored the message, so go ahead and ack it.
 */
int
vigil_chat_stored(u_int64_t token)
{
	int		ret;

	ret = chat_command_queue(CHAT_COMMAND_STORED, 0, token, NULL, NULL, 0);

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
 * Tell the given peer if we are typing.
 */
void
vigil_chat_typing(u_int8_t peer, int on)
{
	PRECOND(peer > 0 && peer < KYRKA_PEERS_PER_FLOCK);
	PRECOND(on == 0 || on == 1);

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

	chat_lock();
	memset(nick, 0, sizeof(nick));
	len = snprintf(nick, sizeof(nick), "%s", name);
	chat_unlock();

	if (len < 0)
		return (-1);

	if ((size_t)len >= sizeof(nick))
		return (-1);

	return (0);
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
 * Append the state of our chats to the status, this includes our peers, our
 * tunnels and our group with its members.
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

	PRECOND(buf != NULL);

	if (!running)
		return;

	vigil_buf_appendf(buf,
	    ",\"chat\":{\"session\":%" PRIu64 ",\"alive\":%s,\"liturgies\":[",
	    session, signaling.last != 0 ? "true" : "false");

	vigil_status_liturgy(buf, "chat", signaling.group,
	    &signaling.cathedral, signaling.rx, signaling.tx);

	if (discovery.group != 0) {
		vigil_buf_appendf(buf, ",");
		vigil_status_liturgy(buf, "group", discovery.group,
		    &discovery.cathedral, discovery.rx, discovery.tx);
	}

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
		vigil_buf_appendf(buf, "}");

		first = 0;
	}

	vigil_buf_appendf(buf, "]");

	if (discovery.group == 0) {
		vigil_buf_appendf(buf, ",\"group\":null}");
		return;
	}

	vigil_buf_appendf(buf,
	    ",\"group\":{\"id\":\"%04x\",\"alive\":%s,\"members\":[",
	    discovery.group, discovery.last != 0 ? "true" : "false");

	first = 1;

	for (idx = 1; idx < KYRKA_PEERS_PER_FLOCK; idx++) {
		peer = &peers[idx];

		if (peer->member == 0 && peer->group == NULL)
			continue;

		chat_tunnel_state(peer->group, &link, &online, &p2p);
		vigil_buf_appendf(buf,
		    "%s{\"id\":\"%02x\",\"online\":%s,\"link\":\"%s\""
		    ",\"typing\":%s,\"nick\":", first ? "" : ",", idx,
		    online ? "true" : "false", link,
		    online && peer->group->typing ? "true" : "false");

		vigil_buf_quote(buf, peer->nick);
		vigil_buf_appendf(buf, "}");

		first = 0;
	}

	vigil_buf_appendf(buf, "]}}");
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

	for (idx = 0; idx < KYRKA_PEERS_PER_FLOCK; idx++) {
		if (lit == &signaling)
			peers[idx].present = 0;
		else
			peers[idx].member = 0;
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
	int			idx, first;
	u_int8_t		local, state;

	PRECOND(ctx != NULL);
	PRECOND(evt != NULL);
	PRECOND(udata != NULL);

	lit = udata;
	first = 0;
	local = vigil->gospel->spi & 0xff;

	if (evt->type == KYRKA_EVENT_LITURGY_RECEIVED ||
	    evt->type == KYRKA_EVENT_REMEMBRANCE_RECEIVED) {
		if (lit->last == 0) {
			vigil_log(LOG_INFO, "[chat] liturgy %04x is alive",
			    lit->group);
			first = 1;
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

			state = evt->liturgy.peers[idx] ? 1 : 0;

			/*
			 * The cathedral pushes us each peer that signals us
			 * on its own, peers expire in vigil_chat_update().
			 */
			if (lit == &signaling) {
				if (state == 0)
					continue;
				if (peers[idx].present == 0) {
					vigil_log(LOG_INFO,
					    "[chat] %02x is around", idx);
				}
				peers[idx].present = 1;
				peers[idx].pushed = vigil->now;
			} else {
				if (state != peers[idx].member) {
					vigil_log(LOG_INFO, "[group] %02x %s",
					    idx, state ? "joined" : "left");

					if (first == 0) {
						chat_member_report(&peers[idx],
						    lit->group, state);
					}
				}

				peers[idx].member = state;
			}
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
 * Join the given group, leaving the one we are in first.
 */
static void
chat_group_join(u_int16_t id)
{
	if (id == discovery.group)
		return;

	chat_group_leave();

	if (id == VIGIL_CHAT_SIGNAL_GROUP)
		return;

	if (chat_liturgy_init(&discovery, id) == -1)
		return;

	vigil_log(LOG_INFO, "[group] joined %04x", id);
}

/*
 * Leave our group, dropping all messages pending.
 */
static void
chat_group_leave(void)
{
	int		idx;

	if (discovery.group == 0)
		return;

	vigil_log(LOG_INFO, "[group] left %04x", discovery.group);

	for (idx = 0; idx < KYRKA_PEERS_PER_FLOCK; idx++) {
		if (peers[idx].group != NULL)
			chat_tunnel_free(peers[idx].group);
		peers[idx].member = 0;
	}

	chat_liturgy_cleanup(&discovery);
}

/*
 * Update the cathedral about our group liturgy.
 */
static void
chat_group_update(void)
{
	if (discovery.ctx == NULL)
		return;

	if (discovery.cathedral.addr.sin_port == 0)
		return;

	if (discovery.notify != 0 && vigil->now < discovery.notify)
		return;

	discovery.notify = vigil->now + VIGIL_CALL_NOTIFY;

	if (kyrka_cathedral_liturgy(discovery.ctx, NULL, 0) == -1) {
		vigil_log(LOG_NOTICE, "[group] kyrka_cathedral_liturgy: %d",
		    kyrka_last_error(discovery.ctx));
	}
}

/*
 * Queue a copy of the given message for every member of our group, each
 * member is pending in the tally of the message until it acks.
 */
static void
chat_group_send(u_int64_t ref, const struct chat_quote *quote,
    const u_int8_t *data, size_t len)
{
	int			idx;
	struct chat_tunnel	*tun;
	struct chat_msg		*msg;
	struct chat_tally	*tally;

	PRECOND(data != NULL);
	PRECOND(len > 0 && len <= VIGIL_CHAT_MESSAGE_MAX);

	tally = NULL;

	for (idx = 0; idx < KYRKA_PEERS_PER_FLOCK; idx++) {
		tun = peers[idx].group;
		if (tun == NULL)
			continue;

		if (tally == NULL) {
			tally = calloc(1, sizeof(*tally));
			if (tally == NULL)
				fatal("calloc: failed to allocate tally");

			tally->ref = ref;
			tally->report = 1;
			tally->group = discovery.group;

			TAILQ_INSERT_TAIL(&tallies, tally, list);
		}

		msg = calloc(1, sizeof(*msg));
		if (msg == NULL)
			fatal("calloc: failed to allocate group message");

		msg->ref = ref;
		msg->tally = tally;
		msg->frame.len = htobe16(len);
		msg->frame.type = VIGIL_CHAT_TYPE_TEXT;

		memcpy(msg->frame.data, data, len);
		memcpy(&msg->quote, quote, sizeof(*quote));

		TAILQ_INSERT_TAIL(&tun->waitq, msg, wlist);
		chat_msg_queue(tun, msg);

		tally->pending++;
	}
}

/*
 * Tell the platform about the tallies that changed, those that have
 * nothing pending anymore are done once the platform heard about it.
 */
static void
chat_tallies_report(void)
{
	struct chat_tally	*tally, *next;

	for (tally = TAILQ_FIRST(&tallies); tally != NULL; tally = next) {
		next = TAILQ_NEXT(tally, list);

		if (tally->report && chat_tally_report(tally) != -1)
			tally->report = 0;

		if (tally->report == 0 && tally->pending == 0) {
			TAILQ_REMOVE(&tallies, tally, list);
			free(tally);
		}
	}
}

/*
 * A member acked its copy of a group message or never will, it is no longer
 * pending.
 */
static void
chat_tally_update(struct chat_tally *tally, int acked)
{
	PRECOND(tally != NULL);
	PRECOND(tally->pending > 0);
	PRECOND(acked == 0 || acked == 1);

	tally->pending--;
	tally->report = 1;

	if (acked)
		tally->acked++;
}

/*
 * Drop all tallies, the platform keeps the last counts it heard.
 */
static void
chat_tallies_free(void)
{
	struct chat_tally	*tally;

	while ((tally = TAILQ_FIRST(&tallies)) != NULL) {
		TAILQ_REMOVE(&tallies, tally, list);
		free(tally);
	}
}

/*
 * Set up or tear down the tunnel with the given peer in the given group.
 *
 * We want a tunnel for as long as the peer shows up in the liturgy for it,
 * each tunnel uses a cathedral picked at random from the ones we know.
 *
 * In the signaling group the peer only shows up while it signals us, so
 * we keep a tunnel that is online for as long as we want the peer too.
 */
static void
chat_tunnel_check(struct chat_peer *peer, u_int16_t id)
{
	struct vigil_cathedral	cat;
	struct chat_tunnel	*tun;
	int			want;

	PRECOND(peer != NULL);

	if (id == VIGIL_CHAT_SIGNAL_GROUP) {
		tun = peer->tunnel;
		want = peer->present;
		if (chat_tunnel_online(tun) && chat_signal_check(peer))
			want = 1;
	} else {
		tun = peer->group;
		want = peer->member;
	}

	if (want == 0) {
		if (tun != NULL)
			chat_tunnel_free(tun);
		return;
	}

	if (tun == NULL) {
		memset(&cat, 0, sizeof(cat));

		if (vigil_call_cathedral_pick(&cat) == -1)
			return;

		tun = chat_tunnel_new(peer, &cat, id);
	}

	if (tun != NULL)
		chat_tunnel_manage(tun);
}

/*
 * Set up a tunnel with the given peer via the given cathedral, in the
 * litany domain or in the group domain if it is for our group. Any messages
 * we hold for the peer are queued right away.
 */
static struct chat_tunnel *
chat_tunnel_new(struct chat_peer *peer, struct vigil_cathedral *cathedral,
    u_int16_t id)
{
	struct kyrka_cathedral_cfg	cfg;
	struct chat_msg			*msg;
	struct chat_tunnel		*tun;
	u_int8_t			local;
	struct vigil_config		*gospel;

	PRECOND(peer != NULL);
	PRECOND(cathedral != NULL);
	PRECOND(id == VIGIL_CHAT_SIGNAL_GROUP || id == discovery.group);
	PRECOND(id != VIGIL_CHAT_SIGNAL_GROUP || peer->tunnel == NULL);
	PRECOND(id == VIGIL_CHAT_SIGNAL_GROUP || peer->group == NULL);

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
	TAILQ_INIT(&tun->waitq);

	tun->group = id;
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
	cfg.flock_src = gospel->cathedral_flock;

	if (id == VIGIL_CHAT_SIGNAL_GROUP)
		cfg.flock_src |= VIGIL_CHAT_FLOCK_DOMAIN;
	else
		cfg.flock_src |= VIGIL_CHAT_GROUP_DOMAIN;

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

	if (id != VIGIL_CHAT_SIGNAL_GROUP) {
		peer->group = tun;
		vigil_log(LOG_INFO, "[group] %02x tunnel via %s",
		    peer->id, vigil_address(&cathedral->addr));
		return (tun);
	}

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
 * outbox of the peer for the next tunnel, group messages are dropped.
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

	while ((msg = TAILQ_FIRST(&tun->waitq)) != NULL) {
		TAILQ_REMOVE(&tun->waitq, msg, wlist);
		chat_tally_update(msg->tally, 0);
		chat_msg_free(msg);
	}

	if (tun == peer->tunnel) {
		TAILQ_FOREACH(msg, &peer->outbox, wlist) {
			msg->id = 0;
			msg->pending = 0;
		}
	}

	vigil_io_unschedule(&tun->io);
	vigil_socket_close(tun->io.fd);

	kyrka_ctx_free(tun->ctx);

	if (tun == peer->group) {
		vigil_log(LOG_INFO, "[group] %02x tunnel closed", peer->id);
		peer->group = NULL;
	} else {
		vigil_log(LOG_INFO, "[chat] %02x tunnel closed", peer->id);
		peer->tunnel = NULL;
	}

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
	chat_xfer_run(tun);
	chat_rxfer_check(tun);

	if (tun->group != 0)
		typed = vigil_atomic_read(&group_typing);
	else
		typed = vigil_atomic_read(&typing[tun->peer->id]);

	if (!tun->greeted || typed != tun->typed ||
	    vigil->now >= tun->next_heartbeat)
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
		chat_text_recv(tun, id, NULL, frame->data, len);
		break;
	case VIGIL_CHAT_TYPE_REPLY:
		chat_reply_recv(tun, id, frame->data, len);
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

		if (msg->quote.hash != 0 &&
		    (tun->features & VIGIL_CHAT_FEATURE_REPLY))
			chat_reply_frame(frame, msg);

		if (msg->frame.type == VIGIL_CHAT_TYPE_TEXT) {
			msg->age = vigil->now;

			if (tun->group == 0 && msg->sent == 0 &&
			    chat_event_queue(CHAT_EVENT_SENT, tun->peer, 0,
			    msg->ref, NULL, 0) != -1)
				msg->sent = 1;
		} else {
			free(msg);
		}
	} else {
		memset(frame, 0, sizeof(*frame));

		frame->id = VIGIL_CHAT_HEARTBEAT_ID;
		frame->type = VIGIL_CHAT_TYPE_HEARTBEAT;

		hb = (struct vigil_chat_hb *)frame->data;
		hb->uid = tun->local_uid;
		hb->features = VIGIL_CHAT_FEATURE_DATA |
		    VIGIL_CHAT_FEATURE_REPLY;

		if (tun->group != 0)
			hb->typing = vigil_atomic_read(&group_typing);
		else
			hb->typing = vigil_atomic_read(&typing[tun->peer->id]);

		chat_lock();
		memcpy(hb->name, nick, sizeof(hb->name));
		chat_unlock();

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
 * delivered and drop it. Group messages count towards their tally instead.
 */
static void
chat_ack_recv(struct chat_tunnel *tun, u_int64_t id)
{
	struct chat_msg		*msg;
	struct chat_msg_list	*list;

	PRECOND(tun != NULL);
	PRECOND(id != 0);

	chat_outbox_lookup(tun, &list);

	TAILQ_FOREACH(msg, list, wlist) {
		if (msg->id == id)
			break;
	}

	if (msg == NULL)
		return;

	if (tun->group == 0) {
		if (chat_event_queue(CHAT_EVENT_DELIVERED,
		    tun->peer, 0, msg->ref, NULL, 0) == -1)
			return;

		tun->peer->active = vigil->now;
	} else {
		chat_tally_update(msg->tally, 1);
	}

	if (msg->pending)
		TAILQ_REMOVE(&tun->sendq, msg, slist);

	if (tun->xfer != NULL && tun->xfer->msg == msg)
		chat_xfer_free(tun);

	TAILQ_REMOVE(list, msg, wlist);
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
	}

	tun->heard = 1;
	tun->features = hb->features;
	tun->typing = hb->typing ? 1 : 0;

	if (tun->remote_uid == hb->uid)
		return;

	if (tun->remote_uid != 0) {
		tun->seen_next = 0;
		memset(tun->seen, 0, sizeof(tun->seen));
		chat_rxfer_free(tun);
	}

	tun->remote_uid = hb->uid;
}

/*
 * We got a message from our peer and hand it to the platform.
 *
 * If we had it already we ack it again once it was stored, if the platform
 * did not store it in time we hand it over again under a new token.
 */
static void
chat_text_recv(struct chat_tunnel *tun, u_int64_t id,
    const struct chat_quote *quote, u_int8_t *data, u_int16_t len)
{
	int			ret;
	u_int32_t		idx;
	u_int64_t		hash;
	struct chat_seen	*seen;

	PRECOND(tun != NULL);
	PRECOND(id != 0);
	PRECOND(data != NULL);
	PRECOND(len <= VIGIL_CHAT_MESSAGE_MAX);

	if (len == 0) {
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

	if (quote != NULL) {
		ret = chat_reply_report(tun, tokens, quote, data, len);
	} else {
		ret = chat_event_queue(CHAT_EVENT_MESSAGE,
		    tun->peer, tun->group, tokens, data, len);
	}

	if (ret == -1)
		return;

	if (seen == NULL)
		seen = &tun->seen[tun->seen_next++ % CHAT_SEEN_MAX];

	seen->id = id;
	seen->size = 0;
	seen->hash = hash;
	seen->token = tokens;
	seen->age = vigil->now;

	if (tun->group == 0)
		tun->peer->active = vigil->now;
}

/*
 * We got a reply from our peer, take who wrote the message it replies to
 * and hand it over like any other message.
 */
static void
chat_reply_recv(struct chat_tunnel *tun, u_int64_t id, u_int8_t *data,
    u_int16_t len)
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
		chat_text_recv(tun, id, NULL, reply->text, len - head);
		return;
	}

	chat_text_recv(tun, id, &quote, reply->text, len - head);
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
 * Queue a message event for a reply we got, returns -1 if there are
 * too many.
 */
static int
chat_reply_report(struct chat_tunnel *tun, u_int64_t token,
    const struct chat_quote *quote, const u_int8_t *data, size_t len)
{
	struct chat_event	*evt;

	PRECOND(tun != NULL);
	PRECOND(quote != NULL);
	PRECOND(quote->hash != 0);
	PRECOND(data != NULL);
	PRECOND(len <= VIGIL_CHAT_MESSAGE_MAX);

	evt = calloc(1, sizeof(*evt));
	if (evt == NULL)
		fatal("calloc: failed to allocate chat event");

	evt->value = token;
	evt->group = tun->group;
	evt->peer = tun->peer->id;
	evt->type = CHAT_EVENT_MESSAGE;

	memcpy(evt->text, data, len);
	memcpy(&evt->quote, quote, sizeof(*quote));
	memcpy(evt->nick, tun->peer->nick, sizeof(evt->nick));

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
	struct chat_msg_list	*list;

	PRECOND(tun != NULL);

	chat_outbox_lookup(tun, &list);

	TAILQ_FOREACH(msg, list, wlist) {
		if (msg->pending)
			continue;

		if (msg->id == 0)
			continue;

		if (msg->blob != NULL)
			continue;

		if ((vigil->now - msg->age) < CHAT_RESEND)
			continue;

		if (msg->quote.hash != 0 && !tun->heard)
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

	/* Replies wait until we know if our peer takes them. */
	if (msg->quote.hash != 0 && !tun->heard) {
		msg->age = 0;
		return;
	}

	msg->pending = 1;
	TAILQ_INSERT_TAIL(&tun->sendq, msg, slist);
}

/*
 * Add a message to the outbox of the given peer unless we have it.
 */
static void
chat_outbox_add(struct chat_peer *peer, u_int64_t ref,
    const struct chat_quote *quote, const u_int8_t *data, size_t len)
{
	struct chat_msg		*msg;

	PRECOND(peer != NULL);
	PRECOND(data != NULL);
	PRECOND(len > 0 && len <= VIGIL_CHAT_MESSAGE_MAX);

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
		if (chat_tunnel_ack(peers[idx].group, token) != -1)
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
    const struct chat_quote *quote, const void *data, size_t len)
{
	struct chat_command	*cmd;

	PRECOND(type == CHAT_COMMAND_SEND ||
	    type == CHAT_COMMAND_FORGET ||
	    type == CHAT_COMMAND_STORED ||
	    type == CHAT_COMMAND_GROUP ||
	    type == CHAT_COMMAND_GROUP_SEND);
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
chat_data_queue(int type, u_int8_t peer, u_int64_t value, int kind,
    const char *name, const void *data, size_t len)
{
	int				ret;
	size_t				size;
	struct chat_command		*cmd;
	struct vigil_chat_data_head	head;
	struct chat_blob		*blob;

	PRECOND(type == CHAT_COMMAND_DATA ||
	    type == CHAT_COMMAND_GROUP_DATA);
	PRECOND(name != NULL);
	PRECOND(data != NULL);
	PRECOND(len > 0 && len <= VIGIL_CHAT_DATA_MAX);

	if (!vigil_atomic_read(&accepting))
		return (-1);

	if (chat_data_kind_valid(kind) == -1)
		return (-1);

	if (chat_data_name_valid((const u_int8_t *)name, strlen(name)) == -1)
		return (-1);

	size = sizeof(head) + len;

	chat_lock();
	ret = chat_blob_room(peer, value, size);
	chat_unlock();

	if (ret == 1)
		return (0);

	if (ret == -1)
		return (-1);

	memset(&head, 0, sizeof(head));
	head.kind = kind;
	head.len = strlen(name);
	memcpy(head.name, name, head.len);

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

	memcpy(blob->data, &head, sizeof(head));
	memcpy(blob->data + sizeof(head), data, len);

	cmd = calloc(1, sizeof(*cmd));
	if (cmd == NULL)
		fatal("calloc: failed to allocate chat command");

	cmd->type = type;
	cmd->peer = peer;
	cmd->blob = blob;
	cmd->value = value;

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

	chat_lock();

	if (!vigil_atomic_read(&accepting) ||
	    command_count >= CHAT_COMMANDS_MAX) {
		chat_unlock();
		chat_command_free(cmd);
		return (-1);
	}

	if (cmd->blob != NULL) {
		ret = chat_blob_room(cmd->blob->peer,
		    cmd->blob->ref, cmd->blob->size);

		if (ret != 0) {
			chat_unlock();
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

	chat_unlock();

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
				chat_outbox_add(peer, cmd->value,
				    &cmd->quote, cmd->data, cmd->len);
			}
			break;
		case CHAT_COMMAND_FORGET:
			chat_outbox_free(peer);
			break;
		case CHAT_COMMAND_STORED:
			chat_token_ack(cmd->value);
			break;
		case CHAT_COMMAND_GROUP:
			chat_group_join(cmd->value & 0xffff);
			break;
		case CHAT_COMMAND_GROUP_SEND:
			if (cmd->quote.hash != 0 && cmd->quote.author == 0)
				cmd->quote.author = local;

			chat_group_send(cmd->value,
			    &cmd->quote, cmd->data, cmd->len);
			break;
		case CHAT_COMMAND_DATA:
			if (cmd->peer != local)
				chat_outbox_data(peer, cmd->value, cmd->blob);
			break;
		case CHAT_COMMAND_GROUP_DATA:
			chat_group_data(cmd->value, cmd->blob);
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
chat_event_queue(int type, struct chat_peer *peer, u_int16_t id,
    u_int64_t value, const u_int8_t *data, size_t len)
{
	struct chat_event	*evt;

	PRECOND(type == CHAT_EVENT_MESSAGE ||
	    type == CHAT_EVENT_SENT ||
	    type == CHAT_EVENT_DELIVERED ||
	    type == CHAT_EVENT_JOINED ||
	    type == CHAT_EVENT_LEFT);
	PRECOND(peer != NULL);
	PRECOND(data != NULL || len == 0);
	PRECOND(len <= VIGIL_CHAT_MESSAGE_MAX);

	evt = calloc(1, sizeof(*evt));
	if (evt == NULL)
		fatal("calloc: failed to allocate chat event");

	evt->group = id;
	evt->type = type;
	evt->value = value;
	evt->peer = peer->id;

	memcpy(evt->nick, peer->nick, sizeof(evt->nick));

	if (len > 0)
		memcpy(evt->text, data, len);

	return (chat_event_insert(evt));
}

/*
 * Tell the platform the given peer joined or left our group.
 */
static void
chat_member_report(struct chat_peer *peer, u_int16_t group, int joined)
{
	PRECOND(peer != NULL);
	PRECOND(group != 0);

	if (joined) {
		if (chat_event_queue(CHAT_EVENT_JOINED,
		    peer, group, 0, NULL, 0) == -1) {
			vigil_log(LOG_NOTICE, "[chat] %02x joined %04x, "
			    "too many events to tell", peer->id, group);
		}
		return;
	}

	if (chat_event_queue(CHAT_EVENT_LEFT, peer, group, 0, NULL, 0) == -1) {
		vigil_log(LOG_NOTICE, "[chat] %02x left %04x, "
		    "too many events to tell", peer->id, group);
	}
}

/*
 * Queue an event with the counts of the given tally for the platform,
 * returns -1 if there are too many.
 */
static int
chat_tally_report(struct chat_tally *tally)
{
	struct chat_event	*evt;

	PRECOND(tally != NULL);

	evt = calloc(1, sizeof(*evt));
	if (evt == NULL)
		fatal("calloc: failed to allocate chat event");

	evt->value = tally->ref;
	evt->group = tally->group;
	evt->acked = tally->acked;
	evt->type = CHAT_EVENT_ACKS;
	evt->pending = tally->pending;

	return (chat_event_insert(evt));
}

/*
 * Put the given event on the queue for the platform, returns -1 and
 * frees it if there are too many.
 */
static int
chat_event_insert(struct chat_event *evt)
{
	PRECOND(evt != NULL);

	chat_lock();

	if (event_count >= CHAT_EVENTS_MAX) {
		chat_unlock();
		vigil_log(LOG_NOTICE, "[chat] too many events queued");
		nyfe_mem_zero(evt, sizeof(*evt));
		free(evt);
		return (-1);
	}

	event_count++;
	TAILQ_INSERT_TAIL(&events, evt, list);

	chat_unlock();

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

	chat_lock();

	while ((evt = TAILQ_FIRST(&events)) != NULL) {
		if (max == 0)
			break;

		TAILQ_REMOVE(&events, evt, list);
		TAILQ_INSERT_TAIL(list, evt, list);

		event_count--;
		max--;
	}

	chat_unlock();
}

/*
 * Append the given event as a JSON object to the given buffer.
 */
static void
chat_event_render(struct vigil_buf *buf, struct chat_event *evt, int first)
{
	PRECOND(buf != NULL);
	PRECOND(evt != NULL);

	vigil_buf_appendf(buf, "%s{\"peer\":\"%02x\"",
	    first ? "" : ",", evt->peer);

	if (evt->group != 0) {
		vigil_buf_appendf(buf, ",\"group\":\"%04x\"", evt->group);
	}

	switch (evt->type) {
	case CHAT_EVENT_MESSAGE:
		vigil_buf_appendf(buf,
		    ",\"type\":\"message\",\"token\":%" PRIu64
		    ",\"nick\":", evt->value);
		vigil_buf_quote(buf, evt->nick);
		vigil_buf_appendf(buf, ",\"text\":");
		vigil_buf_quote(buf, evt->text);

		if (evt->quote.hash != 0) {
			vigil_buf_appendf(buf,
			    ",\"author\":\"%02x\",\"hash\":\"%016" PRIx64 "\"",
			    evt->quote.author, evt->quote.hash);
		}
		break;
	case CHAT_EVENT_SENT:
		vigil_buf_appendf(buf,
		    ",\"type\":\"sent\",\"ref\":%" PRIu64, evt->value);
		break;
	case CHAT_EVENT_DELIVERED:
		vigil_buf_appendf(buf,
		    ",\"type\":\"delivered\",\"ref\":%" PRIu64, evt->value);
		break;
	case CHAT_EVENT_ACKS:
		vigil_buf_appendf(buf,
		    ",\"type\":\"acks\",\"ref\":%" PRIu64
		    ",\"acked\":%u,\"pending\":%u", evt->value,
		    evt->acked, evt->pending);
		break;
	case CHAT_EVENT_JOINED:
		vigil_buf_appendf(buf, ",\"type\":\"joined\",\"nick\":");
		vigil_buf_quote(buf, evt->nick);
		break;
	case CHAT_EVENT_LEFT:
		vigil_buf_appendf(buf, ",\"type\":\"left\",\"nick\":");
		vigil_buf_quote(buf, evt->nick);
		break;
	case CHAT_EVENT_DATA:
		vigil_buf_appendf(buf, ",\"type\":\"data\",\"kind\":\"%s\""
		    ",\"token\":%" PRIu64 ",\"size\":%zu,\"name\":",
		    chat_data_kind_name(evt->kind), evt->value, evt->size);
		vigil_buf_quote(buf, evt->text);
		vigil_buf_appendf(buf, ",\"nick\":");
		vigil_buf_quote(buf, evt->nick);
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

	chat_lock();

	while ((evt = TAILQ_FIRST(&events)) != NULL) {
		TAILQ_REMOVE(&events, evt, list);
		nyfe_mem_zero(evt, sizeof(*evt));
		free(evt);
	}

	event_count = 0;

	chat_unlock();
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

	chat_lock();

	*cmd = TAILQ_FIRST(&commands);

	if (*cmd != NULL) {
		TAILQ_REMOVE(&commands, *cmd, list);
		command_count--;
	}

	chat_unlock();
}

/*
 * Returns the list that the messages waiting for an ack from the peer of
 * the given tunnel are on.
 */
static void
chat_outbox_lookup(struct chat_tunnel *tun, struct chat_msg_list **list)
{
	PRECOND(tun != NULL);
	PRECOND(list != NULL);

	if (tun->group != 0)
		*list = &tun->waitq;
	else
		*list = &tun->peer->outbox;
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
		chat_lock();
		blob_bytes -= blob->size;
		TAILQ_REMOVE(&blobs, blob, list);
		chat_unlock();
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
	struct chat_msg			*msg;
	struct vigil_chat_data_head	*head;
	char				what[VIGIL_CHAT_DATA_NAME_MAX + 16];

	PRECOND(blob != NULL);
	PRECOND(blob->size > sizeof(*head));

	msg = calloc(1, sizeof(*msg));
	if (msg == NULL)
		fatal("calloc: failed to allocate chat message");

	head = (struct vigil_chat_data_head *)blob->data;

	if (head->kind == VIGIL_CHAT_DATA_IMAGE) {
		(void)snprintf(what, sizeof(what), "an image");
	} else if (head->kind == VIGIL_CHAT_DATA_VOICE) {
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
 * Queue the given data for every member of our group, like we do for
 * messages to it with chat_group_send().
 */
static void
chat_group_data(u_int64_t ref, struct chat_blob *blob)
{
	int			idx;
	struct chat_tunnel	*tun;
	struct chat_msg		*msg;
	struct chat_tally	*tally;

	PRECOND(blob != NULL);

	tally = NULL;

	for (idx = 0; idx < KYRKA_PEERS_PER_FLOCK; idx++) {
		tun = peers[idx].group;
		if (tun == NULL)
			continue;

		if (tally == NULL) {
			tally = calloc(1, sizeof(*tally));
			if (tally == NULL)
				fatal("calloc: failed to allocate tally");

			tally->ref = ref;
			tally->report = 1;
			tally->group = discovery.group;

			TAILQ_INSERT_TAIL(&tallies, tally, list);
		}

		msg = chat_data_msg(ref, blob);
		msg->tally = tally;

		TAILQ_INSERT_TAIL(&tun->waitq, msg, wlist);
		chat_msg_queue(tun, msg);

		tally->pending++;
	}
}

/*
 * Start the xfer for the peer if it indicated it can take more.
 */
static void
chat_xfer_run(struct chat_tunnel *tun)
{
	struct chat_msg		*msg;
	struct chat_msg_list	*list;

	PRECOND(tun != NULL);

	if (tun->xfer == NULL) {
		if (!tun->heard)
			return;

		chat_outbox_lookup(tun, &list);
		TAILQ_FOREACH(msg, list, wlist) {
			if (msg->blob == NULL || msg->id == 0)
				continue;

			if (tun->features & VIGIL_CHAT_FEATURE_DATA)
				break;

			chat_data_gospel(tun, msg);
		}

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

		memset(xfer->acked, 0, (count + 7) / 8);
		memset(xfer->sent, 0, count * sizeof(*xfer->sent));

		xfer->base = 0;
		xfer->next = 0;
		xfer->credit = 0;
		xfer->rtt_min = 0;
		xfer->rtt = CHAT_RTT_START;
		xfer->rate = CHAT_RATE_START;
		xfer->last_ack = vigil->now;
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

	if (tun->group == 0 && xfer->msg->sent == 0) {
		if (chat_event_queue(CHAT_EVENT_SENT,
		    tun->peer, 0, xfer->msg->ref, NULL, 0) != -1)
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
	struct chat_seen		*seen;
	struct vigil_chat_data_head	*head;
	struct chat_parcel		*parcel;

	PRECOND(tun != NULL);
	PRECOND(xfer != NULL);
	PRECOND(xfer->data != NULL);
	PRECOND(xfer->size > sizeof(*head));

	head = (struct vigil_chat_data_head *)xfer->data;

	if (chat_data_kind_valid(head->kind) == -1) {
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

	chat_lock();

	if ((parcel_bytes + xfer->size) > CHAT_PARCELS_MAX) {
		chat_unlock();
		vigil_log(LOG_NOTICE, "[chat] %02x data has to wait",
		    tun->peer->id);
		return (-1);
	}

	chat_unlock();

	parcel = calloc(1, sizeof(*parcel));
	if (parcel == NULL)
		fatal("calloc: failed to allocate data");

	parcel->data = xfer->data;
	parcel->size = xfer->size;
	parcel->age = vigil->now;
	parcel->token = ++tokens;

	xfer->data = NULL;

	if (chat_data_report(tun, parcel) == -1) {
		chat_parcel_free(parcel);
		return (-1);
	}

	chat_lock();
	parcel_bytes += parcel->size;
	TAILQ_INSERT_TAIL(&parcels, parcel, list);
	chat_unlock();

	seen = &tun->seen[tun->seen_next++ % CHAT_SEEN_MAX];

	seen->hash = 0;
	seen->id = xfer->id;
	seen->size = xfer->size;
	seen->token = parcel->token;
	seen->age = vigil->now;

	if (tun->group == 0)
		tun->peer->active = vigil->now;

	vigil_log(LOG_INFO, "[chat] %02x sent us data of %zu bytes",
	    tun->peer->id, parcel->size);

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

	evt->kind = head->kind;
	evt->group = tun->group;
	evt->peer = tun->peer->id;
	evt->value = parcel->token;
	evt->type = CHAT_EVENT_DATA;
	evt->size = parcel->size - sizeof(*head);

	memcpy(evt->text, head->name, head->len);
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

	chat_lock();

	for (parcel = TAILQ_FIRST(&parcels); parcel != NULL; parcel = next) {
		next = TAILQ_NEXT(parcel, list);

		if ((vigil->now - parcel->age) < CHAT_DATA_WAIT)
			continue;

		TAILQ_REMOVE(&parcels, parcel, list);
		parcel_bytes -= parcel->size;
		chat_parcel_free(parcel);
	}

	chat_unlock();
}

/*
 * Drop all parcels that arrived.
 */
static void
chat_parcels_free(void)
{
	struct chat_parcel	*parcel;

	chat_lock();

	while ((parcel = TAILQ_FIRST(&parcels)) != NULL) {
		TAILQ_REMOVE(&parcels, parcel, list);
		chat_parcel_free(parcel);
	}

	parcel_bytes = 0;
	chat_unlock();
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
 * Take the lock that protects our commands, events, parcels and blobs.
 */
static void
chat_lock(void)
{
	if (pthread_mutex_lock(&lock) != 0)
		fatal("failed to take the chat lock");
}

/*
 * Release the lock that protects our commands, events, parcels and blobs.
 */
static void
chat_unlock(void)
{
	if (pthread_mutex_unlock(&lock) != 0)
		fatal("failed to release the chat lock");
}
