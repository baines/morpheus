#include "morpheus.h"

char* cvt_m2i_user(mtx_id user_id){
	assert(user_id);
	const char* user = id_lookup(user_id);
	assert(user[0] == '@');
	++user;

	char* colon = strchr(user, ':');
	assert(colon);

	int n = colon - user;

	char* result;
	asprintf(&result, "%.*s!%.*s@%s", n, user, n, user, colon+1);
	assert(result);

	return result;
}

mtx_id cvt_i2m_user(const char* user){
	assert(user);

	// FIXME: why am i doing it this way... the proper server name should be in user
	int len = strchrnul(user, '!') - user;

	char buf[256];
	snprintf(buf, sizeof(buf), "@%.*s:%s", len, user, global.mtx_server_name);

	return id_intern(buf);
}

static const char* colors[] = {
	"white" , "black"  , "navy"  , "green",
	"red"   , "maroon" , "purple", "olive",
	"yellow", "lime"   , "teal"  , "aqua" ,
	"blue"  , "fuchsia", "gray"  , "silver",
};

sb(char) cvt_m2i_msg(const char* msg){
	sb(char) out = NULL;

	struct {
		const char* tag;
		char val;
	} tags[] = {
		{ "<b>", 0x02 }, { "</b>", 0x02 },
		{ "<i>", 0x1d }, { "</i>", 0x1d },
		{ "<u>", 0x1f }, { "</u>", 0x1f },
	};

	while(*msg){
		bool found_format = false;
		for(size_t i = 0; i < countof(tags); ++i){
			int len = 3 + (i&1);
			if(strncmp(msg, tags[i].tag, len) == 0){
				found_format = true;
				sb_push(out, tags[i].val);
				msg += len;
				break;
			}
		}

		if(!found_format){
			char color[64];
			int len = 0;

			// TODO: allow no quotes / single
			if(sscanf(msg, "<font color=\"%63[^\"]\">%n", color, &len) == 1 && len){

				for(size_t i = 0; i < countof(colors); ++i){
					if(strcmp(colors[i], color) == 0){
						sb_push(out, 0x03);
						sb_push(out, (i / 10) + '0');
						sb_push(out, (i % 10) + '0');
						break;
					}
				}

				msg += len;
			} else if(strncmp(msg, "</font>", 7) == 0){
				sb_push(out, 0x03); // XXX: bug if next char is number?
				msg += 7;
			} else {
				sb_push(out, *msg);
				++msg;
			}
		}
	}

	sb_push(out, 0);
	return out;
}

sb(char) cvt_i2m_msg(const char* msg, sb(char)* stripped){
	int_fast8_t state = 0, old_state = 0;

	enum {
		FMT_BOLD   = (1 << 0),
		FMT_ITALIC = (1 << 1),
		FMT_ULINE  = (1 << 2),
	};

	static const char* tags[] = {
		"<b>", "</b>",
		"<u>", "</u>",
		"<i>", "</i>",
	};

	sb(char) out = NULL;

	for(const char* p = msg; *p; ++p){
		switch(*p){
			case 0x02:
				state ^= FMT_BOLD;
				break;
			case 0x0f:
				state = 0;
				break;
			case 0x1d:
				state ^= FMT_ITALIC;
				break;
			case 0x1f:
				state ^= FMT_ULINE;
				break;
			case 0x03: {
				size_t color = 0;

				if(ISDIGIT(p[1])){
					color = p[1] - '0';
					++p;
					
					if(ISDIGIT(p[1])){
						color = (color * 10) + (p[1] - '0');
						++p;
					}

					++color;
				}

				// skip background
				if(p[1] == ',' && ISDIGIT(p[2])){
					++p;
					if(ISDIGIT(p[2])){
						++p;
					}
				}

				printf("color = %zu\n", color);

				if(color > 16){
					color = 0;
				}

				state = (state & 0x07) | (color << 3);
			} break;

			default: {
				uint_fast8_t diff = old_state ^ state;

				for(int i = 0; i < 3; ++i){
					if(!(diff & (1 << i))) continue;

					bool closed = !(state & (1 << i));
					const char* tag = tags[i*2+closed];
					size_t n = closed + 3;

					memcpy(sb_add(out, n), tag, n);
				}

				if(diff >> 3){
					size_t color = state >> 3;
					assert(color <= 16);

					if(color){
						char buf[32];
						int n = sprintf(buf, "<font color=\"%s\">", colors[color-1]);
						assert(n > 0 && n < 32);

						memcpy(sb_add(out, n), buf, n);
					} else {
						static const char* tag = "</font>";
						memcpy(sb_add(out, sizeof(tag)-1), tag, sizeof(tag)-1);
					}
				}

				sb_push(out, *p);
				sb_push(*stripped, *p);
				old_state = state;
			}
		}
	}
	// TODO: we should probably close all the tags here...

	return out;
}
