#ifndef MORPHEUS_H_
#define MORPHEUS_H_
#include <curl/curl.h>
#include <yajl/yajl_tree.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "stb_sb.h"

struct client;
struct sock;
struct net_msg;
struct room;
struct sync_state;
struct irc_msg;

typedef uint32_t mtx_id;

struct client*  client_new        (int socket);
void            client_del        (struct client*);
void            client_tick       (void);

void            net_init          (void);
void            net_update        (int event_mask, struct sock*);
struct net_msg* net_msg_new       (struct client*, int type);
void            net_msg_send      (struct net_msg*);
void            net_msg_free      (struct net_msg*);

mtx_id          id_intern         (const char* id);
const char*     id_lookup         (mtx_id);

void            mtx_send_sync     (struct client*);
void            mtx_send_login    (struct client*);
void            mtx_send_msg      (struct client*, struct room*, const char* msg);
void            mtx_send_topic    (struct client*, struct room*, const char* topic);
void            mtx_send_join     (struct client*, const char* room);
void            mtx_send_leave    (struct client*, struct room*);
void            mtx_send_pm_setup (struct client*, mtx_id user, const char* text);
void            mtx_recv          (struct client*, struct net_msg*);
void            mtx_event         (const char* ev, struct sync_state*, yajl_val);

int             irc_send          (struct client*, struct irc_msg*);
void            irc_send_names    (struct client*, struct room*);
void            irc_recv          (struct client*, const char* buf, size_t n);
void            irc_event         (struct client*, struct irc_msg*);

struct room*    room_new          (mtx_id id);
struct room*    room_get_mtx      (mtx_id id);
struct room*    room_get_irc      (const char* chan);
void            room_free         (struct room*);
struct member*  room_member_get   (struct room*, mtx_id member_id);
struct member*  room_member_add   (struct room*, mtx_id member_id, int state);
void            room_member_del   (struct room*, mtx_id member_id);
char*           room_get_irc_name (struct room*, struct client*, int* type);
struct room*    room_find_query   (struct client*, mtx_id partner);

char*           cvt_m2i_user      (mtx_id id);
mtx_id          cvt_i2m_user      (const char* irc_id);
sb(char)        cvt_m2i_msg       (const char* mtx_msg);
sb(char)        cvt_i2m_msg       (const char* irc_msg, sb(char)* stripped);

bool            yajl_generate     (char** out, const char* fmt, ...);

#define NUM_ARGS(...) (sizeof((const void*[]){ __VA_ARGS__ })/sizeof(void*))

#define IRC_SEND_PF(client, pre, fl, command, ...) \
	irc_send(client, &(struct irc_msg){\
		.cmd = (command),\
		.prefix = (pre),\
		.params = { __VA_ARGS__ },\
		.pcount = NUM_ARGS(__VA_ARGS__),\
		.flags = (fl)\
	});

#define IRC_SEND_F(client, fl, command, ...)\
	IRC_SEND_PF((client), NULL, (fl), (command), __VA_ARGS__)

#define IRC_SEND(client, command, ...)\
	IRC_SEND_PF((client), NULL, 0, (command), __VA_ARGS__)

#define IRC_SEND_NUM(client, num, ...)\
	IRC_SEND((client), (num), client->irc_nick ?: "*", __VA_ARGS__)

// For discriminating epoll fds
enum {
	EPOLL_TAG_CURL,
	EPOLL_TAG_CURL_TIMER,
	EPOLL_TAG_IRC_LISTEN,
	EPOLL_TAG_IRC_CLIENT,
	EPOLL_TAG_IRC_TIMER,
};

// For discriminating which type of message a net_msg struct refers to
enum {
	MTX_MSG_LOGIN,
	MTX_MSG_SYNC,
	MTX_MSG_MSG,
	MTX_MSG_TOPIC,
	MTX_MSG_JOIN,
	MTX_MSG_LEAVE,
	
	MTX_MSG_PM_LOOKUP,
	MTX_MSG_PM_CREATE,
};

// For client->irc_state
enum {
	IRC_STATE_REGISTERED  = (1 << 0),
	IRC_STATE_IDLE        = (1 << 1), // have sent PING, need PONG
	IRC_STATE_REG_SUSPEND = (1 << 2),
};

// For client->irc_caps
enum {
	IRC_CAP_SERVER_TIME = (1 << 0),
	IRC_CAP_AWAY_NOTIFY = (1 << 1),
};

// For mtx sync_state flags
enum {
	SYNC_TIMELINE = (1 << 0),
	SYNC_NEW_ROOM = (1 << 1),
	SYNC_INVITE   = (1 << 2),
};

// For irc_msg_send, to know operations to apply to the irc_msg struct before sending.
enum {
	SF_CVT_PREFIX  = (1 << 0),
	SF_CVT_ROOM_P0 = (1 << 1),
	SF_CVT_ROOM_P1 = (1 << 2),
};

// Returned by room_get_irc_name's type argument
enum {
	ROOM_IRC_INVALID = -1,
	ROOM_IRC_CHANNEL,
	ROOM_IRC_QUERY,
	ROOM_IRC_GROUP,
};

// Used in struct member, to show a room member's status w.r.t that room.
enum {
	MEMBER_STATE_JOINED,
	MEMBER_STATE_INVITED,
};

struct sock {
	int tag;
	int fd;
};

struct net_msg {
	int   type;
	CURL* curl;
	char* data;
	void* user_data;
	struct curl_slist* headers;
	struct net_msg* next;
};

struct member {
	mtx_id id;
	int state;
	int power;
};

struct room {
	mtx_id id;
	char* canon;
	sb(struct member) members;
	bool invite_only;
	time_t created;
	// TODO:
	// power level for OP, HOP etc
	// other aliases?
};

struct sync_state {
	struct room* room;
	struct client* client;
	yajl_val new_topic;
	int flags;

	const char* inviter;
};

struct irc_msg {
	const char* tags;
	const char* prefix;
	const char* cmd;
	const char* params[16];
	size_t pcount;
	int flags;
};

struct client {
	char* irc_user;
	char* irc_nick;
	char* irc_pass;
	int   irc_sock;
	int   irc_caps;
	int   irc_state;

	sb(char)   irc_buf;      // space for buffering incoming IRC messages
	sb(mtx_id) irc_rooms;    // room IDs that we are joined to in IRC
	sb(char*)  mtx_sent_ids; // to prevent echo of our own mtx events

	char* mtx_token;
	char* mtx_since;
	char* mtx_server;

	mtx_id mtx_id;

	size_t mtx_txid;

	time_t connect_time;
	time_t last_cmd_time;
	time_t last_active;

	int epoll_irc_tag;

	struct net_msg* msgs;
	struct client* next;
};

extern struct global_state {
	const char* mtx_server_base_url;
	const char* mtx_server_name;
	int epoll;
} global;

#define container_of(ptr, type, member) ({            \
	const typeof(((type*)0)->member)* __mptr = (ptr); \
	(type*)((char*)__mptr - offsetof(type, member));  \
})

#define countof(x) (sizeof(x)/sizeof(*x))

#define YAJL_PATH_EXPAND(...) { __VA_ARGS__, NULL }
#define YAJL_GET(root, type, path) ({               \
	static const char* p[] = YAJL_PATH_EXPAND path; \
	yajl_tree_get((root), p, (type));     \
})

#define ISDIGIT(x)    (x >= '0' && x <= '9')
#define ISLETTER(x) ((x >= 'A' && x <= 'Z') || (x >= 'a' && x <= 'z'))

#define MTX_CLIENT "/_matrix/client/r0"

#endif
