/* native_dispatch.c — Minimal OS ABI dispatch for Snovalang runtime (Sockets & OS only) */
#include "sn_native_dispatch.h"
#include "sn_socket.h"
#include "eval_internal.h"
#include "arena.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void sn_runtime_init(void) {
    sn_socket_init();
}

void sn_runtime_shutdown(void) {
    sn_socket_cleanup();
}

/* Dispatch native calls from the evaluator strictly for OS ABI (Socket / NativeNetwork) */
int sn_native_try_dispatch(Interp *in, const SnDecl *fn, SnList *args, Env *caller,
                           Object *self, SnSpan span, Value *out) {
    (void)span;
    if (!fn || !fn->name) return 0;

    /* Socket / NativeNetwork static methods: minimal OS ABI */
    if (self == NULL || (self->cls && (strcmp(self->cls->name, "Socket") == 0 ||
                                       strcmp(self->cls->name, "NativeNetwork") == 0))) {
        if (strcmp(fn->name, "listen") == 0 && args && args->len >= 2) {
            Value vhost = eval_expr(in, caller, (const SnExpr *)args->items[0]);
            Value vport = eval_expr(in, caller, (const SnExpr *)args->items[1]);
            const char *host = (vhost.kind == V_STRING && vhost.as.s) ? vhost.as.s : "0.0.0.0";
            int port = (vport.kind == V_INT) ? (int)vport.as.i : 8080;
            int handle = sn_socket_listen(host, port);
            *out = v_int(handle);
            return 1;
        }

        if (strcmp(fn->name, "accept") == 0 && args && args->len >= 1) {
            Value vhandle = eval_expr(in, caller, (const SnExpr *)args->items[0]);
            int shandle = (vhandle.kind == V_INT) ? (int)vhandle.as.i : -1;
            char ip[64] = {0};
            int client_handle = sn_socket_accept(shandle, ip, sizeof(ip));
            *out = v_int(client_handle);
            return 1;
        }

        if (strcmp(fn->name, "receive") == 0 && args && args->len >= 2) {
            Value vhandle = eval_expr(in, caller, (const SnExpr *)args->items[0]);
            Value vlen = eval_expr(in, caller, (const SnExpr *)args->items[1]);
            int handle = (vhandle.kind == V_INT) ? (int)vhandle.as.i : -1;
            size_t max_len = (vlen.kind == V_INT && vlen.as.i > 0) ? (size_t)vlen.as.i : 4096;
            char *buf = (char *)malloc(max_len + 1);
            if (!buf) {
                *out = v_str("");
                return 1;
            }
            int64_t n = sn_socket_receive(handle, buf, max_len);
            if (n <= 0) {
                free(buf);
                *out = v_str("");
                return 1;
            }
            buf[n] = '\0';
            char *interned = sn_arena_strndup(in->arena, buf, (size_t)n);
            free(buf);
            *out = v_str(interned);
            return 1;
        }

        if (strcmp(fn->name, "send") == 0 && args && args->len >= 2) {
            Value vhandle = eval_expr(in, caller, (const SnExpr *)args->items[0]);
            Value vdata = eval_expr(in, caller, (const SnExpr *)args->items[1]);
            int handle = (vhandle.kind == V_INT) ? (int)vhandle.as.i : -1;
            const char *data = (vdata.kind == V_STRING && vdata.as.s) ? vdata.as.s : "";
            int64_t sent = sn_socket_send(handle, data, strlen(data));
            *out = v_int(sent);
            return 1;
        }

        if (strcmp(fn->name, "close") == 0 && args && args->len >= 1) {
            Value vhandle = eval_expr(in, caller, (const SnExpr *)args->items[0]);
            int handle = (vhandle.kind == V_INT) ? (int)vhandle.as.i : -1;
            sn_socket_close(handle);
            *out = v_unit();
            return 1;
        }
    }

    return 0;
}
