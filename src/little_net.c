#include "little_net.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

void lt_net_close(lt_NetSocket socket_fd)
{
	if (socket_fd == LT_NET_BAD_SOCKET) return;
#ifdef _WIN32
	closesocket(socket_fd);
#else
	close(socket_fd);
#endif
}

void lt_net_shutdown(lt_NetSocket socket_fd)
{
	if (socket_fd == LT_NET_BAD_SOCKET) return;
	shutdown(socket_fd, LT_NET_SHUTDOWN_BOTH);
}

int lt_net_nonblocking(lt_NetSocket socket_fd)
{
#ifdef _WIN32
	u_long mode = 1;
	return ioctlsocket(socket_fd, FIONBIO, &mode) == 0;
#else
	int flags = fcntl(socket_fd, F_GETFL, 0);
	return flags >= 0 && fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

void lt_net_no_sigpipe(lt_NetSocket socket_fd)
{
#if defined(SO_NOSIGPIPE)
	int enabled = 1;
	setsockopt(socket_fd, SOL_SOCKET, SO_NOSIGPIPE, (const char*)&enabled, sizeof(enabled));
#else
	(void)socket_fd;
#endif
}

lt_NetSocket lt_net_listen_ipv4(struct sockaddr_in* address, int* bound_port, const char** error)
{
	lt_NetSocket socket_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (socket_fd == LT_NET_BAD_SOCKET) {
		*error = "Failed to create server socket!";
		return LT_NET_BAD_SOCKET;
	}
	int reuse = 1;
	setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
	if (bind(socket_fd, (struct sockaddr*)address, sizeof(*address)) != 0 ||
		listen(socket_fd, 16) != 0 || !lt_net_nonblocking(socket_fd)) {
		lt_net_close(socket_fd);
		*error = "Failed to start server socket!";
		return LT_NET_BAD_SOCKET;
	}
	socklen_t length = sizeof(*address);
	if (getsockname(socket_fd, (struct sockaddr*)address, &length) != 0) {
		lt_net_close(socket_fd);
		*error = "Failed to inspect server socket!";
		return LT_NET_BAD_SOCKET;
	}
	*bound_port = ntohs(address->sin_port);
	return socket_fd;
}

char* lt_net_strdup(const char* text)
{
	if (!text) return 0;
	size_t length = strlen(text);
	char* copy = malloc(length + 1);
	if (copy) memcpy(copy, text, length + 1);
	return copy;
}
