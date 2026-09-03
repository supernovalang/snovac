/* sn_native_dispatch.h — Native method and ABI bridge for Snovalang runtime */
#ifndef SNOVA_NATIVE_DISPATCH_H
#define SNOVA_NATIVE_DISPATCH_H

#include <stddef.h>
#include <stdint.h>
#include "sn_socket.h"
#include "sn_http.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize native runtime subsystems */
void sn_runtime_init(void);

/* Shutdown native runtime subsystems */
void sn_runtime_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* SNOVA_NATIVE_DISPATCH_H */
