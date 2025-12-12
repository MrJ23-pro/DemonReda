#ifndef ERRAID_EXECUTOR_H
#define ERRAID_EXECUTOR_H

#include "common.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char *stdout_buf;
    size_t stdout_len;
    char *stderr_buf;
    size_t stderr_len;
    int status;
    bool stdout_truncated;
    bool stderr_truncated;
} executor_result_t;

int executor_run_task(const task_t *task, executor_result_t *result);

void executor_result_free(executor_result_t *result);

#ifdef __cplusplus
}
#endif

#endif /* ERRAID_EXECUTOR_H */
