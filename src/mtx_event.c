#include <yajl/yajl_tree.h>
#include <yajl/yajl_gen.h>
#include <string.h>
#include <stdbool.h>
#include "morpheus.h"

static void mtx_event_message(struct sync_state* state, yajl_val obj){

	yajl_val type   = YAJL_GET(obj, yajl_t_string, ("content", "msgtype"));
	yajl_val body   = YAJL_GET(obj, yajl_t_string, ("content", "body"));
	yajl_val fmt    = YAJL_GET(obj, yajl_t_string, ("content", "format"));
	yajl_val fbody  = YAJL_GET(obj, yajl_t_string, ("content", "formatted_body"));
	yajl_val sender = YAJL_GET(obj, yajl_t_string, ("sender"));
	yajl_val id     = YAJL_GET(obj, yajl_t_string, ("event_id"));
	yajl_val ts     = YAJL_GET(obj, yajl_t_number, ("origin_server_ts"));

	if(YAJL_IS_INTEGER(ts) && (ts->u.number.i / 1000) < state->client->last_active){
		// They've probably already seen this message, skip it
		return;
	}

	bool our_msg = false;
	if(id){
		sb_each(i, state->client->mtx_sent_ids){
			if(strcmp(*i, id->u.string) == 0){
				our_msg = true;
				free(*i);
				sb_erase(state->client->mtx_sent_ids, i - state->client->mtx_sent_ids);
				break;
			}
		}
	}

	char* room_name = NULL;
	room_get_irc_info(state->room, state->client, &room_name);

	if(!our_msg && type && body && sender){

		char* body_str;
		bool rich;
		if(fmt && strcmp(fmt->u.string, "org.matrix.custom.html") == 0 && fbody){
			body_str = fbody->u.string;
			rich = true;
		} else {
			body_str = body->u.string;
			rich = false;
		}

		static const char* msgtypes[] = {
			"m.video", "m.image", "m.file", "m.audio"
		};

		// TODO: m.location

		char* msg = NULL;
		bool is_notice = strcmp(type->u.string, "m.notice") == 0;

		if(is_notice || strcmp(type->u.string, "m.text") == 0){
			msg = strdup(body_str);
		} else if(strcmp(type->u.string, "m.emote") == 0){
			asprintf(&msg, "\001ACTION %s\001", body_str);
		} else {

			yajl_val media_url  = YAJL_GET(obj, yajl_t_string, ("content", "url"));
			yajl_val media_mime = YAJL_GET(obj, yajl_t_string, ("content", "info", "mimetype"));

			if(media_url && media_mime && strncmp(media_url->u.string, "mxc://", 6) == 0){
				for(size_t i = 0; i < countof(msgtypes); ++i){
					if(strcmp(type->u.string, msgtypes[i]) == 0){
						asprintf(
							&msg,
							"\002[\0039%s\003]\002 %s: %s/_matrix/media/r0/download/%s",
							media_mime->u.string,
							body_str,
							global.mtx_server_base_url,
							media_url->u.string + 6
						);
						break;
					}
				}
			}
		}

		if(msg){
			sb(char) msg_converted = rich ? cvt_m2i_msg_rich(msg) : cvt_m2i_msg_plain(msg);

			struct irc_msg irc_msg = {
				.cmd = is_notice ? "NOTICE" : "PRIVMSG",
				.prefix = sender->u.string,
				.params = {
					room_name,
					msg_converted,
				},
				.pcount = 2,
				.flags = SF_CVT_PREFIX
			};

			char time_buf[64] = "";
			if((state->client->irc_caps & IRC_CAP_SERVER_TIME) && ts){
				time_t t = ts->u.number.i / 1000;
				struct tm tm = {};
				gmtime_r(&t, &tm);
				strftime(time_buf, sizeof(time_buf), "time=%Y-%m-%dT%T.000Z", &tm);
				irc_msg.tags = time_buf;
			}

			irc_send(state->client, &irc_msg);
			sb_free(msg_converted);
			free(msg);
		}
	}

	free(room_name);
}

static void mtx_event_topic(struct sync_state* state, yajl_val obj){
	if(state->flags & SYNC_INVITE) return;

	if(state->flags & SYNC_NEW_ROOM){
		state->new_topic = obj;
	} else {
		yajl_val topic  = YAJL_GET(obj, yajl_t_string, ("content", "topic"));
		yajl_val sender = YAJL_GET(obj, yajl_t_string, ("sender"));

		char* irc_room = NULL;
		if(topic && sender && room_get_irc_info(state->room, state->client, &irc_room) != ROOM_IRC_INVALID){
			IRC_SEND_PF(
				state->client,
				sender->u.string,
				SF_CVT_PREFIX,
				"TOPIC",
				irc_room,
				topic->u.string
			);
			free(irc_room);
		}
	}
}

static void mtx_event_member(struct sync_state* state, yajl_val obj){
	yajl_val membership = YAJL_GET(obj, yajl_t_string, ("content", "membership"));
	yajl_val member     = YAJL_GET(obj, yajl_t_string, ("state_key"));
	yajl_val kind       = YAJL_GET(obj, yajl_t_string, ("content", "kind"));

	if(kind && strcmp(kind->u.string, "guest") == 0) return;

	if(member && membership){
		mtx_id member_id = id_intern(member->u.string);

		if(strcmp(membership->u.string, "join") == 0){
			struct member* m = room_member_add(state->room, member_id, MEMBER_STATE_JOINED);
			char* irc_room = NULL;

			if((state->flags & SYNC_TIMELINE) && m->id != state->client->mtx_id && room_get_irc_info(state->room, state->client, &irc_room) > ROOM_IRC_QUERY){
				IRC_SEND_PF(state->client, member->u.string, SF_CVT_PREFIX, "JOIN", irc_room);
			}
			free(irc_room);

		} else if(strcmp(membership->u.string, "leave") == 0){
			room_member_del(state->room, member_id);
		} else if(strcmp(membership->u.string, "invite") == 0){
			room_member_add(state->room, member_id, MEMBER_STATE_INVITED);

			// TODO: use this IRCv3 thing?
			// http://ircv3.net/specs/extensions/invite-notify-3.2.html

			if(state->flags & SYNC_INVITE){
				yajl_val inviter = YAJL_GET(obj, yajl_t_string, ("sender"));
				if(inviter){
					state->inviter = inviter->u.string;
				}
			}
		}
	}
}

static void mtx_event_join_rules(struct sync_state* state, yajl_val obj){
	yajl_val rule = YAJL_GET(obj, yajl_t_string, ("content", "join_rule"));
	if(rule){
		state->room->invite_only = strcmp(rule->u.string, "public") != 0;
	}
}

static void mtx_event_guest_access(struct sync_state* state, yajl_val obj){
	// TODO: ??? change p/s mode on room?
}

static void mtx_event_create(struct sync_state* state, yajl_val obj){
	yajl_val creation_ts = YAJL_GET(obj, yajl_t_number, ("origin_server_ts"));
	if(YAJL_IS_INTEGER(creation_ts)){
		state->room->created = creation_ts->u.number.i / 1000;
	}
}

static void mtx_event_aliases(struct sync_state* state, yajl_val obj){
	yajl_val key = YAJL_GET(obj, yajl_t_string, ("state_key"));
	yajl_val arr = YAJL_GET(obj, yajl_t_array,  ("content", "aliases"));

	if(!key || strcmp(key->u.string, global.mtx_server_name) != 0){
		return;
	}

	for(size_t i = 0; i < arr->u.array.len; ++i){
		yajl_val v = arr->u.array.values[i];
		if(!YAJL_IS_STRING(v)) continue;

		sb_push(state->room->aliases, id_intern(v->u.string));
	}

}

static void mtx_event_canon_alias(struct sync_state* state, yajl_val obj){
	yajl_val alias = YAJL_GET(obj, yajl_t_string, ("content", "alias"));
	if(alias){
		printf("[%02d]     Canonical alias = [%s]\n", state->client->irc_sock, alias->u.string);
		free(state->room->canon);
		state->room->canon = strdup(alias->u.string);
	}
}

static void mtx_event_history_vis(struct sync_state* state, yajl_val obj){
	// TODO: ???
}

static void mtx_event_power_levels(struct sync_state* state, yajl_val obj){
	yajl_val users = YAJL_GET(obj, yajl_t_object, ("content", "users"));
	if(users){
		for(size_t i = 0; i < users->u.object.len; ++i){
			const char* user = users->u.object.keys[i];
			yajl_val power   = users->u.object.values[i];

			struct member* member = room_member_add(state->room, id_intern(user), 0);

			if(YAJL_IS_INTEGER(power)){
				// TODO: if not SYNC_NEW_ROOM, check the old value
				//       and send a MODE for +o, -o, +h, -h, etc
				// XXX:  comparing old value is not safe w.r.t multiple clients,
				//       assume if we get this during non new-room sync, then it changed?
				member->power = power->u.number.i;
			}
		}
	}
}

static void mtx_event_name(struct sync_state* state, yajl_val obj){
	yajl_val name = YAJL_GET(obj, yajl_t_string, ("content", "name"));
	if(name){
		free(state->room->display_name);
		state->room->display_name = strdup(name->u.string);
	}
}

typedef void event_fn(struct sync_state* state, yajl_val);

static struct mtx_handler {
	const char* event;
	event_fn*   func;
} mtx_handlers[] = {
	{ "m.room.message"           , &mtx_event_message },
	{ "m.room.member"            , &mtx_event_member },
	{ "m.room.topic"             , &mtx_event_topic },
	{ "m.room.join_rules"        , &mtx_event_join_rules },
	{ "m.room.guest_access"      , &mtx_event_guest_access },
	{ "m.room.create"            , &mtx_event_create },
	{ "m.room.aliases"           , &mtx_event_aliases },
	{ "m.room.canonical_alias"   , &mtx_event_canon_alias },
	{ "m.room.history_visibility", &mtx_event_history_vis },
	{ "m.room.power_levels"      , &mtx_event_power_levels },
	{ "m.room.name"              , &mtx_event_name },
};

void mtx_event(const char* event, struct sync_state* state, yajl_val obj){
	for(size_t i = 0; i < countof(mtx_handlers); ++i){
		struct mtx_handler* h = mtx_handlers + i;
		if(strcmp(event, h->event) == 0){
			h->func(state, obj);
			break;
		}
	}
}
