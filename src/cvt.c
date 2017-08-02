#include "morpheus.h"
#include <wchar.h>

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

sb(char) cvt_m2i_msg_plain(const char* msg){
	sb(char) out = NULL;

	for(const char* c = msg; *c; ++c){
		if(*(uint8_t*)c > 0x03 && *(uint8_t*)c < ' '){
			sb_push(out, ' ');
		} else {
			sb_push(out, *c);
		}
	}

	sb_push(out, 0);
	return out;
}

static uint8_t html_tag_to_irc(const char* tag, size_t len){
	if(!tag || *tag != '<') return 0;

	if(len == 2 || (len == 3 && tag[1] == '/')){
		switch(tag[len-1]){
			case 'b': return 0x02;
			case 'i': return 0x1d;
			case 'u': return 0x1f;
			default:  return 0;
		}
	}

	if(len == 7 && strncmp(tag, "</font>", 7) == 0){
		return 0x03;
	}

	if(len > 12 && strncmp(tag, "<font color=", 12) == 0){
		const char* s = tag + 12;
		const char* e = tag + len - 1;

		if(*s == '"' || *s == '\'') ++s;
		if(*e == '"' || *e == '\'') --e;

		if(e <= s) return 0;

		for(size_t i = 0; i < countof(colors); ++i){
			if(strncmp(s, colors[i], (e - s) + 1) == 0){
				return i + '0';
			}
		}
	}

	return 0;
}

sb(char) cvt_m2i_msg_rich(const char* msg){
	sb(char) out = NULL;

	while(*msg){
		if(*msg == '<'){ // strip / convert  html tags

			char* end = strchrnul(msg+1, '>');

			uint8_t code = html_tag_to_irc(msg, end - msg);
			if(code >= '0'){ // colour
				code -= '0';
				sb_push(out, 0x03);
				sb_push(out, (code / 10) + '0');
				sb_push(out, (code % 10) + '0');
			} else if(code){
				sb_push(out, code);
			}

			msg = *end ? end + 1 : end;

		} else if(*msg == '&'){ // unescape html entities

			const char* end = msg + strcspn(msg, "; ");
			size_t orig_count = sb_count(out);

			/**/ if(strncmp(msg+1, "amp;" , 4) == 0) sb_push(out, '&');
			else if(strncmp(msg+1, "gt;"  , 3) == 0) sb_push(out, '>');
			else if(strncmp(msg+1, "lt;"  , 3) == 0) sb_push(out, '<');
			else if(strncmp(msg+1, "quot;", 5) == 0) sb_push(out, '"');
			else if(strncmp(msg+1, "nbsp;", 5) == 0) sb_push(out, ' ');
			else if(msg[1] == '#'){
				wint_t wc = 0;
				char buf[MB_LEN_MAX];
				int n;

				if((sscanf(msg+2, "%u;" , &wc) == 1
				||  sscanf(msg+2, "x%x;", &wc) == 1)
				&& (n = wctomb(buf, wc)) > 0){
					memcpy(sb_add(out, n), buf, n);
				}
			}

			if(sb_count(out) != orig_count) msg = end;
			if(*msg) ++msg;

		} else if(*(uint8_t*)msg < ' '){ // strip control chars
			sb_push(out, ' ');
			++msg;
		} else {
			sb_push(out, *msg);
			++msg;
		}
	}

	sb_push(out, 0);
	return out;
}

static void add_char_escaped(uint8_t c, sb(char)* out){

	struct {
		uint8_t match;
		const char* str;
		size_t len;
	} items[] = {
		{ '>', "&gt;"  , 4 },
		{ '<', "&lt;"  , 4 },
		{ '&', "&amp;" , 5 },
		{ '"', "&quot;", 6 },
	};

	if(c < ' ') sb_push(*out, ' ');
	else {
		for(size_t i = 0; i < countof(items); ++i){
			if(c == items[i].match){
				memcpy(sb_add(*out, items[i].len), items[i].str, items[i].len);
				return;
			}
		}

		sb_push(*out, c);
	}
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

				add_char_escaped(*p, &out);
				sb_push(*stripped, *p);
				old_state = state;
			}
		}
	}
	// TODO: we should probably close all the tags here...

	return out;
}
