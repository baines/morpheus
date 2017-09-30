#include "morpheus.h"
#include "inso_ht.h"

#define I2V(x) ((void*)(uintptr_t)(x))

enum pres_status {
	PRES_ONLINE,
	PRES_OFFLINE,
	PRES_UNAVAILABLE,
};

struct presence {
	mtx_id member; // must be first member
	enum pres_status status;
	time_t last_updated;
};

static inso_ht pres_ht;

static size_t pres_hash(const void* entry){
	uint32_t x = *(uint32_t*)entry;
    x = ((x >> 16) ^ x) * 0x45d9f3b;
    x = ((x >> 16) ^ x) * 0x45d9f3b;
    x = (x >> 16) ^ x;
    return x;
}

static bool pres_cmp(const void* entry, void* param){
	return I2V(*(uint32_t*)entry) == param;
}

static enum pres_status pres_lookup(const char* str){
	/**/ if(strcmp(str, "online")      == 0) return PRES_ONLINE;
	else if(strcmp(str, "unavailable") == 0) return PRES_UNAVAILABLE;
	else return PRES_OFFLINE;
}

bool presence_update(struct client* client, mtx_id id, const char* pres_str){
	if(!pres_ht.memory){
		inso_ht_init(&pres_ht, 32, sizeof(struct presence), &pres_hash);
	}

	time_t now = time(0);
	bool updated = false;
	enum pres_status status = pres_lookup(pres_str);
	struct presence* p = inso_ht_get(&pres_ht, pres_hash(&id), &pres_cmp, I2V(id));

	if(p){
		if(p->status != status){
			p->status = status;
			p->last_updated = now;
			updated = true;
		} else if(p->last_updated > client->last_sync){
			updated = true;
		}
	} else {
		struct presence newp = {
			.member = id,
			.status = status,
			.last_updated = now,
		};
		inso_ht_put(&pres_ht, &newp);
		updated = true;
	}

	return updated;
}
