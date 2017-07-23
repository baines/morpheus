#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <locale.h>
#include "morpheus.h"

static int epoll_tag_listen = EPOLL_TAG_IRC_LISTEN;
static int epoll_tag_timer  = EPOLL_TAG_IRC_TIMER;
static int main_sock;
static int irc_timer;

struct global_state global;

void epoll_dispatch(struct epoll_event* e){

	switch(*(int*)e->data.ptr){

		case EPOLL_TAG_IRC_LISTEN: {
			struct sockaddr_storage addr;
			socklen_t size = sizeof(addr);

			int fd = accept(main_sock, (struct sockaddr*)&addr, &size);

			if(fd == -1){
				perror("accept");
			} else {
				// TODO: give addr?
				client_new(fd);
			}
		} break;

		case EPOLL_TAG_IRC_CLIENT: {
			struct client* client = container_of(e->data.ptr, struct client, epoll_irc_tag);

			if(e->events & EPOLLRDHUP){
				client_del(client);
			} else if(e->events & EPOLLIN){
				char buf[1024];
				int n = recv(client->irc_sock, buf, sizeof(buf), 0);
				if(n == -1){
					perror("read");
					client_del(client);
				} else if(n == 0){
					client_del(client);
				} else {
					//printf("client data: %.*s", n, buf);
					irc_recv(client, buf, n);
				}
			}
		} break;

		case EPOLL_TAG_CURL: {
			struct sock* s = container_of(e->data.ptr, struct sock, tag);
			net_update(e->events, s);
		} break;

		case EPOLL_TAG_CURL_TIMER: {
			int timer_fd = ((int*)e->data.ptr)[1];
			uint64_t blah;
			ssize_t ret;
			
			do {
				ret = read(timer_fd, &blah, 8);
			} while(ret == -1 && errno == EAGAIN);

			net_update(e->events, NULL);
		} break;

		// TODO: disable timer when num clients == 0;
		case EPOLL_TAG_IRC_TIMER: {
			uint64_t blah;
			ssize_t ret;

			do {
				ret = read(irc_timer, &blah, 8);
			} while(ret == -1 && errno == EAGAIN);

			client_tick();
		} break;
	}
}

int main(int argc, char** argv){
	srand(time(NULL) ^ (getpid() << 10));
	assert(setlocale(LC_CTYPE, "C.UTF-8"));

	global.mtx_server_base_url = getenv("MTX_URL");
	if(!global.mtx_server_base_url){
		global.mtx_server_base_url = "https://localhost:8448";
	}

	global.mtx_server_name = getenv("MTX_NAME");
	if(!global.mtx_server_name){
		global.mtx_server_name = "localhost";
	}

	global.epoll = epoll_create(16);
	main_sock = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);

	int listen_port = 1999;
	const char* port_str = getenv("MTX_LISTEN_PORT");
	if(port_str){
		listen_port = atoi(port_str);
	}

	if(main_sock == -1){
		perror("socket");
	}

	struct sockaddr_in in = {
		.sin_family = AF_INET,
		.sin_port = htons(listen_port),
		.sin_addr.s_addr = INADDR_ANY,
	};

	setsockopt(main_sock, SOL_SOCKET, SO_REUSEADDR, (int[]){ 1 }, sizeof(int));

	if(bind(main_sock, &in, sizeof(in)) == -1){
		perror("bind");
	}

	if(listen(main_sock, 16) == -1){
		perror("listen");
	}

	struct epoll_event ev = { .events = EPOLLIN, .data.ptr = &epoll_tag_listen };
	epoll_ctl(global.epoll, EPOLL_CTL_ADD, main_sock, &ev);

	irc_timer = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	struct itimerspec it = {
		.it_value    = { .tv_sec = 5 },
		.it_interval = { .tv_sec = 5 }
	};
	timerfd_settime(irc_timer, 0, &it, NULL);

	ev.data.ptr = &epoll_tag_timer;
	epoll_ctl(global.epoll, EPOLL_CTL_ADD, irc_timer, &ev);

	net_init();

	while(1){
		struct epoll_event buf[8];

		int n = epoll_wait(global.epoll, buf, 8, -1);
		//printf("epoll wakeup: %d\n", n);

		if(n < 0){
			perror("epoll");
			continue;
		}

		for(int i = 0; i < n; ++i){
			epoll_dispatch(buf + i);
		}
	}

	return 0;
}
