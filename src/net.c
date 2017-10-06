#include <curl/curl.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <stdlib.h>
#include <string.h>
#include "stb_sb.h"
#include "morpheus.h"

static struct {
	int tag;
	int fd;
} timer;

static CURLM* curl;

static struct sock* sock_new(int fd){
	struct sock* s = malloc(sizeof(*s));
	s->tag = EPOLL_TAG_CURL;
	s->fd = fd;
	return s;
}

static int curl_cb_socket(CURL* c, curl_socket_t sock, int what, void* uarg, void* sarg){
	int epoll = (intptr_t)uarg;

	//printf("socket cb: c=%p, s=%d, what=%d, sarg=%p\n", c, sock, what, sarg);

	int op = EPOLL_CTL_MOD;

	if(!sarg){
		struct sock* s = sock_new(sock);
		curl_multi_assign(curl, sock, s);
		sarg = s;
		op = EPOLL_CTL_ADD;
	} else if(what == CURL_POLL_REMOVE){
		op = EPOLL_CTL_DEL;
		free(sarg);
	}

	struct epoll_event ev = {
		.data.ptr = sarg
	};

	if(what & CURL_POLL_IN)  ev.events |= EPOLLIN;
	if(what & CURL_POLL_OUT) ev.events |= EPOLLOUT;

	if(epoll_ctl(epoll, op, sock, &ev) == -1){
		perror("epoll_ctl");
	}

	return 0;
}

static int curl_cb_timer(CURLM* multi, long timeout_ms, void* uarg){
	struct itimerspec it = {};

	//printf("timer cb: %ld\n", timeout_ms);

	if(timeout_ms != -1){
		it.it_value.tv_sec  = timeout_ms / 1000;
		it.it_value.tv_nsec = 1 + (timeout_ms % 1000) * 1000000L;
	}

	timerfd_settime(timer.fd, 0, &it, NULL);

	return 0;
}

static size_t net_curl_cb(char* ptr, size_t sz, size_t nmemb, void* data){
	char** out = data;
	const size_t total = sz * nmemb;
	memcpy(sb_add(*out, total), ptr, total);
	return total;
}

static int net_msg_sort(const void* _a, const void* _b){
	struct net_msg* const* a = _a;
	struct net_msg* const* b = _b;
	return (*b)->type - (*a)->type;
}

bool net_init(void){

	timer.tag = EPOLL_TAG_CURL_TIMER;
	timer.fd  = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);

	struct epoll_event ev = {
		.events = EPOLLIN,
		.data.ptr = &timer.tag
	};
	epoll_ctl(global.epoll, EPOLL_CTL_ADD, timer.fd, &ev);

	curl = curl_multi_init();
	curl_multi_setopt(curl, CURLMOPT_SOCKETFUNCTION, &curl_cb_socket);
	curl_multi_setopt(curl, CURLMOPT_SOCKETDATA    , (void*)((intptr_t)global.epoll));
	curl_multi_setopt(curl, CURLMOPT_TIMERFUNCTION , &curl_cb_timer);
	//curl_multi_setopt(curl, CURLMOPT_PIPELINING, CURLPIPE_HTTP1 | CURLPIPE_MULTIPLEX);
	//curl_multi_setopt(curl, CURLMOPT_MAX_PIPELINE_LENGTH, 32);

	bool got_server_name = false;
	sb(char) data = NULL;
	char* url;

	asprintf(&url, "%s/_matrix/key/v2/server/", global.mtx_server_base_url);

	CURL* c = curl_easy_init();
	curl_easy_setopt(c, CURLOPT_USERAGENT, "Morpheus");
	curl_easy_setopt(c, CURLOPT_URL, url);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, &net_curl_cb);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, &data);
	curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
	curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);

	free(url);

	if(curl_easy_perform(c) == CURLE_OK){
		sb_push(data, 0);

		yajl_val root = yajl_tree_parse(data, NULL, 0);
		yajl_val serv = YAJL_GET(root, yajl_t_string, ("server_name"));

		if(serv){
			printf("Upstream server_name: [%s]\n", serv->u.string);
			global.mtx_server_name = strdup(serv->u.string);
			got_server_name = true;
		}

		yajl_tree_free(root);
	}

	curl_easy_cleanup(c);
	sb_free(data);

	return got_server_name;
}

void net_update(int emask, struct sock* s){

	int curlmask = 0;
	if(emask & EPOLLIN)  curlmask |= CURL_CSELECT_IN;
	if(emask & EPOLLOUT) curlmask |= CURL_CSELECT_OUT;
	if(emask & EPOLLERR) curlmask |= CURL_CSELECT_ERR;

	int blah;
	curl_multi_socket_action(curl, s ? s->fd : CURL_SOCKET_TIMEOUT, curlmask, &blah);

	sb(struct net_msg*) done_list = NULL;

	// get all the completed messages from curl

	CURLMsg* cm;
	while((cm = curl_multi_info_read(curl, &blah))){
		if(cm->msg != CURLMSG_DONE) continue;

		struct client* client;
		curl_easy_getinfo(cm->easy_handle, CURLINFO_PRIVATE, &client);

		for(struct net_msg* msg = client->msgs; msg; msg = msg->next){
			if(msg->curl == cm->easy_handle){
				printf("[%02d] MTX msg [%s]\n", client->irc_sock, mtx_msg_strs[msg->type]);
				sb_push(msg->data, 0);
				sb_push(done_list, msg);
				msg->done = true;

				long status = 0L - cm->data.result;
				if(-status == CURLE_OK){
					curl_easy_getinfo(cm->easy_handle, CURLINFO_HTTP_CODE, &status);
				}
				msg->curl_status = status;
			} else if(msg->done){
				// this must be a completed SYNC that we deferred processing, add it again.
				sb_push(done_list, msg);
			}
		}
	}

	if(!done_list) return;

	// sort the messages so that SYNCs come last
	qsort(done_list, sb_count(done_list), sizeof(struct net_msg*), &net_msg_sort);

	sb_each(m, done_list){
		struct net_msg* msg = *m;
		struct client* client;
		curl_easy_getinfo(msg->curl, CURLINFO_PRIVATE, &client);

		// count number of non-done messages this client has
		int remaining_msgs = 0;
		for(struct net_msg* tmp = client->msgs; tmp; tmp = tmp->next){
			if(!tmp->done) remaining_msgs++;
		}

		// if there is a remaining message and this completed one is a SYNC, we need to wait
		// until the other message completes first. otherwise duplicate messages can happen.
		if(msg->type == MTX_MSG_SYNC && remaining_msgs){
			*m = NULL; // so it doesn't get free'd in the next loop
			continue;
		}

		mtx_recv(client, msg);
	}

	// free completed messages
	sb_each(m, done_list){
		struct net_msg* msg = *m;
		if(!msg) continue;

		struct client* client;
		curl_easy_getinfo(msg->curl, CURLINFO_PRIVATE, &client);

		for(struct net_msg** p = &client->msgs; *p; /**/){
			if(*p == msg){
				*p = msg->next;
				net_msg_free(msg);
			} else {
				p = &(*p)->next;
			}
		}
	}

	sb_free(done_list);
}

struct net_msg* net_msg_new(struct client* client, int type){
	struct net_msg* msg = calloc(1, sizeof(*msg));
	
	msg->curl = curl_easy_init();
	curl_easy_setopt(msg->curl, CURLOPT_ACCEPT_ENCODING, "");
	curl_easy_setopt(msg->curl, CURLOPT_USERAGENT, "morpheus");
	curl_easy_setopt(msg->curl, CURLOPT_TCP_NODELAY, 1);
	curl_easy_setopt(msg->curl, CURLOPT_WRITEFUNCTION, &net_curl_cb);
	curl_easy_setopt(msg->curl, CURLOPT_WRITEDATA, &msg->data);

	curl_easy_setopt(msg->curl, CURLOPT_PRIVATE, client);
	curl_easy_setopt(msg->curl, CURLOPT_SSL_VERIFYHOST, 0L);
	curl_easy_setopt(msg->curl, CURLOPT_SSL_VERIFYPEER, 0L);
	curl_easy_setopt(msg->curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);

	if(type == MTX_MSG_SYNC){
		// XXX: breaks other GETs if they're pipelined onto the sync request
		//      so only enable it for sync itself (even this causes issues? investigate)
		// curl_easy_setopt(msg->curl, CURLOPT_PIPEWAIT, 1L);
		curl_easy_setopt(msg->curl, CURLOPT_TIMEOUT, 100);
	} else {
		// all the non-sync messages should complete timely, if not something is busted.
		curl_easy_setopt(msg->curl, CURLOPT_TIMEOUT, 10);
	}

	//curl_easy_setopt(msg->curl, CURLOPT_VERBOSE, 1L);

	msg->headers = curl_slist_append(msg->headers, "Content-Type: application/json");
	msg->headers = curl_slist_append(msg->headers, "Accept: application/json");

	curl_easy_setopt(msg->curl, CURLOPT_HTTPHEADER, msg->headers);

	msg->type = type;

	struct net_msg** p = &client->msgs;
	while(*p) p = &(*p)->next;
	*p = msg;

	return msg;
}

void net_msg_send(struct net_msg* msg){
	curl_multi_add_handle(curl, msg->curl);
}

void net_msg_free(struct net_msg* msg){
	sb_free(msg->data);
	curl_multi_remove_handle(curl, msg->curl);
	curl_easy_cleanup(msg->curl);
	curl_slist_free_all(msg->headers);
	free(msg);
}
