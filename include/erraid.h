#ifndef ERRAID_DAEMON_H
#define ERRAID_DAEMON_H

#include "common.h"
#include "proto.h"
#include "scheduler.h"
#include "storage.h"
#include "utils.h"

#include <limits.h>
#include <stddef.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    storage_paths_t paths;
    char root_dir[PATH_MAX];
    char tasks_dir[PATH_MAX];
    char logs_dir[PATH_MAX];
    char state_dir[PATH_MAX];
    char pipes_dir[PATH_MAX];
    char request_pipe_path[PATH_MAX];
    char reply_pipe_path[PATH_MAX];
    task_t *tasks;
    size_t task_count;
    schedule_entry_t *plan;
    size_t plan_capacity;
    size_t plan_count;
    int request_fd;
    int reply_fd;
    int wake_pipe[2];
    int request_dummy_fd;
    bool should_quit;
} erraid_context_t;

int erraid_init(erraid_context_t *ctx, const char *run_dir);

int erraid_run(erraid_context_t *ctx);

void erraid_shutdown(erraid_context_t *ctx);

int erraid_reload_tasks(erraid_context_t *ctx);

int erraid_handle_message(erraid_context_t *ctx, const proto_message_t *request);

int erraid_schedule_loop(erraid_context_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* ERRAID_DAEMON_H */
