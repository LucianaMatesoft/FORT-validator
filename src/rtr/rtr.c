#include "rtr.h"

#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#include "config.h"
#include "clients.h"
#include "log.h"
#include "updates_daemon.h"
#include "rtr/err_pdu.h"
#include "rtr/pdu.h"
#include "rtr/db/vrps.h"

struct thread_param {
	int fd;
	pthread_t tid;
	struct sockaddr_storage addr;
};

static int
init_addrinfo(struct addrinfo **result)
{
	char const *hostname;
	char const *service;
	char *tmp;
	struct addrinfo hints;
	unsigned long parsed, port;
	int error;

	memset(&hints, 0 , sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	/* hints.ai_socktype = SOCK_DGRAM; */
	hints.ai_flags |= AI_PASSIVE;

	hostname = config_get_server_address();
	service = config_get_server_port();

	if (hostname != NULL)
		hints.ai_flags |= AI_CANONNAME;

	error = getaddrinfo(hostname, service, &hints, result);
	if (error) {
		pr_op_err("Could not infer a bindable address out of address '%s' and port '%s': %s",
		    (hostname != NULL) ? hostname : "any", service,
		    gai_strerror(error));
		return error;
	}

	errno = 0;
	parsed = strtoul(service, &tmp, 10);
	if (errno || *tmp != '\0')
		return 0; /* Ok, not a number */

	/*
	 * 'getaddrinfo' isn't very strict validating the service when a port
	 * number is indicated. If a port larger than the max (65535) is
	 * received, the 16 rightmost bits are utilized as the port and set at
	 * the addrinfo returned.
	 *
	 * So, a manual validation is implemented. Port is actually a uint16_t,
	 * so read what's necessary and compare using the same data type.
	 */
	port = (unsigned char)((*result)->ai_addr->sa_data[0]) << 8;
	port += (unsigned char)((*result)->ai_addr->sa_data[1]);
	if (parsed != port)
		return pr_op_err("Service port %s is out of range (max value is %d)",
		    service, USHRT_MAX);

	return 0;
}

/*
 * Creates the socket that will stay put and wait for new connections started
 * from the clients.
 */
static int
create_server_socket(int *result)
{
	struct addrinfo *addrs;
	struct addrinfo *addr;
	unsigned long port;
	int reuse;
	int fd; /* "file descriptor" */
	int error;

	*result = 0; /* Shuts up gcc */
	reuse = 1;

	error = init_addrinfo(&addrs);
	if (error)
		return error;

	for (addr = addrs; addr != NULL; addr = addr->ai_next) {
		pr_op_info(
		    "Attempting to bind socket to address '%s', port '%s'.",
		    (addr->ai_canonname != NULL) ? addr->ai_canonname : "any",
		    config_get_server_port());

		fd = socket(addr->ai_family, SOCK_STREAM, 0);
		if (fd < 0) {
			pr_op_errno(errno, "socket() failed");
			continue;
		}

		/* enable SO_REUSEADDR */
		if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse,
		    sizeof(int)) < 0) {
			pr_op_errno(errno, "setsockopt(SO_REUSEADDR) failed");
			continue;
		}

		/* enable SO_REUSEPORT */
		if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &reuse,
		    sizeof(int)) < 0) {
			pr_op_errno(errno, "setsockopt(SO_REUSEPORT) failed");
			continue;
		}

		if (bind(fd, addr->ai_addr, addr->ai_addrlen) < 0) {
			pr_op_errno(errno, "bind() failed");
			continue;
		}

		error = getsockname(fd, addr->ai_addr, &addr->ai_addrlen);
		if (error) {
			close(fd);
			freeaddrinfo(addrs);
			return pr_op_errno(errno, "getsockname() failed");
		}

		port = (unsigned char)(addr->ai_addr->sa_data[0]) << 8;
		port += (unsigned char)(addr->ai_addr->sa_data[1]);
		pr_op_info("Success; bound to address '%s', port '%ld'.",
		    (addr->ai_canonname != NULL) ? addr->ai_canonname : "any",
		    port);
		freeaddrinfo(addrs);
		*result = fd;
		return 0; /* Happy path */
	}

	freeaddrinfo(addrs);
	return pr_op_err("None of the addrinfo candidates could be bound.");
}

enum verdict {
	/* No errors; continue happily. */
	VERDICT_SUCCESS,
	/* A temporal error just happened. Try again. */
	VERDICT_RETRY,
	/* "Stop whatever you're doing and return." */
	VERDICT_EXIT,
};

/*
 * Converts an error code to a verdict.
 * The error code is assumed to have been spewed by the `accept()` function.
 */
static enum verdict
handle_accept_result(int client_fd, int err)
{
	if (client_fd >= 0)
		return VERDICT_SUCCESS;

	/*
	 * Note: I can't just use a single nice switch because EAGAIN and
	 * EWOULDBLOCK are the same value in at least one supported system
	 * (Linux).
	 */

#if __linux__
	/*
	 * man 2 accept (on Linux):
	 * Linux  accept() (...) passes already-pending network errors on the
	 * new socket as an error code from accept(). This behavior differs from
	 * other BSD socket implementations. For reliable operation the
	 * application should detect the network errors defined for the protocol
	 * after accept() and treat them like EAGAIN by retrying. In the case of
	 * TCP/IP, these are (...)
	 */
	if (err == ENETDOWN || err == EPROTO || err == ENOPROTOOPT
	    || err == EHOSTDOWN || err == ENONET || err == EHOSTUNREACH
	    || err == EOPNOTSUPP || err == ENETUNREACH)
		goto retry;
#endif

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wlogical-op"
	if (err == EAGAIN || err == EWOULDBLOCK)
		goto retry;
#pragma GCC diagnostic pop

	pr_op_info("Client connection attempt not accepted: %s. Quitting...",
	    strerror(err));
	return VERDICT_EXIT;

retry:
	pr_op_info("Client connection attempt not accepted: %s. Retrying...",
	    strerror(err));
	return VERDICT_RETRY;
}

static void
clean_request(struct rtr_request *request, const struct pdu_metadata *meta)
{
	free(request->bytes);
	meta->destructor(request->pdu);
}

static void
print_close_failure(int error, int fd)
{
	struct sockaddr_storage sockaddr;
	char buffer[INET6_ADDRSTRLEN];
	char const *addr_str;

	addr_str = (clients_get_addr(fd, &sockaddr) == 0)
	    ? sockaddr2str(&sockaddr, buffer)
	    : "(unknown)";

	pr_op_errno(error, "close() failed on socket of client %s", addr_str);
}

static void
end_client(int fd)
{
	if (close(fd) != 0)
		print_close_failure(errno, fd);
}

static void
print_client_addr(struct sockaddr_storage *addr, char const *action, int fd)
{
	char buffer[INET6_ADDRSTRLEN];
	pr_op_info("Client %s [ID %d]: %s", action, fd,
	    sockaddr2str(addr, buffer));
}

/*
 * The client socket threads' entry routine.
 * @arg must be released.
 */
static void *
client_thread_cb(void *arg)
{
	struct pdu_metadata const *meta;
	struct rtr_request request;
	struct thread_param param;
	int error;

	memcpy(&param, arg, sizeof(param));
	free(arg);

	error = clients_add(param.fd, param.addr, param.tid);
	if (error) {
		close(param.fd);
		return NULL;
	}

	while (true) { /* For each PDU... */
		error = pdu_load(param.fd, &param.addr, &request, &meta);
		if (error)
			break;

		error = meta->handle(param.fd, &request);
		clean_request(&request, meta);
		if (error)
			break;
	}

	print_client_addr(&param.addr, "closed", param.fd);
	end_client(param.fd);
	clients_forget(param.fd);

	/* Release to avoid the wait till the parent tries to join */
	pthread_detach(param.tid);

	return NULL;
}

/*
 * Waits for client connections and spawns threads to handle them.
 */
static int
handle_client_connections(int server_fd)
{
	struct sigaction ign;
	struct sockaddr_storage client_addr;
	struct thread_param *param;
	socklen_t sizeof_client_addr;
	int client_fd;
	int error;

	/* Ignore SIGPIPES, they're handled apart */
	ign.sa_handler = SIG_IGN;
	sigaction(SIGPIPE, &ign, NULL);

	listen(server_fd, config_get_server_queue());

	sizeof_client_addr = sizeof(client_addr);

	pr_op_debug("Waiting for client connections...");
	do {
		client_fd = accept(server_fd, (struct sockaddr *) &client_addr,
		    &sizeof_client_addr);
		switch (handle_accept_result(client_fd, errno)) {
		case VERDICT_SUCCESS:
			break;
		case VERDICT_RETRY:
			continue;
		case VERDICT_EXIT:
			return -EINVAL;
		}

		print_client_addr(&client_addr, "accepted", client_fd);

		/*
		 * Note: My gut says that errors from now on (even the unknown
		 * ones) should be treated as temporary; maybe the next
		 * accept() will work.
		 * So don't interrupt the thread when this happens.
		 */

		param = malloc(sizeof(struct thread_param));
		if (param == NULL) {
			/* No error response PDU on memory allocation. */
			pr_op_err("Couldn't create thread parameters struct");
			close(client_fd);
			continue;
		}
		param->fd = client_fd;
		param->addr = client_addr;

		error = pthread_create(&param->tid, NULL,
		    client_thread_cb, param);
		if (error && error != EAGAIN)
			/* Error with min RTR version */
			err_pdu_send_internal_error(client_fd, RTR_V0);
		if (error) {
			pr_op_errno(error, "Could not spawn the client's thread");
			close(client_fd);
			free(param);
		}

	} while (true);

	return 0; /* Unreachable. */
}

/*
 * Receive @arg to be called as a clients_foreach_cb
 */
static int
kill_client(struct client *client, void *arg)
{
	end_client(client->fd);
	print_client_addr(&(client->addr), "terminated", client->fd);
	/* Don't call clients_forget to avoid deadlock! */
	return 0;
}

static void
end_clients(void)
{
	clients_foreach(kill_client, NULL);
	/* Let the clients be deleted when clients DB is destroyed */
}

static int
join_thread(pthread_t tid, void *arg)
{
	close_thread(tid, "Client");
	return 0;
}

/*
 * Starts the server, using the current thread to listen for RTR client
 * requests. If configuration parameter 'mode' is STANDALONE, then the
 * server runs "one time" (a.k.a. run the validation just once), it doesn't
 * waits for clients requests.
 *
 * When listening for client requests, this function blocks.
 */
int
rtr_listen(void)
{
	bool changed;
	int server_fd; /* "file descriptor" */
	int error;

	error = clients_db_init();
	if (error)
		return error;

	if (config_get_mode() == STANDALONE) {
		error = vrps_update(&changed);
		if (error)
			pr_op_err("Error %d while trying to update the ROA database.",
			    error);
		goto revert_clients_db; /* Error 0 it's ok */
	}

	error = create_server_socket(&server_fd);
	if (error)
		goto revert_clients_db;

	error = updates_daemon_start();
	if (error)
		goto revert_server_socket;

	error = handle_client_connections(server_fd);

	end_clients();
	updates_daemon_destroy();
revert_server_socket:
	close(server_fd);
revert_clients_db:
	clients_db_destroy(join_thread, NULL);
	return error;
}
