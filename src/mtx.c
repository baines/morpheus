#include <yajl/yajl_tree.h>
#include <yajl/yajl_gen.h>
#include <string.h>
#include <stdbool.h>
#include "morpheus.h"

#define yajl_gen_strlit(j, str) yajl_gen_string(j, str, sizeof(str)-1)

#define MTX_SET_URL(client, msg, fmt, ...) ({\
	char* url;\
	assert(asprintf(&url, "%s" MTX_CLIENT fmt "?access_token=%s", global.mtx_server_base_url, ##__VA_ARGS__, client->mtx_token) != -1);\
	curl_easy_setopt(msg->curl, CURLOPT_URL, url);\
	free(url);\
})

#define cprintf(fmt, ...) printf("[%02d] " fmt, client->irc_sock, ##__VA_ARGS__)
#define net_msg_perror(msg, fmt, ...) cprintf(fmt " FAIL: [%ld] [%s] [%s]\n", ##__VA_ARGS__, msg->curl_status, msg->errbuf, msg->data)

struct pm_data {
	mtx_id friend;
	char message[];
};

static void mtx_send_pm_create_room (struct client*, struct pm_data* data);

const char* mtx_msg_strs[] = {
	[MTX_MSG_SYNC]      = "SYNC",
	[MTX_MSG_LOGIN]     = "LOGIN",
	[MTX_MSG_MSG]       = "MSG",
	[MTX_MSG_TOPIC]     = "TOPIC",
	[MTX_MSG_JOIN]      = "JOIN",
	[MTX_MSG_LEAVE]     = "LEAVE",
	[MTX_MSG_PM_LOOKUP] = "PM_LOOKUP",
	[MTX_MSG_PM_CREATE] = "PM_CREATE",
};

void mtx_recv_sync(struct client* client, struct net_msg* msg){
	yajl_val root  = yajl_tree_parse(msg->data, NULL, 0);

	yajl_val since    = YAJL_GET(root, yajl_t_string, ("next_batch"));
	yajl_val presence = YAJL_GET(root, yajl_t_array , ("presence", "events"));
	yajl_val joins    = YAJL_GET(root, yajl_t_object, ("rooms", "join"));
	yajl_val leaves   = YAJL_GET(root, yajl_t_object, ("rooms", "leave"));
	yajl_val invites  = YAJL_GET(root, yajl_t_object, ("rooms", "invite"));

	if(presence){
		for(size_t i = 0; i < presence->u.array.len; ++i){
			yajl_val obj = presence->u.array.values[i];
			if(!YAJL_IS_OBJECT(obj)) continue;

			yajl_val type = YAJL_GET(obj, yajl_t_string, ("type"));
			if(!type || strcmp(type->u.string, "m.presence") != 0) continue;

			yajl_val status = YAJL_GET(obj, yajl_t_string, ("content", "presence"));
			yajl_val user   = YAJL_GET(obj, yajl_t_string, ("sender"));
			yajl_val ago    = YAJL_GET(obj, yajl_t_number, ("content", "last_active_ago"));

			if(!user) continue;
			mtx_id user_id = id_intern(user->u.string);

			if(status && (client->irc_caps & IRC_CAP_AWAY_NOTIFY)){
				const char* away_msg =
					strcmp(status->u.string, "unavailable") == 0 ? "Idle" :
					strcmp(status->u.string, "online")      == 0 ? NULL :
					"Offline";

				if(presence_update(client, user_id, status->u.string)){
					cprintf("New presence for [%s]: %s\n", user->u.string, away_msg ?: "Online");
					if(away_msg){
						IRC_SEND_PF(client, user->u.string, SF_CVT_PREFIX, "AWAY", away_msg);
					} else {
						IRC_SEND_PF(client, user->u.string, SF_CVT_PREFIX, "AWAY");
					}
				}
			}

			if(!client->last_active && YAJL_IS_INTEGER(ago) && user_id == client->mtx_id){
				// XXX: this is likely wrong, ago gets updated when we login? :(
				//      figure out if our last active time is available somewhere else?
				client->last_active = time(NULL) - (ago->u.number.i / 1000);
			}
		}
	}

	for(size_t i = 0; joins && i < joins->u.object.len; ++i){
		const char* room = joins->u.object.keys[i];
		yajl_val obj = joins->u.object.values[i];
		mtx_id room_id = id_intern(room);

		cprintf("Processing events for [%s] (join)\n", room);

		struct sync_state state = {
			.room   = room_new(room_id),
			.client = client,
		};

		room_member_add(state.room, client->mtx_id, MEMBER_STATE_JOINED);

		bool known_to_irc = false;
		sb_each(r, client->irc_rooms){
			if(*r == room_id){
				known_to_irc = true;
				break;
			}
		}

		if(!known_to_irc){
			sb_push(client->irc_rooms, room_id);
			state.flags |= SYNC_NEW_ROOM;
		}

		yajl_val events;

		// state events
		events = YAJL_GET(obj, yajl_t_array, ("state", "events"));
		if(events){
			for(size_t j = 0; j < events->u.array.len; ++j){
				if(!YAJL_IS_OBJECT(events->u.array.values[j])) continue;

				yajl_val type = YAJL_GET(events->u.array.values[j], yajl_t_string, ("type"));
				if(!type) continue;

				//cprintf("Join state: [%s]\n", type->u.string);
				mtx_event(type->u.string, &state, events->u.array.values[j]);
			}
		}

		// FIXME: should we / can we do this after the timeline events?
		char* irc_room = NULL;
		int   irc_room_type = room_get_irc_info(state.room, client, &irc_room);

		if(!known_to_irc && irc_room_type > ROOM_IRC_QUERY){
			IRC_SEND_PF(client, id_lookup(client->mtx_id), SF_CVT_PREFIX, "JOIN", irc_room);
			irc_send_names(client, state.room);
		}

		// timeline events
		state.flags |= SYNC_TIMELINE;
		events = YAJL_GET(obj, yajl_t_array, ("timeline", "events"));
		if(events){
			for(size_t j = 0; j < events->u.array.len; ++j){
				if(!YAJL_IS_OBJECT(events->u.array.values[j])) continue;

				yajl_val type = YAJL_GET(events->u.array.values[j], yajl_t_string, ("type"));
				if(!type) continue;

				//cprintf("Join timel: [%s]\n", type->u.string);
				mtx_event(type->u.string, &state, events->u.array.values[j]);
			}
		}

		if(state.new_topic && irc_room){
			yajl_val topic  = YAJL_GET(state.new_topic, yajl_t_string, ("content", "topic"));
			yajl_val sender = YAJL_GET(state.new_topic, yajl_t_string, ("sender"));
			yajl_val epoch  = YAJL_GET(state.new_topic, yajl_t_number, ("origin_server_ts"));

			if(topic && sender && YAJL_IS_INTEGER(epoch)){
				char epoch_str[32] = "";
				snprintf(epoch_str, sizeof(epoch_str), "%zu", (size_t)epoch->u.number.i / 1000);

				mtx_id sender_id = id_intern(sender->u.string);
				char* hostmask = cvt_m2i_user(sender_id);

				IRC_SEND_NUM(client, "332", irc_room, topic->u.string);
				IRC_SEND_NUM(client, "333", irc_room, hostmask, epoch_str);

				free(hostmask);
			}
		}

		free(irc_room);
	}

	for(size_t i = 0; leaves && i < leaves->u.object.len; ++i){
		const char* room = leaves->u.object.keys[i];
		mtx_id room_id = id_intern(room);

		cprintf("Processing events for [%s] (leave)\n", room);

		struct sync_state state = {
			.room   = room_new(room_id),
			.client = client,
		};

		char* irc_room = NULL;
		if(room_get_irc_info(state.room, client, &irc_room) > ROOM_IRC_QUERY){
			IRC_SEND_PF(client, id_lookup(client->mtx_id), SF_CVT_PREFIX, "PART", irc_room);
		}
		free(irc_room);
		room_member_del(state.room, client->mtx_id);

		sb_each(r, client->irc_rooms){
			if(*r == room_id){
				sb_erase(client->irc_rooms, r - client->irc_rooms);
				break;
			}
		}
	}

	for(size_t i = 0; invites && i < invites->u.object.len; ++i){
		const char* room = invites->u.object.keys[i];
		yajl_val obj = invites->u.object.values[i];
		mtx_id room_id = id_intern(room);

		cprintf("Processing events for [%s] (invite)\n", room);

		struct sync_state state = {
			.room   = room_new(room_id),
			.client = client,
			.flags  = SYNC_INVITE,
		};

		yajl_val events = YAJL_GET(obj, yajl_t_array, ("invite_state", "events"));

		if(events){
			for(size_t j = 0; j < events->u.array.len; ++j){
				if(!YAJL_IS_OBJECT(events->u.array.values[j])) continue;

				yajl_val type = YAJL_GET(events->u.array.values[j], yajl_t_string, ("type"));
				if(!type) continue;

				mtx_event(type->u.string, &state, events->u.array.values[j]);
			}
		}

		// XXX: I would like to do this logic, but the invite state doesn't seem to always include
		//      any info about the canonical_alias or other members.
		//      If there is a way to get that before joining, then I'd like to know...
#if 0
		int room_type;
		char* irc_room = room_get_irc_name(state.room, client, &room_type);

		if(room_type == ROOM_IRC_QUERY){
			// auto accept the invite if it's a private message room
			// TODO: we should probably send a NOTICE or something about this?
			mtx_send_join(client, room);
		} else if(irc_room){
			// otherwise send an IRC invite
			assert(state.inviter);
			IRC_SEND_PF(client, state.inviter, SF_CVT_PREFIX, "INVITE", client->irc_nick, irc_room);
		}

		free(irc_room);
#else
		mtx_send_join(client, room);
#endif
	}

#if 0
	FILE* f = fopen("debug.json", "r+");
	if(!f){
		f = fopen("debug.json", "w");
		fputs(msg->data, f);
	}
	fclose(f);
#endif

	//cprintf("sync %d [%s]\n", client->irc_sock, msg->data);

	if(since){
		free(client->mtx_since);
		client->mtx_since = strdup(since->u.string);
	}

	client->last_sync = time(NULL);

	yajl_tree_free(root);
}

void mtx_recv(struct client* client, struct net_msg* msg){

	time_t now = time(NULL);
	yajl_val root = yajl_tree_parse(msg->data, NULL, 0);

	switch(msg->type){

		case MTX_MSG_LOGIN: {

			// TODO: track state, we should only get one login?
			if(msg->curl_status == 200){
				yajl_val tkn  = YAJL_GET(root, yajl_t_string, ("access_token"));
				yajl_val uid  = YAJL_GET(root, yajl_t_string, ("user_id"));
				yajl_val serv = YAJL_GET(root, yajl_t_string, ("home_server"));
				yajl_val dev  = YAJL_GET(root, yajl_t_string, ("device_id"));

				if(tkn && uid){
					client->mtx_token  = strdup(tkn->u.string);
					client->mtx_id     = id_intern(uid->u.string);
					client->mtx_server = strdup(serv->u.string);
					client->irc_state |= IRC_STATE_REGISTERED;

					mtx_send_sync(client);

					IRC_SEND_NUM(client, "001", "Welcome to IRC");
					IRC_SEND_NUM(client, "002", "Your device_id is", dev ? dev->u.string : "unknown");
					IRC_SEND_NUM(client, "003", "This server was created at some point");
					IRC_SEND_NUM(client, "004", "morpheus 1.0 ¯\\_(ツ)_/¯");
					IRC_SEND_NUM(client, "005", "PREFIX=(ohv)@%+ CHANTYPES=#!+", "are supported by this server");

				} else {
					msg->curl_status = 0;
				}
			}

			if(msg->curl_status == 403){
				IRC_SEND_NUM(client, "464", "Password incorrect");
			} else if(msg->curl_status != 200){
				net_msg_perror(msg, "LOGIN");
				IRC_SEND(client, "NOTICE", "*", "Internal Server Error");
			}
		} break;

		case MTX_MSG_SYNC: {
			// TODO: handle the different possible error statuses separately
			if(msg->curl_status == 200){
				mtx_recv_sync(client, msg);
				mtx_send_sync(client);
			} else {
				net_msg_perror(msg, "SYNC");
				IRC_SEND(client, "NOTICE", client->irc_nick, "Matrix sync failed D:");
				client->next_sync = now + 10;
			}
		} break;

		case MTX_MSG_MSG: {
			if(msg->curl_status == 200){
				yajl_val id = YAJL_GET(root, yajl_t_string, ("event_id"));
				if(id){
					sb_push(client->mtx_sent_ids, strdup(id->u.string));
				}
			} else {
				net_msg_perror(msg, "MSG");
				IRC_SEND(client, "NOTICE", client->irc_nick, "Failed to send message.");
			}
		} break;

		case MTX_MSG_JOIN: {
			if(msg->curl_status == 200){
				yajl_val room_id = YAJL_GET(root, yajl_t_string, ("room_id"));
				if(room_id){
					cprintf("Joined [%s]\n", room_id->u.string);
					room_new(id_intern(room_id->u.string));
				}
			} else {
				yajl_val err = YAJL_GET(root, yajl_t_string, ("errcode"));
				// TODO: if this was an auto-join, don't send stuff...
				//       should user_data be null in this case?
				if(err){
					// TODO: can we get M_FORBIDDEN when we're banned too?
					if(strcmp(err->u.string, "M_FORBIDDEN") == 0){
						IRC_SEND_NUM(client, "473", msg->user_data, "Cannot join channel (+i)");
					} else {
						net_msg_perror(msg, "JOIN");
						IRC_SEND(client, "NOTICE", client->irc_nick, "Error joining channel");
					}
				}
			}
			free(msg->user_data);
		} break;

		case MTX_MSG_LEAVE: {
			if(msg->curl_status == 200){
				cprintf("TODO: leave ok, tell IRC\n");
			} else {
				char* url = NULL;
				curl_easy_getinfo(msg->curl, CURLINFO_EFFECTIVE_URL, &url);
				net_msg_perror(msg, "LEAVE");
			}
		} break;

		case MTX_MSG_TOPIC: {
			if(msg->curl_status != 200){
				// TODO: send error to IRC
				net_msg_perror(msg, "TOPIC");
			}
		} break;

		case MTX_MSG_PM_LOOKUP: {
			struct pm_data* data = msg->user_data;
			assert(data);

			if(msg->curl_status == 200){
				// TODO: check if room was created in the meantime?
				mtx_send_pm_create_room(client, data);
			} else {
				char* who = cvt_m2i_user(data->friend);
				*strchrnul(who, '!') = 0;
				IRC_SEND_NUM(client, "401", who, "No such nick/channel.");
				free(who);

				net_msg_perror(msg, "PM_LOOKUP");
			}
		} break;

		case MTX_MSG_PM_CREATE: {
			struct pm_data* data = msg->user_data;
			assert(data);

			yajl_val room = YAJL_GET(root, yajl_t_string, ("room_id"));
			char buf[256] = "";

			if(msg->curl_status == 200){
				assert(room);

				// TODO: notify of room creation in a nice way
				//snprintf(buf, sizeof(buf), "Created room [%s]", room->u.string);
				//IRC_SEND_PF(client, id_lookup(data->friend), SF_CVT_PREFIX, "NOTICE", "bob", buf);

				struct room* r = room_new(id_intern(room->u.string));
				mtx_send_msg(client, r, data->message);
			} else {
				// TODO: proper error message
				snprintf(buf, sizeof(buf), "RIP IN PIECES: [%ld]", msg->curl_status); 
				IRC_SEND(client, "NOTICE", client->irc_nick, "Error sending PM");
				net_msg_perror(msg, "PM_CREATE");
			}

			free(data);
		} break;

		default: {
			cprintf("Unhandled message: [%d] [%ld] [%s]\n", msg->type, msg->curl_status, msg->data);
		} break;
	}

	yajl_tree_free(root);
}

void mtx_send_login(struct client* client){
	struct net_msg* msg = net_msg_new(client, MTX_MSG_LOGIN);
	
	char* url;
	asprintf(&url, "%s" MTX_CLIENT "/login", global.mtx_server_base_url);
	curl_easy_setopt(msg->curl, CURLOPT_URL, url);
	free(url);

	assert(client->irc_user);
	assert(client->irc_pass);

	char* json = NULL;
	yajl_generate(
		&json,
		"{ "
		"'type': 'm.login.password', "
		"'user': %s, 'password': %s, "
		"'device_id': %s, "
		"'initial_device_display_name': %s, "
		"}",
		client->irc_nick,
		client->irc_pass,
		global.device_id,
		global.device_name
	);

	curl_easy_setopt(msg->curl, CURLOPT_COPYPOSTFIELDS, json);
	free(json);

	{
		volatile char *p = client->irc_pass;
		while(*p) *p++ = 0;
		free(client->irc_pass);
		client->irc_pass = NULL;
	}

	net_msg_send(msg);
}

void mtx_send_logout(struct client* client){

}

void mtx_send_sync(struct client* client){
	struct net_msg* msg = net_msg_new(client, MTX_MSG_SYNC);

	char* filter = curl_easy_escape(
		msg->curl,
		"{ \"room\": { \"ephemeral\": { \"not_types\": [ \"*\" ] }}}",
		0
	);

	// XXX: I would like to poll for longer, but anything over ~60s seems to time out with nginx

	char* url;
	asprintf(
		&url,
		"%s" MTX_CLIENT "/sync?timeout=55000%s%s&access_token=%s&filter=%s",
		global.mtx_server_base_url,
		client->mtx_since ? "&since=" : "&full_state=true",
		client->mtx_since ?: "",
		client->mtx_token,
		filter
	);

	curl_free(filter);
	curl_easy_setopt(msg->curl, CURLOPT_URL, url);
	free(url);

	net_msg_send(msg);
}

void mtx_send_msg(struct client* client, struct room* room, const char* user_msg){
	struct net_msg* msg = net_msg_new(client, MTX_MSG_MSG);
	MTX_SET_URL(client, msg, "/rooms/%s/send/m.room.message/%zu", id_lookup(room->id), client->mtx_txid++);

	bool is_emote = false;
	if(strncmp(user_msg, "\001ACTION ", 8) == 0){
		size_t user_msg_len = strlen(user_msg + 8);
		if(user_msg_len > 0 && user_msg[user_msg_len + 7] == '\001'){
			user_msg = strndupa(user_msg + 8, user_msg_len - 1);
			is_emote = true;
		}
	}

	sb(char) stripped = NULL;
	sb(char) html = cvt_i2m_msg(user_msg, &stripped);

	cprintf("Sending msg: [%.*s] [%.*s]\n", (int)sb_count(stripped), stripped, (int)sb_count(html), html);

	curl_easy_setopt(msg->curl, CURLOPT_CUSTOMREQUEST, "PUT");

	char* json = NULL;
	yajl_generate(
		&json,
		"{ "
		"'msgtype': %s, "
		"'body': %z, "
		"'format': 'org.matrix.custom.html', "
		"'formatted_body': %z "
		"}",
		is_emote ? "m.emote" : "m.text",
		sb_count(stripped), stripped,
		sb_count(html), html
	);

	cprintf("MSG JSON = [%s]\n", json);
	curl_easy_setopt(msg->curl, CURLOPT_COPYPOSTFIELDS, json);
	free(json);

	sb_free(html);
	sb_free(stripped);

	net_msg_send(msg);
}

void mtx_send_topic(struct client* client, struct room* room, const char* topic){
	struct net_msg* msg = net_msg_new(client, MTX_MSG_TOPIC);
	MTX_SET_URL(client, msg, "/rooms/%s/send/m.room.topic/%zu", id_lookup(room->id), client->mtx_txid++);
	curl_easy_setopt(msg->curl, CURLOPT_CUSTOMREQUEST, "PUT");

	// TODO: can this do html colour stuff?
	sb(char) stripped = NULL;
	sb(char) html = cvt_i2m_msg(topic, &stripped);

	char* json = NULL;
	yajl_generate(&json, "{ 'topic': %z }", sb_count(stripped), stripped);
	curl_easy_setopt(msg->curl, CURLOPT_COPYPOSTFIELDS, json);
	free(json);

	sb_free(html);
	sb_free(stripped);

	net_msg_send(msg);
}

void mtx_send_join(struct client* client, const char* room){
	struct net_msg* msg = net_msg_new(client, MTX_MSG_JOIN);
	assert(room);

	char* r = curl_easy_escape(msg->curl, room, 0);

	if(room[0] == '#'){
		msg->user_data = strdup(room);
		MTX_SET_URL(client, msg, "/join/%s%%3A%s", r, client->mtx_server);
	} else if(room[0] == '!'){ // XXX: what happens if we get a ! join to a different host?
		MTX_SET_URL(client, msg, "/join/%s", r);
	}

	curl_easy_setopt(msg->curl, CURLOPT_POSTFIELDS, "{}");

	curl_free(r);
	net_msg_send(msg);
}

void mtx_send_leave(struct client* client, struct room* room){
	assert(room->id);

	struct net_msg* msg = net_msg_new(client, MTX_MSG_LEAVE);
	char* r = curl_easy_escape(msg->curl, id_lookup(room->id), 0);

	MTX_SET_URL(client, msg, "/rooms/%s/leave", r);
	curl_easy_setopt(msg->curl, CURLOPT_POSTFIELDS, "{}");

	// TODO: should we call /forget too?

	curl_free(r);
	net_msg_send(msg);
}

void mtx_send_pm_setup(struct client* client, mtx_id mtx_user, const char* text){
	struct net_msg* msg = net_msg_new(client, MTX_MSG_PM_LOOKUP);

	struct pm_data* data = malloc(sizeof(*data) + strlen(text) + 1);
	data->friend = mtx_user;
	strcpy(data->message, text);
	msg->user_data = data;

	char* u = curl_easy_escape(msg->curl, id_lookup(mtx_user), 0);
	MTX_SET_URL(client, msg, "/profile/%s", u);

	curl_free(u);
	net_msg_send(msg);
}

static void mtx_send_pm_create_room(struct client* client, struct pm_data* data){
	struct net_msg* msg = net_msg_new(client, MTX_MSG_PM_CREATE);
	msg->user_data = data;

	MTX_SET_URL(client, msg, "/createRoom");

	char* json = NULL;
	yajl_generate(
		&json,
		"{ "
		"'preset': 'private_chat', "
		"'creation_content': { 'm.federate': false }, "
		"'invite': [ %s ], "
		"}",
		id_lookup(data->friend)
	);

	curl_easy_setopt(msg->curl, CURLOPT_POSTFIELDS, json);
	net_msg_send(msg);
}
