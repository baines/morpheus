#include <stdlib.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "morpheus.h"

static struct client* client_list;

struct client* client_new(int sock){
	struct client* client = calloc(1, sizeof(*client));

	client->epoll_irc_tag = EPOLL_TAG_IRC_CLIENT;
	client->irc_sock = sock;
	client->connect_time = client->last_cmd_time = time(0);

	printf("new client: %d\n", sock);

	struct epoll_event ev = {
		.events = EPOLLIN | EPOLLRDHUP,
		.data.ptr = &client->epoll_irc_tag
	};
	epoll_ctl(global.epoll, EPOLL_CTL_ADD, sock, &ev);

	struct client** c = &client_list;
	while(*c) c = &(*c)->next;
	*c = client;

	return client;
}

void client_del(struct client* client){

	printf("rip client: %d\n", client->irc_sock);

	close(client->irc_sock);

	free(client->irc_nick);
	free(client->irc_user);
	free(client->irc_pass);

	free(client->mtx_token);
	free(client->mtx_since);
	free(client->mtx_server);

	sb_each(id, client->mtx_sent_ids) free(*id);
	sb_free(client->mtx_sent_ids);

	sb_free(client->irc_rooms);
	sb_free(client->irc_buf);

	for(struct net_msg* msg = client->msgs; msg; /**/){
		struct net_msg* tmp = msg->next;
		net_msg_free(msg);
		msg = tmp;
	}

	for(struct client** c = &client_list; *c; c = &(*c)->next){
		if(*c == client){
			*c = (*c)->next;
			break;
		}
	}

	free(client);
}

void client_tick(){
	time_t now = time(0);

	for(struct client** c = &client_list; *c; /**/){
		bool disconnect = false;

		if((*c)->irc_state & IRC_STATE_REGISTERED){
			int cmd_diff = now - (*c)->last_cmd_time;

			if(cmd_diff >= 90){
				disconnect = true;
			} else {
				if(!((*c)->irc_state & IRC_STATE_IDLE) && cmd_diff >= 60){
					send((*c)->irc_sock, "PING :morpheus\r\n", 16, 0);
					(*c)->irc_state |= IRC_STATE_IDLE;
				}

				if((*c)->next_sync && now > (*c)->next_sync){
					mtx_send_sync(*c);
					(*c)->next_sync = 0;
				}
			}
		} else {
			disconnect = now - (*c)->connect_time > 15 || now - (*c)->last_cmd_time > 5;
		}

		if(disconnect){
			printf("client timed out: %d\n", (*c)->irc_sock);
			client_del(*c);
		} else {
			c = &(*c)->next;
		}
	}
}
