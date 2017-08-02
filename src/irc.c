#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include "morpheus.h"

static bool irc_parse(char* buf, struct irc_msg* msg){

	// tags
	char* p = buf;
	if(*p == '@'){
		msg->tags = p;

		if(!(p = strchr(p+1, ' '))){
			return false;
		} else {
			*p++ = '\0';
		}
	}

	// prefix
	if(*p == ':'){
		msg->prefix = p;

		if(!(p = strchr(p+1, ' '))){
			return false;
		} else {
			*p++ = '\0';
		}
	}

	// command
	msg->cmd = p;
	if(ISDIGIT(*p)){
		if(!ISDIGIT(p[1]) || !ISDIGIT(p[2])){
			return false;
		} else {
			p += 3;
		}
	} else if(ISLETTER(*p)){
		do {
			++p;
		} while(ISLETTER(*p));
	} else {
		return false;
	}

	// params
	int pcount = 0;
	while(*p == ' '){
		*p++ = '\0';

		if(pcount >= 16) return false;

		if(*p == ':'){
			msg->params[pcount++] = p+1;
			p += strcspn(p, "\r");
		} else if(!*p || strchr("\r ", *p)){
			return false;
		} else {
			msg->params[pcount++] = p;
			p += strcspn(p, "\r ");
		}
	}

	if(!p[0] || (p[0] == '\r' && !p[1])){
		*p = '\0';
		msg->pcount = pcount;
		return true;
	} else {
		return false;
	}
}

void irc_recv(struct client* client, const char* buf, size_t len){

	memcpy(sb_add(client->irc_buf, len), buf, len);

	char* p;
	while((p = memchr(client->irc_buf, '\n', sb_count(client->irc_buf)))){
		*p = '\0';

		struct irc_msg msg = {};
		if(irc_parse(client->irc_buf, &msg)){
			irc_event(client, &msg);
		}

		size_t rem = sb_count(client->irc_buf) - ((p+1) - client->irc_buf);
		memmove(client->irc_buf, p+1, rem);
		stb__sbn(client->irc_buf) = rem;
	}

	if(sb_count(client->irc_buf) > 1024){
		client_del(client);
	}
}

int irc_send(struct client* client, struct irc_msg* _msg){
	char buf[1024] = "";
	char* p = buf;
	int result = 0;
	struct irc_msg msg = *_msg;

	if(msg.flags & SF_CVT_ROOM_P0){
		assert(msg.pcount >= 1);
		assert(msg.params[0]);

		const char* colon = strchr(msg.params[0], ':');
		msg.params[0] = strndupa(msg.params[0], colon - msg.params[0]);
	}

	// TODO: this stuff is messy... think of a better way
	char* cvt_prefix = NULL;
	if(msg.flags & SF_CVT_PREFIX){
		cvt_prefix = cvt_m2i_user(id_intern(msg.prefix));
		msg.prefix = cvt_prefix;
	}

	if(!msg.prefix) msg.prefix = "morpheus";

	if(msg.tags){
		if(strlen(msg.tags) > 510){
			result = -1;
			goto out;
		}

		*p++ = '@';
		p = stpcpy(p, msg.tags);
		*p++ = ' ';
	}

	size_t total_len = strlen(msg.prefix) + strlen(msg.cmd) + 2;
	for(size_t i = 0; i < msg.pcount; ++i){
		total_len += strlen(msg.params[i]) + 1;
	}
	if(total_len > 510){
		result = -2;
		goto out;
	}

	*p++ = ':';
	p = stpcpy(p, msg.prefix);
	*p++ = ' ';
	p = stpcpy(p, msg.cmd);

	if(msg.pcount){
		*p++ = ' ';

		for(size_t i = 0; i < msg.pcount - 1; ++i){
			p = stpcpy(p, msg.params[i]);
			*p++ = ' ';
		}

		*p++ = ':';
		p = stpcpy(p, msg.params[msg.pcount-1]);
	}

	*p++ = '\r';
	*p++ = '\n';

	// TODO: if msg is too big, send multiple packets

	if(send(client->irc_sock, buf, p - buf, 0) == -1){
		perror("send");
		result = -3;
	}

out:
	free(cvt_prefix);
	return result;
}

void irc_send_names(struct client* client, struct room* room){
	sb(char) buf = NULL;

	if(!room || !room->canon) return;

	// TODO: we need to put user prefixes in here, using member->power
	//       100+ = OP (@), 50+ = HOP (%)

	// We also need to either do msg splitting here, or let irc_send take care of that

	char* room_name = room_get_irc_name(room, client, NULL);

	sb_each(m, room->members){
		char prefix = 0;

		if(m->power == 100){
			prefix = '@';
		} else if(m->power >= 50){
			prefix = '%';
		}

		const char* c = id_lookup(m->id)+1;
		const char* end = strchrnul(c, ':');
		size_t name_len = end - c;

		// TODO: better sizing
		if(sb_count(buf) + (name_len+2) > 400){
			sb_push(buf, 0);
			IRC_SEND_NUM(client, "353", "=", room_name, buf);
			stb__sbn(buf) = 0;
		}

		if(prefix){
			sb_push(buf, prefix);
		}
		memcpy(sb_add(buf, name_len), c, name_len);
		sb_push(buf, ' ');
	}
	sb_push(buf, 0);

	IRC_SEND_NUM(client, "353", "=", room_name, buf);
	IRC_SEND_NUM(client, "366", room_name, "End of /NAMES list.");

	free(room_name);
	sb_free(buf);
}
