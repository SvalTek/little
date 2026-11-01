#pragma once

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <winsock2.h>
typedef SOCKET lt_NetSocket;
#define LT_NET_BAD_SOCKET INVALID_SOCKET
#define LT_NET_SHUTDOWN_BOTH SD_BOTH
#else
#include <sys/socket.h>
#include <netinet/in.h>
typedef int lt_NetSocket;
#define LT_NET_BAD_SOCKET (-1)
#define LT_NET_SHUTDOWN_BOTH SHUT_RDWR
#endif

void lt_net_close(lt_NetSocket socket_fd);
void lt_net_shutdown(lt_NetSocket socket_fd);
int lt_net_nonblocking(lt_NetSocket socket_fd);
void lt_net_no_sigpipe(lt_NetSocket socket_fd);
lt_NetSocket lt_net_listen_ipv4(struct sockaddr_in* address, int* bound_port, const char** error);
char* lt_net_strdup(const char* text);
