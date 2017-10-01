#include "morpheus.h"
#define INSO_IMPL
#include "inso_ht.h"

static sb(char) id_mem;
static inso_ht  id_ht;
static uint32_t id_seed;
static inso_ht  id_server_ht;

static void id_server_add(const char*);

static uint32_t id_murmur2(const void* key, int len, uint32_t seed){
	const uint32_t m = 0x5bd1e995;
	const int r = 24;

	uint32_t h = seed ^ len;
	const uint8_t* data = (const uint8_t*)key;

	while(len >= 4){
		uint32_t k = *(uint32_t*)data;

		k *= m; 
		k ^= k >> r; 
		k *= m; 

		h *= m; 
		h ^= k;

		data += 4;
		len -= 4;
	}

	switch(len){
		case 3: h ^= data[2] << 16;
		case 2: h ^= data[1] << 8;
		case 1: h ^= data[0];
		        h *= m;
	};

	h ^= h >> 13;
	h *= m;
	h ^= h >> 15;

	return h;
} 

static size_t id_hash(const void* ht_entry){
	const uint32_t* offset = ht_entry;
	return id_murmur2(id_mem + *offset, strlen(id_mem + *offset), id_seed);
}

static bool id_cmp(const void* ht_entry, void* param){
	const uint32_t* offset = ht_entry;
	const char* str = param;
	return strcmp(id_mem + *offset, str) == 0;
}

static void id_init(void){
	inso_ht_init(&id_ht, 32, sizeof(uint32_t), &id_hash);
	inso_ht_init(&id_server_ht, 32, sizeof(uint32_t), &id_hash);
	sb_push(id_mem, 0);
	id_seed = rand();
}

mtx_id id_intern(const char* id){
	if(!id_ht.memory) id_init();

	// There are IDs that start with $ too, for events,
	// but interning those would be too spammy (since they're never freed currently)
	assert(id && (*id == '#' || *id == '@' || *id == '!'));

	size_t id_len = strlen(id);
	uint32_t* offset = inso_ht_get(&id_ht, id_murmur2(id, id_len, id_seed), &id_cmp, (void*)id);

	if(!offset){
		char* p = memcpy(sb_add(id_mem, id_len + 1), id, id_len + 1);
		uint32_t elem = p - id_mem;
		offset = inso_ht_put(&id_ht, &elem);
		id_server_add(p);
	}

	return *offset;
}

const char* id_lookup(mtx_id id){
	assert(id < sb_count(id_mem));
	return id_mem + id;
}

// id_server stuff, for generating the 4-digit hex suffix on
// names/channels from other homeservers

static inline uint16_t legit_maths(uint16_t n, int i){
	uint16_t data = (0xba771e70ad5 >> (14 * i));
	uint8_t r1 = n & 0xff;
	uint8_t r2 = (n >> 8) ^ id_murmur2(&data, 2, r1);
	return (r2 << 8) | r1;
}

static inline uint16_t id_server_encode(uint16_t off){
	for(int i = 0; i < 3; ++i){
		off = legit_maths(off, i);
	}
	return off ^ 0xa0ba;
}

static inline uint16_t id_server_decode(uint16_t off){
	off ^= 0xa0ba;
	for(int i = 3; i --> 0; /**/){
		off = legit_maths(off, i);
	}
	return off;
}

static void id_server_add(const char* p){
	p = strchr(p, ':');
	assert(p);
	++p;

	uint32_t elem = p - id_mem;
	uint32_t hash = id_murmur2(p, strlen(p), id_seed);

	if(!inso_ht_get(&id_server_ht, hash, &id_cmp, (void*)p)){
		inso_ht_put(&id_server_ht, &elem);
	}
}

int id_server_hash(mtx_id id){
	const char* p = id_lookup(id);
	p = strchr(p, ':');
	assert(p);
	++p;

	uint32_t hash = id_murmur2(p, strlen(p), id_seed);
	uint32_t* off = inso_ht_get(&id_server_ht, hash, &id_cmp, (void*)p);
	assert(off);

	return id_server_encode(*off);
}

const char* id_server_unhash(int hash){
	uint16_t code = id_server_decode(hash);
	assert(code < sb_count(id_mem));
	return id_mem + code;
}
