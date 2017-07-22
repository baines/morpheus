#include "morpheus.h"

static void irc_event_ping(struct client* client, struct irc_msg* msg){
	char buf[256];
	int len = snprintf(buf, sizeof(buf), ":morpheus PONG :%s\r\n", msg->params[0] ?: "");
	send(client->irc_sock, buf, len, 0);
}

static void irc_event_pong(struct client* client, struct irc_msg* msg){
	// do nothing, successfully
}

static void irc_event_privmsg(struct client* client, struct irc_msg* msg){
	struct room* room;

	if(msg->params[0][0] == '#'){ // PRIVMSG to channel
		if((room = room_get_irc(msg->params[0]))){
			mtx_send_msg(client, room, msg->params[1]);
		} else {
			// TODO: it might exist, but we're not in it.. should we check?
			IRC_SEND_NUM(client, "401", msg->params[0], "No such nick/channel.");
		}
	} else if(msg->params[0][0] == '&'){ // TODO: group rooms

	} else { // PM to user
		mtx_id user = cvt_i2m_user(msg->params[0]);

		if((room = room_find_query(client, user))){
			mtx_send_msg(client, room, msg->params[1]);
		} else {
			// TODO: check if the user exists, if so, create room and invite them
			mtx_send_pm_setup(client, user, msg->params[1]);
			// upon creating room, send NOTICE from user stating room was created
			// XXX: we need to store the msg somewhere across messages
		}
	}
}

static void irc_event_join(struct client* client, struct irc_msg* msg){
	mtx_send_join(client, msg->params[0]);
}

static void irc_event_part(struct client* client, struct irc_msg* msg){
	struct room* room = room_get_irc(msg->params[0]);
	if(room){
		mtx_send_leave(client, room);
	} else {
		IRC_SEND_NUM(client, "403", msg->params[0], "No such channel.");
	}
}

static void irc_event_topic(struct client* client, struct irc_msg* msg){
	struct room* room = room_get_irc(msg->params[0]);
	
	// TODO: handle showing / removal of topic?
	if(room){
		mtx_send_topic(client, room, msg->params[1]);
	} else {
		IRC_SEND_NUM(client, "403", msg->params[0], "No such channel.");
	}
}

static void irc_event_mode(struct client* client, struct irc_msg* msg){
	struct room* room = room_get_irc(msg->params[0]);

	if(room && msg->pcount == 1){
		// TODO: use the other func to check?
		if(msg->params[0][0] == '#' || msg->params[0][0] == '&'){
			const char* mode = room->invite_only ? "+ni" : "+n";
			IRC_SEND_NUM(client, "324", msg->params[0], mode);
			if(room->created){
				char buf[32] = "";
				snprintf(buf, sizeof(buf), "%zu", (size_t)room->created);
				IRC_SEND_NUM(client, "329", msg->params[0], buf);
			}
		}
	}
}

static void irc_event_nick(struct client* client, struct irc_msg* msg){
	// TODO: check other nicks, see if it is available or not
	//       handle changing nick with matrix?
	free(client->irc_nick);
	client->irc_nick = strdup(msg->params[0]);
}

static void irc_event_user(struct client* client, struct irc_msg* msg){
	free(client->irc_user);
	client->irc_user = strdup(msg->params[0]);
}

static void irc_event_pass(struct client* client, struct irc_msg* msg){
	free(client->irc_pass);
	client->irc_pass = strdup(msg->params[0]);
}

static void irc_event_cap(struct client* client, struct irc_msg* msg){

	if(strcmp(msg->params[0], "LS") == 0){

		struct irc_msg m = {
			.cmd = "CAP",
			.params  = { client->irc_nick ?: "*", "LS", "server-time away-notify" },
			.pcount  = 3,
		};
		irc_send(client, &m);
		client->irc_state |= IRC_STATE_REG_SUSPEND;

	} else if(strcmp(msg->params[0], "LIST") == 0){

		char caps[64] = "";
		char* p = caps;

		if(client->irc_caps & IRC_CAP_SERVER_TIME) p = stpcpy(p, "server-time ");
		if(client->irc_caps & IRC_CAP_AWAY_NOTIFY) p = stpcpy(p, "away-notify ");

		struct irc_msg m = {
			.cmd = "CAP",
			.params  = { client->irc_nick ?: "*", "LIST", caps },
			.pcount  = 3,
		};
		irc_send(client, &m);

	} else if(strcmp(msg->params[0], "REQ") == 0 && msg->params[1]){

		char* state;
		int old_caps = client->irc_caps;

		char* list = strdupa(msg->params[1]);
		for(char* cap = strtok_r(list, " ", &state); cap; cap = strtok_r(NULL, " ", &state)){
			if(strcmp(cap, "server-time") == 0){
				client->irc_caps |= IRC_CAP_SERVER_TIME;
			} else if(strcmp(cap, "away-notify") == 0){
				client->irc_caps |= IRC_CAP_AWAY_NOTIFY;
			}
		}

		char caps[64] = "";
		char* p = caps;

		int cap_diff = old_caps ^ client->irc_caps;
		if(cap_diff){
			if(cap_diff & IRC_CAP_SERVER_TIME) p = stpcpy(p, "server-time ");
			if(cap_diff & IRC_CAP_AWAY_NOTIFY) p = stpcpy(p, "away-notify ");
			struct irc_msg m = {
				.cmd = "CAP",
				.params  = { client->irc_nick ?: "*", "ACK", caps },
				.pcount  = 3,
			};
			irc_send(client, &m);
		} else {
			struct irc_msg m = {
				.cmd = "CAP",
				.params  = { client->irc_nick ?: "*", "NAK", msg->params[1] },
				.pcount  = 3,
			};
			irc_send(client, &m);
		}
		client->irc_state |= IRC_STATE_REG_SUSPEND;

	} else if(strcmp(msg->params[0], "END") == 0){

		client->irc_state &= ~IRC_STATE_REG_SUSPEND;

	} else if(strcmp(msg->params[0], "ACK") != 0 && strcmp(msg->params[0], "NAK") != 0){

		struct irc_msg m = {
			.cmd = "410",
			.params  = { client->irc_nick ?: "*", msg->params[1] ?: "", "Invalid CAP cmd" },
			.pcount  = 3,
		};
		irc_send(client, &m);
	}

}

typedef void event_fn(struct client* client, struct irc_msg*);

enum {
	SF_NEED_REG   = (1 << 0),
	SF_NEED_UNREG = (1 << 1),
};

static struct irc_handler {
	const char* event;
	size_t      min_params;
	int         state_flags;
	event_fn*   func;
} irc_handlers[] = {
	{ "PING"    , 0, SF_NEED_REG  , &irc_event_ping },
	{ "PONG"    , 0, SF_NEED_REG  , &irc_event_pong },
	{ "PRIVMSG" , 2, SF_NEED_REG  , &irc_event_privmsg },
	{ "JOIN"    , 1, SF_NEED_REG  , &irc_event_join },
	{ "PART"    , 1, SF_NEED_REG  , &irc_event_part },
	{ "TOPIC"   , 2, SF_NEED_REG  , &irc_event_topic },
	{ "MODE"    , 1, SF_NEED_REG  , &irc_event_mode },
	{ "NICK"    , 1, 0            , &irc_event_nick },
	{ "USER"    , 3, SF_NEED_UNREG, &irc_event_user },
	{ "PASS"    , 1, SF_NEED_UNREG, &irc_event_pass },
	{ "CAP"     , 1, 0            , &irc_event_cap },
};

void irc_event(struct client* client, struct irc_msg* msg){
	bool found = false;

	for(size_t i = 0; i < countof(irc_handlers); ++i){
		struct irc_handler* h = irc_handlers + i;

		if(strcasecmp(msg->cmd, h->event) == 0){
			found = true;

			if(msg->pcount < h->min_params){
				IRC_SEND_NUM(client, "461", msg->cmd, "Not enough parameters");
			} else if((h->state_flags & SF_NEED_REG) && !(client->irc_state & IRC_STATE_REGISTERED)){
				IRC_SEND_NUM(client, "451", "You have not registered");
			} else if((h->state_flags & SF_NEED_UNREG) && (client->irc_state & IRC_STATE_REGISTERED)){
				IRC_SEND_NUM(client, "462", "Unauthorized command (already registered)");
			} else {
				client->irc_state &= ~IRC_STATE_IDLE;
				client->last_cmd_time = time(0);

				printf("Got IRC cmd [%s]\n", msg->cmd);

				h->func(client, msg);

				if(!(client->irc_state & (IRC_STATE_REGISTERED | IRC_STATE_REG_SUSPEND)) && client->irc_user && client->irc_nick && client->irc_pass){
					mtx_send_login(client);
				}
			}
			break;
		}
	}

	if(!found){
		IRC_SEND_NUM(client, "421", msg->cmd, "Unknown command");
	}
}
