#include "morpheus.h"

// TODO: array vs linked list vs hash table etc ??
static sb(struct room) room_list;

struct room* room_new(mtx_id id){
	sb_each(r, room_list){
		if(id == r->id) return r;
	}

	struct room r = {
		.id = id,
	};
	sb_push(room_list, r);

	return &sb_last(room_list);
}

struct room* room_get_irc(const char* chan){
	size_t chan_sz = strlen(chan);

	sb_each(r, room_list){
		if(!r->canon) continue;

		char* colon = strchr(r->canon, ':');
		assert(colon);
		if(chan_sz != (size_t)(colon - r->canon)) continue;

		if(strncmp(chan, r->canon, chan_sz) == 0){
			return r;
		}
	}

	return NULL;
}

struct room* room_get_mtx(mtx_id id){
	sb_each(r, room_list){
		if(id == r->id) return r;
	}

	return NULL;
}

struct member* room_member_get(struct room* room, mtx_id member_id){
	assert(room);
	sb_each(m, room->members){
		if(m->id == member_id) return m;
	}
	return NULL;
}

struct member* room_member_add(struct room* room, mtx_id member_id, int state){
	struct member* result = room_member_get(room, member_id);
	if(!result){
		struct member mem = {
			.id = member_id,
		};
		sb_push(room->members, mem);
		result = &sb_last(room->members);
	}

	result->state = state;

	return result;
}

void room_member_del(struct room* room, mtx_id member_id){
	struct member* result = room_member_get(room, member_id);
	if(result){
		sb_erase(room->members, result - room->members);
	}
}

// TODO: probably saner to return int, char** as param, to avoid alloc'ing in some cases
char* room_get_irc_name(struct room* room, struct client* client, int* type){
	char* result = NULL;
	int res_type = ROOM_IRC_INVALID;

	if(room->canon){

		res_type = ROOM_IRC_CHANNEL;
		result   = strndup(room->canon, strchrnul(room->canon, ':') - room->canon);

	} else if(sb_count(room->members) == 2){

		// TODO: check for invite-only?

		if(room->members[0].id == client->mtx_id){
			res_type = ROOM_IRC_QUERY;
			result   = cvt_m2i_user(room->members[1].id);
			*strchrnul(result, '!') = '\0';
		} else if(room->members[1].id == client->mtx_id){
			res_type = ROOM_IRC_QUERY;
			result   = cvt_m2i_user(room->members[0].id);
			*strchrnul(result, '!') = '\0';
		}

	} else if(sb_count(room->members) > 2){

		// TODO: check for invite-only?

		sb_each(m, room->members){
			if(m->id == client->mtx_id){
				res_type = ROOM_IRC_GROUP;
				asprintf(&result, "&group_%6s", id_lookup(room->id) + 1);
				break;
			}
		}

	}

	if(type){
		*type = res_type;
	}

	return result;
}

struct room* room_find_query(struct client* client, mtx_id partner){
	sb_each(r, client->irc_rooms){
		struct room* room = room_get_mtx(*r);
		if(!room) continue;
		if(room->canon) continue;
		if(sb_count(room->members) != 2) continue;

		// TODO: check for invite-only?

		bool found = true;
		sb_each(m, room->members){
			if(m->id != client->mtx_id && m->id != partner){
				found = false;
				break;
			}
		}

		if(found){
			return room;
		}
	}

	return NULL;
}

void room_free(struct room* room){

}
