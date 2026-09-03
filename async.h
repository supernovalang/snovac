/* async.h — async/await asynchronous execution and event loop. */
#ifndef SNOVAC_ASYNC_H
#define SNOVAC_ASYNC_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    SN_TASK_PENDING,
    SN_TASK_RUNNING,
    SN_TASK_COMPLETED,
    SN_TASK_FAILED
} SnTaskState;

typedef struct SnAsyncTask SnAsyncTask;
typedef void (*SnAsyncStepFn)(SnAsyncTask *task);

struct SnAsyncTask {
    uint64_t id;
    SnTaskState state;
    SnAsyncStepFn step;
    void *user_data;
    int result_code;
    SnAsyncTask *next;
};

typedef struct SnAsyncLoop {
    SnAsyncTask *head;
    SnAsyncTask *tail;
    uint64_t next_id;
} SnAsyncLoop;

void sn_async_loop_init(SnAsyncLoop *loop);
SnAsyncTask *sn_async_spawn(SnAsyncLoop *loop, SnAsyncStepFn step, void *user_data);
void sn_async_loop_run(SnAsyncLoop *loop);
void sn_async_loop_cleanup(SnAsyncLoop *loop);

#endif /* SNOVAC_ASYNC_H */
