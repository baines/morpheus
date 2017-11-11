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

struct room* room_lookup_irc(const char* chan){
	size_t chan_sz = strlen(chan);

	if(*chan == '#'){
		sb_each(r, room_list){
			const char* name = NULL;
			if(r->canon){
				name = r->canon;
			} else if(r->chosen_alias){
				name = id_lookup(r->chosen_alias);
			}

			if(!name) continue;

			char* colon = strchr(name, ':');
			assert(colon);
			if(chan_sz != (size_t)(colon - name)) continue;

			if(strncmp(chan, name, chan_sz) == 0){
				return r;
			}
		}
	} else if(*chan == '!'){
		struct room* found = NULL;

		sb_each(r, room_list){
			if(!r->id) continue;
			const char* id = id_lookup(r->id);
			if(strncmp(chan, id, 9) == 0){
				if(found){
					fprintf(stderr, "FIXME: room short id collision [%s] [%s]\n", id_lookup(found->id), id);
				} else {
					found = r;
				}
			}
		}

		return found;
	}

	return NULL;
}

struct room* room_lookup_mtx(mtx_id id){
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

	if(state != MEMBER_STATE_NONE){
		result->state = state;
	}

	return result;
}

void room_member_del(struct room* room, mtx_id member_id){
	struct member* result = room_member_get(room, member_id);
	if(result){
		sb_erase(room->members, result - room->members);
	}
}

static int room_levenshtein(const char* a, const char* b){
	int m = strlen(a);
	int n = strlen(b);

	int* v0 = alloca((n+1)*sizeof(*v0));
	int* v1 = alloca((n+1)*sizeof(*v1));

	for(int i = 0; i < n+1; ++i){
		v0[i] = i;
	}

	for(int i = 0; i < m; ++i){
		v1[0] = i + 1;

		for(int j = 0; j < n; ++j){
			int cost = !(a[i] == b[j]);
			v1[j+1] = MIN(v1[j] + 1, MIN(v0[j+1] + 1, v0[j] + cost));
		}

		int* tmp = v0;
		v0 = v1;
		v1 = tmp;
	}

	return v0[n];
}

static void room_choose_alias(struct room* room){
	assert(!room->canon);

	if(room->chosen_alias) return;
	if(sb_count(room->aliases) == 0) return;

	if(!room->display_name || sb_count(room->aliases) == 1){
		room->chosen_alias = room->aliases[0];
		return;
	}

	char* normalised_display_name = strdupa(room->display_name);

	for(char* c = normalised_display_name; *c; ++c){
		if(*c >= 'A' && *c <= 'Z') *c += ('a' - 'A');
		else if(*c == ' ') *c = '-';
	}

	int score = INT_MAX;
	mtx_id id = 0;

	sb_each(a, room->aliases){
		const char* alias = id_lookup(*a);
		char* fixed_alias = strndupa(alias, strchrnul(alias, ':') - alias);

		int a_score = room_levenshtein(normalised_display_name, fixed_alias + 1);
		if(a_score < score){
			score = a_score;
			id = *a;
		}
	}

	room->chosen_alias = id;
}

int room_get_irc_info(struct room* room, struct client* client, char** name){

	if(room->canon){

		if(name){
			*name = strndup(room->canon, strchrnul(room->canon, ':') - room->canon);
		}
		return ROOM_IRC_CHANNEL;

	} else if(!room->chosen_alias){
		room_choose_alias(room);
	}

	if(room->chosen_alias){

		if(name){
			const char* alias = id_lookup(room->chosen_alias);
			*name = strndup(alias, strchrnul(alias, ':') - alias);
		}
		return ROOM_IRC_CHANNEL;

	} else if(sb_count(room->members) == 2){

		for(size_t i = 0; i < 2; ++i){
			if(room->members[i].id == client->mtx_id){
				if(name){
					*name = cvt_m2i_user(room->members[i ^ 1].id);
					*strchrnul(*name, '!') = '\0';
				}
				return ROOM_IRC_QUERY;
			}
		}

	} else if(sb_count(room->members) > 2){

		sb_each(m, room->members){
			if(m->id == client->mtx_id){
				if(name){
					asprintf(name, "!%8s", id_lookup(room->id) + 1);
				}
				return ROOM_IRC_GROUP;
			}
		}
	}

	return ROOM_IRC_INVALID;
}

struct room* room_find_query(struct client* client, mtx_id partner){
	sb_each(r, client->irc_rooms){
		struct room* room = room_lookup_mtx(*r);
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
