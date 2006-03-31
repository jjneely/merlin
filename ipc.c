#include "shared.h"
#include "logging.h"
#include "ipc.h"
#include "io.h"
#include "protocol.h"

#include <sys/poll.h>
#include <signal.h>

static int ipc_is_module = -1; /* simplify re-initializing the ipc socket */

#define ipc_read_ok(msec) ipc_poll(POLLIN, msec)
#define ipc_write_ok(msec) ipc_poll(POLLOUT, msec)

static char *debug_write, *debug_read;

static int sock = -1; /* for bind() and such */
int ipc_sock = -1; /* once connected, we operate on this */
static char *ipc_sock_path = NULL;


static int ipc_reinit(void)
{
	ipc_deinit(ipc_is_module);

	return ipc_init(ipc_is_module);
}


static int ipc_set_sock_path(const char *path)
{
	int result;
	struct stat st;

	/* the sock-path will be set both from module and daemon,
	 * so path must be absolute */
	if (*path != '/')
		return -1;

	if (strlen(path) > UNIX_PATH_MAX)
		return -1;

	xfree(ipc_sock_path);

	ipc_sock_path = strdup(path);
	if (!ipc_sock_path)
		return -1;

	result = stat(path, &st);
	if (result < 0 && errno != ENOENT)
		return -1;

	if (!result && !(st.st_mode & S_IFSOCK))
		return -2;

	return 0;
}

int ipc_grok_var(char *var, char *val)
{
	if (!val)
		return 0;

	if (!strcmp(var, "ipc_socket"))
		return !ipc_set_sock_path(val);

	if (!strcmp(var, "ipc_debug_write")) {
		debug_write = strdup(val);
		return 1;
	}

	if (!strcmp(var, "ipc_debug_read")) {
		debug_read = strdup(val);
		return 1;
	}

	return 0;
}

int ipc_init(int is_module)
{
	struct sockaddr_un saun;
	struct sockaddr *sa = (struct sockaddr *)&saun;
	socklen_t slen = strlen(ipc_sock_path);

	if (!ipc_sock_path)
		ipc_sock_path = strdup("/opt/monitor/op5/mrd/socket.mrd");

	linfo("Initializing IPC socket '%s' for %s", ipc_sock_path,
	      is_module ? "module" : "daemon");
	ipc_is_module = is_module;

	memset(&saun, 0, sizeof(saun));
	sa->sa_family = AF_UNIX;
	memcpy(saun.sun_path, ipc_sock_path, slen);
	slen += sizeof(struct sockaddr);

	if (!ipc_is_module) {
		if (unlink(ipc_sock_path) && errno != ENOENT)
			return -1;
		ldebug("unlink() didn't complain, or errno == ENOENT");
	}

	if (sock == -1) {
		int optval = 128 << 10; /* set socket buffers to 128KB */
		slen += offsetof(struct sockaddr_un, sun_path);

		sock = socket(AF_UNIX, SOCK_STREAM, 0);
		if (sock < 0) {
			lerr("Failed to obtain ipc socket: %s", strerror(errno));
			return -1;
		}

		setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &optval, sizeof(int));
		setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &optval, sizeof(int));
	}

	if (!ipc_is_module) {
		ldebug("ipc: Trying 'bind' on socket");
		if (bind(sock, sa, slen) < 0) {
			lerr("Failed to bind ipc socket: %s", strerror(errno));
			close(ipc_sock);
			return -1;
		}
		ldebug("Apparently bind() worked well. Socket should be ready now");

		if (listen(sock, 1) < 0)
			ldebug("listen() returned < 0: %s", strerror(errno));

		return sock;
	}

	ldebug("ipc: Trying 'connect()' to local stream socket at '%s'", ipc_sock_path);
	if (connect(sock, sa, slen) < 0) {
		lerr("Failed to connect to ipc socket: %s", strerror(errno));
		switch (errno) {
		case EBADF:
		case ENOTSOCK:
			break;
		case EISCONN:
			return 0;
		default:
			return -1;
		}
		close(sock);
		ipc_sock = sock = -1;
	}
	else {
		ldebug("ipc: connect() seems to have worked OK");

		ipc_sock = sock;

		/* let everybody know we're alive and active */
		linfo("Shoutcasting active status through IPC socket");
		ipc_send_ctrl(CTRL_ACTIVE, -1);
	}

	return sock;
}


int ipc_deinit(int is_module)
{
	int result;

	result = close(ipc_sock);
	close(sock);

	if (!is_module)
		result |= unlink(ipc_sock_path);

	ipc_sock = -1;
	sock = -1;

	return result;
}


static int ipc_is_connected(int msec)
{
	struct sockaddr_un saun;
	socklen_t slen;

	if (ipc_sock == -1) {
		struct pollfd pfd = { sock, POLLIN, 0 };

		slen = sizeof(saun);

		if (ipc_is_module)
			return ipc_reinit();

		if (poll(&pfd, 1, msec) > 0) {
			ipc_sock = accept(sock, (struct sockaddr *)&saun, &slen);
			if (ipc_sock < 0) {
				lerr("ipc: accept() failed: %s", strerror(errno));
				return 0;
			}
		}
	}
	else {
		int result, optval;

		slen = sizeof(optval);
		result = getsockopt(ipc_sock, SOL_SOCKET, SO_ERROR, &optval, &slen);
	}

	ldebug("ipc_is_connected() returning %d, ipc_sock is %d",
		   ipc_sock != -1, ipc_sock);

	return ipc_sock != -1;
}


static int ipc_poll(int events, int msec)
{
	errno = 0;

	if (ipc_is_connected(msec) != 1)
		return 0;

	return io_poll(ipc_sock, events, msec);
}

/* for debugging the ipc communication */
static void binlog(const char *path, const void *buf, int len)
{
	int fd;

	if (!path)
		return;

	fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0666);
	if (fd == -1)
		printf("Failed to open/create '%s': %s\n", path, strerror(errno));
	else {
		write(fd, buf, len);
		close(fd);
	}
}


int ipc_read(void *buf, size_t len, unsigned msec)
{
	int result;

	ldebug("ipc: In ipc_read()");
	result = ipc_read_ok(msec);

	/* read max 4k in one go */
	if (len > 4096)
		return 0;

	if (result < 1) {
		ldebug("ipc: ipc_poll() returned %d: %s (not reading)",
			   result, strerror(errno));

		return result;
	}

	ldebug("ipc: Trying recv(%d, %p, %d, MSG_DONTWAIT | MSG_NOSIGNAL)",
		   ipc_sock, buf, len);
	result = recv(ipc_sock, buf, len, MSG_DONTWAIT | MSG_NOSIGNAL);
	ldebug("ipc: read() returned %d: %s", result, strerror(errno));
	/* if there was inbound data, but none to read that usually means
	 * the other end disconnected or died, so we close the socket and
	 * set it to -1 so that ipc_is_connected() will work properly */
	if (!result) {
		ldebug("ipc: Closing IPC socket to make room for new connection");
		close(ipc_sock);
		ipc_sock = -1;
	}

	if (result > 0)
		binlog(debug_read, buf, len);

	return result;
}

int ipc_send_ctrl(int control_type, int selection)
{
	if (!ipc_is_connected(0))
		return 0;

	return proto_ctrl(ipc_sock, control_type, selection);
}


int ipc_write(const void *buf, size_t len, unsigned msec)
{
	int result = ipc_write_ok(msec);

	if (len > MAX_PKT_SIZE)
		return 0;

	linfo("ipc: Writing %d bytes to ipc socket %d", len, ipc_sock);

	if (result < 1) {
		ldebug("ipc_poll() returned %d: %s (not writing)",
			   result, strerror(errno));

		return result;
	}

	binlog(debug_write, buf, len);

	ldebug("ipc_write: Trying send(%d, %p, %d, MSG_DONTWAIT | MSG_NOSIGNAL)",
		   ipc_sock, buf, len);
	result = send(ipc_sock, buf, len, MSG_DONTWAIT | MSG_NOSIGNAL);
	if (result != len)
		lwarn("ipc_write: send(%d, %p, %d, MSG_DONTWAIT | MSG_NOSIGNAL) returned %d: %s",
			  ipc_sock, buf, len, result, strerror(errno));

	if (result < 0) {
		switch (errno) {
		case ENOTCONN:
			lerr("errno is ENOTCONN");
			break;
		case EFAULT:
			lerr("errno is EFAULT");
			break;
		case EPIPE:
			lerr("errno is EPIPE");
			break;
		case EIO:
			lerr("A low-level IO error occurred. What's that all about?\n");
			break;
		case ENOSPC:
			lerr("Not enough space on the device. Perhaps we need to beef up the receive buffers?\n");
			break;
		case EAGAIN:
			lerr("This shouldn't happen, since the socket isn't non-blocking\n");
			break;
		default:
			lerr("Default write() error fallthrough. Weird, that. trying re-initialization\n");
			ipc_reinit();
			break;
		}
	}

	if (errno == ENOTCONN || errno == EFAULT || errno == EPIPE) {
		lerr("Trying to re-initialize ipc socket. ipc_is_module is %d", ipc_is_module);
		ipc_reinit();
	}

	return result;
}
