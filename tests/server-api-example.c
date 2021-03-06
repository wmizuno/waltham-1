/*
 * Copyright © 2016 DENSO CORPORATION
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <assert.h>
#include <signal.h>

#include <waltham-object.h>
#include <waltham-server.h>
#include <waltham-connection.h>

#include "w-util.h"

#define MAX_EPOLL_WATCHES 2

struct server;
struct client;

/* epoll structure */
struct watch {
	struct server *server;
	int fd;
	void (*cb)(struct watch *w, uint32_t events);
};

/* wthp_region protocol object */
struct region {
	struct wthp_region *obj;
	/* pixman_region32_t region; */
	struct wl_list link; /* struct client::region_list */
};

/* wthp_compositor protocol object */
struct compositor {
	struct wthp_compositor *obj;
	struct client *client;
	struct wl_list link; /* struct client::compositor_list */
};

/* wthp_registry protocol object */
struct registry {
	struct wthp_registry *obj;
	struct client *client;
	struct wl_list link; /* struct client::registry_list */
};

struct client {
	struct wl_list link; /* struct server::client_list */
	struct server *server;

	struct wth_connection *connection;
	struct watch conn_watch;

	/* client object lists for clean-up on disconnection */
	struct wl_list registry_list;   /* struct registry::link */
	struct wl_list compositor_list; /* struct compositor::link */
	struct wl_list region_list;     /* struct region::link */
};

struct server {
	int listen_fd;
	struct watch listen_watch;

	bool running;
	int epoll_fd;

	struct wl_list client_list; /* struct client::link */
};

static int
watch_ctl(struct watch *w, int op, uint32_t events)
{
	struct epoll_event ee;

	ee.events = events;
	ee.data.ptr = w;
	return epoll_ctl(w->server->epoll_fd, op, w->fd, &ee);
}

/* BEGIN wthp_region implementation */

static void
region_destroy(struct region *region)
{
	fprintf(stderr, "region %p destroy\n", region->obj);

	wthp_region_free(region->obj);
	wl_list_remove(&region->link);
	free(region);
}

static void
region_handle_destroy(struct wthp_region *wthp_region)
{
	struct region *region = wth_object_get_user_data((struct wth_object *)wthp_region);

	assert(wthp_region == region->obj);

	region_destroy(region);
}

static void
region_handle_add(struct wthp_region *wthp_region,
		  int32_t x, int32_t y, int32_t width, int32_t height)
{
	fprintf(stderr, "region %p add(%d, %d, %d, %d)\n",
		wthp_region, x, y, width, height);
}

static void
region_handle_subtract(struct wthp_region *wthp_region,
		       int32_t x, int32_t y,
		       int32_t width, int32_t height)
{
	fprintf(stderr, "region %p subtract(%d, %d, %d, %d)\n",
		wthp_region, x, y, width, height);
}

static const struct wthp_region_interface region_implementation = {
	region_handle_destroy,
	region_handle_add,
	region_handle_subtract
};

/* END wthp_region implementation */

/* BEGIN wthp_compositor implementation */

static void
compositor_destroy(struct compositor *comp)
{
	fprintf(stderr, "%s: %p\n", __func__, comp->obj);

	wthp_compositor_free(comp->obj);
	wl_list_remove(&comp->link);
	free(comp);
}

static void
compositor_handle_create_surface(struct wthp_compositor *compositor,
				 struct wthp_surface *id)
{
	wth_object_post_error((struct wth_object *)compositor, 0,
			      "unimplemented: %s", __func__);
}

static void
compositor_handle_create_region(struct wthp_compositor *compositor,
				struct wthp_region *id)
{
	struct compositor *comp = wth_object_get_user_data((struct wth_object *)compositor);
	struct region *region;

	fprintf(stderr, "client %p create region %p\n",
		comp->client, id);

	region = zalloc(sizeof *region);
	if (!region) {
		wth_connection_post_error_no_memory(comp->client->connection);
		return;
	}

	region->obj = id;
	wl_list_insert(&comp->client->region_list, &region->link);

	wthp_region_set_interface(id, &region_implementation, region);
}

static const struct wthp_compositor_interface compositor_implementation = {
	compositor_handle_create_surface,
	compositor_handle_create_region
	/* XXX: protocol is missing destructor */
};

static void
client_bind_compositor(struct client *c, struct wthp_compositor *obj)
{
	struct compositor *comp;

	comp = zalloc(sizeof *comp);
	if (!comp) {
		wth_connection_post_error_no_memory(c->connection);
		return;
	}

	comp->obj = obj;
	comp->client = c;
	wl_list_insert(&c->compositor_list, &comp->link);

	wthp_compositor_set_interface(obj, &compositor_implementation,
				      comp);
	fprintf(stderr, "client %p bound wthp_compositor\n", c);
}

/* END wthp_compositor implementation */

/* BEGIN wthp_registry implementation */

static void
registry_destroy(struct registry *reg)
{
	fprintf(stderr, "%s: %p\n", __func__, reg->obj);

	wthp_registry_free(reg->obj);
	wl_list_remove(&reg->link);
	free(reg);
}

static void
registry_handle_destroy(struct wthp_registry *registry)
{
	struct registry *reg = wth_object_get_user_data((struct wth_object *)registry);

	registry_destroy(reg);
}

static void
registry_handle_bind(struct wthp_registry *registry,
		     uint32_t name,
		     struct wth_object *id,
		     const char *interface,
		     uint32_t version)
{
	struct registry *reg = wth_object_get_user_data((struct wth_object *)registry);

	/* XXX: we could use a database of globals instead of hardcoding them */

	if (strcmp(interface, "wthp_compositor") == 0) {
		/* XXX: check version against limits */
		/* XXX: check that name and interface match */
		client_bind_compositor(reg->client, (struct wthp_compositor *)id);
	} else {
		wth_object_post_error((struct wth_object *)registry, 0,
				      "%s: unknown name %u", __func__, name);
		wth_object_delete(id);
	}
}

const struct wthp_registry_interface registry_implementation = {
	registry_handle_destroy,
	registry_handle_bind
};

/* END wthp_registry implementation */

static void
display_handle_get_registry(struct wthp_registry *registry,
                            void *user_pointer)
{
	struct client *c = (struct client *)user_pointer;
	struct registry *reg;

	reg = zalloc(sizeof *reg);
	if (!reg) {
		wth_connection_post_error_no_memory(c->connection);
		return;
	}

	reg->obj = registry;
	reg->client = c;
	wl_list_insert(&c->registry_list, &reg->link);
	wthp_registry_set_interface(registry,
	                            &registry_implementation, reg);

	/* XXX: advertise our globals */
	wthp_registry_send_global(registry, 1, "wthp_compositor", 4);
}

static void
client_destroy(struct client *c)
{
	struct region *region;
	struct compositor *comp;
	struct registry *reg;

	fprintf(stderr, "Client %p disconnected.\n", c);

	/* clean up remaining client resources in case the client
	 * did not.
	 */
	wl_list_last_until_empty(region, &c->region_list, link)
		region_destroy(region);

	wl_list_last_until_empty(comp, &c->compositor_list, link)
		compositor_destroy(comp);

	wl_list_last_until_empty(reg, &c->registry_list, link)
		registry_destroy(reg);

	wl_list_remove(&c->link);
	watch_ctl(&c->conn_watch, EPOLL_CTL_DEL, 0);
	wth_connection_destroy(c->connection);
	free(c);
}

static void
connection_handle_data(struct watch *w, uint32_t events)
{
	struct client *c = container_of(w, struct client, conn_watch);
	int ret;

	if (events & EPOLLERR) {
		fprintf(stderr, "Client %p errored out.\n", c);
		client_destroy(c);

		return;
	}

	if (events & EPOLLHUP) {
		fprintf(stderr, "Client %p hung up.\n", c);
		client_destroy(c);

		return;
	}

	if (events & EPOLLOUT) {
		ret = wth_connection_flush(c->connection);
		if (ret == 0)
			watch_ctl(&c->conn_watch, EPOLL_CTL_MOD, EPOLLIN);
		else if (ret < 0 && errno != EAGAIN){
			fprintf(stderr, "Client %p flush error.\n", c);
			client_destroy(c);

			return;
		}
	}

	if (events & EPOLLIN) {
		ret = wth_connection_read(c->connection);
		if (ret < 0) {
			fprintf(stderr, "Client %p read error.\n", c);
			client_destroy(c);

			return;
		}

		ret = wth_connection_dispatch(c->connection);
		if (ret < 0 && errno != EPROTO) {
			fprintf(stderr, "Client %p dispatch error.\n", c);
			client_destroy(c);

			return;
		}
	}
}

static struct client *
client_create(struct server *srv, struct wth_connection *conn)
{
	struct client *c;

	c = zalloc(sizeof *c);
	if (!c)
		return NULL;

	c->server = srv;
	c->connection = conn;

	c->conn_watch.server = srv;
	c->conn_watch.fd = wth_connection_get_fd(conn);
	c->conn_watch.cb = connection_handle_data;
	if (watch_ctl(&c->conn_watch, EPOLL_CTL_ADD, EPOLLIN) < 0) {
		free(c);
		return NULL;
	}

	fprintf(stderr, "Client %p connected.\n", c);

	wl_list_insert(&srv->client_list, &c->link);

	wl_list_init(&c->registry_list);
	wl_list_init(&c->compositor_list);
	wl_list_init(&c->region_list);

	wth_connection_set_registry_callback(conn, display_handle_get_registry, c);

	return c;
}

static void
server_flush_clients(struct server *srv)
{
	struct client *c, *tmp;
	int ret;

	wl_list_for_each_safe(c, tmp, &srv->client_list, link) {
		/* Flush out buffered requests. If the Waltham socket is
		 * full, poll it for writable too.
		 */
		ret = wth_connection_flush(c->connection);
		if (ret < 0 && errno == EAGAIN) {
			watch_ctl(&c->conn_watch, EPOLL_CTL_MOD, EPOLLIN | EPOLLOUT);
		} else if (ret < 0) {
			perror("Connection flush failed");
			client_destroy(c);
		}
	}
}

static void
server_accept_client(struct server *srv)
{
	struct client *client;
	struct wth_connection *conn;
	struct sockaddr_in addr;
	socklen_t len;

	len = sizeof addr;
	conn = wth_accept(srv->listen_fd, (struct sockaddr *)&addr, &len);
	if (!conn) {
		fprintf(stderr, "Failed to accept a connection.\n");

		return;
	}

	client = client_create(srv, conn);
	if (!client) {
		fprintf(stderr, "Failed client_create().\n");

		return;
	}
}

static void
listen_socket_handle_data(struct watch *w, uint32_t events)
{
	struct server *srv = container_of(w, struct server, listen_watch);

	if (events & EPOLLERR) {
		fprintf(stderr, "Listening socket errored out.\n");
		srv->running = false;

		return;
	}

	if (events & EPOLLHUP) {
		fprintf(stderr, "Listening socket hung up.\n");
		srv->running = false;

		return;
	}

	if (events & EPOLLIN)
		server_accept_client(srv);
}

static void
mainloop(struct server *srv)
{
	struct epoll_event ee[MAX_EPOLL_WATCHES];
	struct watch *w;
	int count;
	int i;

	srv->running = true;

	while (srv->running) {
		/* Run any idle tasks at this point. */

		server_flush_clients(srv);

		/* Wait for events or signals */
		count = epoll_wait(srv->epoll_fd,
				   ee, ARRAY_LENGTH(ee), -1);
		if (count < 0 && errno != EINTR) {
			perror("Error with epoll_wait");
			break;
		}

		/* Handle all fds, both the listening socket
		 * (see listen_socket_handle_data()) and clients
		 * (see connection_handle_data()).
		 */
		for (i = 0; i < count; i++) {
			w = ee[i].data.ptr;
			w->cb(w, ee[i].events);
		}
	}
}

static int
server_listen(uint16_t tcp_port)
{
	int fd;
	int reuse = 1;
	struct sockaddr_in addr;

	fd = socket(AF_INET, SOCK_STREAM, 0);

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(tcp_port);
	addr.sin_addr.s_addr = htonl(INADDR_ANY);

	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);

	if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
		fprintf(stderr, "Failed to bind to port %d", tcp_port);
		close(fd);
		return -1;
	}

	if (listen(fd, 1024) < 0) {
		fprintf(stderr, "Failed to listen to port %d", tcp_port);
		close (fd);
		return -1;
	}

	return fd;
}

static bool *signal_int_handler_run_flag;

static void
signal_int_handler(int signum)
{
	if (!*signal_int_handler_run_flag)
		abort();

	*signal_int_handler_run_flag = false;
}

static void
set_sigint_handler(bool *running)
{
	struct sigaction sigint;

	signal_int_handler_run_flag = running;
	sigint.sa_handler = signal_int_handler;
	sigemptyset(&sigint.sa_mask);
	sigint.sa_flags = SA_RESETHAND;
	sigaction(SIGINT, &sigint, NULL);
}

int
main(int arcg, char *argv[])
{
	struct server srv = { 0 };
	struct client *c;
	uint16_t tcp_port = 34400;

	set_sigint_handler(&srv.running);

	wl_list_init(&srv.client_list);

	srv.epoll_fd = epoll_create1(EPOLL_CLOEXEC);
	if (srv.epoll_fd == -1) {
		perror("Error on epoll_create1");
		exit(1);
	}

	srv.listen_fd = server_listen(tcp_port);
	if (srv.listen_fd < 0) {
		perror("Error setting up listening socket");
		exit(1);
	}

	srv.listen_watch.server = &srv;
	srv.listen_watch.cb = listen_socket_handle_data;
	srv.listen_watch.fd = srv.listen_fd;
	if (watch_ctl(&srv.listen_watch, EPOLL_CTL_ADD, EPOLLIN) < 0) {
		perror("Error setting up listen polling");
		exit(1);
	}

	printf("Waltham server listening on TCP port %u...\n",
	       tcp_port);

	mainloop(&srv);

	/* destroy all things */
	wl_list_last_until_empty(c, &srv.client_list, link)
		client_destroy(c);

	close(srv.listen_fd);
	close(srv.epoll_fd);

	return 0;
}
