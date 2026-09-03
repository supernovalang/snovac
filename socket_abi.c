/* socket_abi.c — Portable OS ABI Socket layer with Handle Table */
#include "sn_socket.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef SOCKET sn_raw_socket_t;
  #define SN_INVALID_SOCKET INVALID_SOCKET
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <errno.h>
  typedef int sn_raw_socket_t;
  #define SN_INVALID_SOCKET (-1)
#endif

#define MAX_HANDLES 1024
static sn_raw_socket_t g_socket_table[MAX_HANDLES];
static int g_network_initialized = 0;

static int handle_alloc(sn_raw_socket_t s) {
    for (int i = 0; i < MAX_HANDLES; i++) {
        if (g_socket_table[i] == SN_INVALID_SOCKET) {
            g_socket_table[i] = s;
            return i;
        }
    }
    return -1;
}

static sn_raw_socket_t handle_get(int handle) {
    if (handle < 0 || handle >= MAX_HANDLES) return SN_INVALID_SOCKET;
    return g_socket_table[handle];
}

static void handle_free(int handle) {
    if (handle >= 0 && handle < MAX_HANDLES) {
        g_socket_table[handle] = SN_INVALID_SOCKET;
    }
}

int sn_socket_init(void) {
    if (!g_network_initialized) {
        for (int i = 0; i < MAX_HANDLES; i++) {
            g_socket_table[i] = SN_INVALID_SOCKET;
        }
#ifdef _WIN32
        WSADATA wsa;
        int res = WSAStartup(MAKEWORD(2, 2), &wsa);
        if (res != 0) {
            fprintf(stderr, "[socket_abi] WSAStartup failed: %d\n", res);
            return -1;
        }
#endif
        g_network_initialized = 1;
    }
    return 0;
}

void sn_socket_cleanup(void) {
    if (g_network_initialized) {
        for (int i = 0; i < MAX_HANDLES; i++) {
            if (g_socket_table[i] != SN_INVALID_SOCKET) {
#ifdef _WIN32
                closesocket(g_socket_table[i]);
#else
                close(g_socket_table[i]);
#endif
                g_socket_table[i] = SN_INVALID_SOCKET;
            }
        }
#ifdef _WIN32
        WSACleanup();
#endif
        g_network_initialized = 0;
    }
}

int sn_socket_listen(const char *host, int port) {
    if (sn_socket_init() != 0) {
        return -1;
    }

    sn_raw_socket_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == SN_INVALID_SOCKET) {
        return -1;
    }

    int opt = 1;
#ifdef _WIN32
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));
#else
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);

    if (!host || strlen(host) == 0 || strcmp(host, "0.0.0.0") == 0 || strcmp(host, "*") == 0) {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (strcmp(host, "localhost") == 0 || strcmp(host, "127.0.0.1") == 0) {
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    } else {
        addr.sin_addr.s_addr = inet_addr(host);
    }

    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
#ifdef _WIN32
        closesocket(s);
#else
        close(s);
#endif
        return -1;
    }

    if (listen(s, SOMAXCONN) != 0) {
#ifdef _WIN32
        closesocket(s);
#else
        close(s);
#endif
        return -1;
    }

    return handle_alloc(s);
}

int sn_socket_accept(int server_handle, char *client_ip, size_t ip_len) {
    sn_raw_socket_t s = handle_get(server_handle);
    if (s == SN_INVALID_SOCKET) {
        return -1;
    }

    struct sockaddr_in client_addr;
#ifdef _WIN32
    int client_len = sizeof(client_addr);
#else
    socklen_t client_len = sizeof(client_addr);
#endif
    memset(&client_addr, 0, sizeof(client_addr));

    sn_raw_socket_t client_s = accept(s, (struct sockaddr *)&client_addr, &client_len);
    if (client_s == SN_INVALID_SOCKET) {
        return -1;
    }

    if (client_ip && ip_len > 0) {
        const char *ip_str = inet_ntoa(client_addr.sin_addr);
        if (ip_str) {
            strncpy(client_ip, ip_str, ip_len - 1);
            client_ip[ip_len - 1] = '\0';
        } else {
            client_ip[0] = '\0';
        }
    }

    return handle_alloc(client_s);
}

int64_t sn_socket_receive(int handle, char *buf, size_t max_len) {
    if (!buf || max_len == 0) return 0;
    sn_raw_socket_t s = handle_get(handle);
    if (s == SN_INVALID_SOCKET) return -1;

#ifdef _WIN32
    int n = recv(s, buf, (int)max_len, 0);
    if (n < 0) return -1;
    return (int64_t)n;
#else
    ssize_t n = recv(s, buf, max_len, 0);
    if (n < 0) return -1;
    return (int64_t)n;
#endif
}

int64_t sn_socket_send(int handle, const char *buf, size_t len) {
    if (!buf || len == 0) return 0;
    sn_raw_socket_t s = handle_get(handle);
    if (s == SN_INVALID_SOCKET) return -1;

    size_t total_sent = 0;
    while (total_sent < len) {
#ifdef _WIN32
        int n = send(s, buf + total_sent, (int)(len - total_sent), 0);
        if (n < 0) return -1;
        total_sent += (size_t)n;
#else
        ssize_t n = send(s, buf + total_sent, len - total_sent, 0);
        if (n < 0) return -1;
        total_sent += (size_t)n;
#endif
    }
    return (int64_t)total_sent;
}

void sn_socket_close(int handle) {
    sn_raw_socket_t s = handle_get(handle);
    if (s == SN_INVALID_SOCKET) return;

#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
    handle_free(handle);
}
