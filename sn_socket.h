/* sn_socket.h — Portable OS ABI socket layer for Snovalang runtime */
#ifndef SNOVA_SOCKET_H
#define SNOVA_SOCKET_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize networking subsystem (WSAStartup on Windows, no-op on POSIX) */
int sn_socket_init(void);

/* Shutdown networking subsystem (WSACleanup on Windows, no-op on POSIX) */
void sn_socket_cleanup(void);

/* Create, bind and listen on specified host/port. Returns socket fd or -1 on error. */
int sn_socket_listen(const char *host, int port);

/* Accept incoming connection. Returns client fd or -1 on error.
 * If client_ip is non-NULL and ip_len > 0, writes the client IP string into it. */
int sn_socket_accept(int server_fd, char *client_ip, size_t ip_len);

/* Receive bytes from a socket. Returns bytes read, 0 on disconnect, or -1 on error. */
int64_t sn_socket_receive(int fd, char *buf, size_t max_len);

/* Send bytes to a socket. Returns bytes sent or -1 on error. */
int64_t sn_socket_send(int fd, const char *buf, size_t len);

/* Close an open socket. */
void sn_socket_close(int fd);

#ifdef __cplusplus
}
#endif

#endif /* SNOVA_SOCKET_H */
