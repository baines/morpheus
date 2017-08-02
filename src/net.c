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

void net_init(void){

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
	curl_multi_setopt(curl, CURLMOPT_PIPELINING, CURLPIPE_HTTP1 | CURLPIPE_MULTIPLEX);
	curl_multi_setopt(curl, CURLMOPT_MAX_PIPELINE_LENGTH, 32);
}

static int msg_sort(const void* _a, const void* _b){
	struct net_msg* const* a = _a;
	struct net_msg* const* b = _b;
	return (*b)->type - (*a)->type;
}

void net_update(int emask, struct sock* s){

	int curlmask = 0;
	if(emask & EPOLLIN)  curlmask |= CURL_CSELECT_IN;
	if(emask & EPOLLOUT) curlmask |= CURL_CSELECT_OUT;
	if(emask & EPOLLERR) curlmask |= CURL_CSELECT_ERR;

	int blah;
	curl_multi_socket_action(curl, s ? s->fd : CURL_SOCKET_TIMEOUT, curlmask, &blah);

	sb(struct net_msg**) done_list = NULL;

	CURLMsg* cm;
	while((cm = curl_multi_info_read(curl, &blah))){
		if(cm->msg != CURLMSG_DONE) continue;

		char* ptr;
		curl_easy_getinfo(cm->easy_handle, CURLINFO_PRIVATE, &ptr);
		struct client* client = (struct client*)ptr;

		for(struct net_msg** msg = &client->msgs; *msg; msg = &(*msg)->next){
			if((*msg)->curl == cm->easy_handle){
				printf("Recieved mtx msg, type: %d, client: %d\n", (*msg)->type, client->irc_sock);
				sb_push((*msg)->data, 0);
				sb_push(done_list, msg);
				break;
			}
		}
	}

	if(!done_list) return;

	qsort(done_list, sb_count(done_list), sizeof(struct net_msg**), &msg_sort);

	sb_each(m, done_list){
		struct net_msg** msg = *m;

		char* ptr;
		curl_easy_getinfo((*msg)->curl, CURLINFO_PRIVATE, &ptr);
		struct client* client = (struct client*)ptr;

		mtx_recv(client, *msg);

		net_msg_free(*msg);
		*msg = (*msg)->next;
	}

	sb_free(done_list);
}

static size_t net_curl_cb(char* ptr, size_t sz, size_t nmemb, void* data){
	char** out = data;
	const size_t total = sz * nmemb;
	memcpy(sb_add(*out, total), ptr, total);
	return total;
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
		//      so only enable it for sync itself
		curl_easy_setopt(msg->curl, CURLOPT_PIPEWAIT, 1L);
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
