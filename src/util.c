#include "morpheus.h"
#include <stdarg.h>
#include <yajl/yajl_gen.h>

bool yajl_generate(char** out, const char* fmt, ...){
	va_list va;
	va_start(va, fmt);
	bool result = false;

	yajl_gen json = yajl_gen_alloc(NULL);

	for(const char* p = fmt; *p; p += strspn(p, " ,:")){
		int len = strcspn(p+1, " ,:") + 1;

		switch(*p){
			case '{':
				yajl_gen_map_open(json);
				break;
			case '}':
				yajl_gen_map_close(json);
				break;
			case '[':
				yajl_gen_array_open(json);
				break;
			case ']':
				yajl_gen_array_close(json);
				break;
			case '\'':
				yajl_gen_string(json, p+1, len-2);
				break;
			case '%': {
				if(p[1] == 's'){
					char* s = va_arg(va, char*);
					if(s){
						yajl_gen_string(json, s, strlen(s));
					} else {
						yajl_gen_null(json);
					}
				} else if(p[1] == 'z'){
					size_t l = va_arg(va, size_t);
					char* s = va_arg(va, char*);
					if(s){
						yajl_gen_string(json, s, l);
					} else {
						yajl_gen_null(json);
					}
				} else if(p[1] == 'i' || p[1] == 'd' || p[1] == 'l'){
					long long l = va_arg(va, long long);
					yajl_gen_integer(json, l);
				} else if(p[1] == 'b'){
					int b = va_arg(va, int);
					yajl_gen_bool(json, b);
				} else {
					goto out;
				}
			} break;
			default: {
				if(strncasecmp(p, "false", len) == 0){
					yajl_gen_bool(json, 0);
				} else if(strncasecmp(p, "true", len) == 0){
					yajl_gen_bool(json, 1);
				} else if(strncasecmp(p, "null", len) == 0){
					yajl_gen_null(json);
				} else {
					yajl_gen_number(json, p, len);
				}
			} break;
		}

		p += len;
	}

	const uint8_t* buf = NULL;
	size_t sz;
	yajl_gen_get_buf(json, &buf, &sz);
	*out = strndup(buf, sz);
	result = true;

out:
	yajl_gen_free(json);
	va_end(va);

	return result;
}
