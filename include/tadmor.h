#ifndef ERRAID_TADMOR_H
#define ERRAID_TADMOR_H

#include "common.h"
#include "proto.h"
#include "utils.h"

#include <limits.h>
#include <stdbool.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char pipes_dir[PATH_MAX];
    char request_pipe[PATH_MAX];
    char reply_pipe[PATH_MAX];
    int request_fd;
    int reply_fd;
} tadmor_connection_t;

typedef struct {
    bool opt_list;
    bool opt_shutdown;
    bool opt_create_simple;
    bool opt_create_sequence;
    bool opt_create_abstract;
    bool opt_remove;
    bool opt_history;
    bool opt_stdout;
    bool opt_stderr;
    bool has_schedule;
    char minutes[32];
    char hours[16];
    char weekdays[16];
    uint64_t task_id;
    command_t *commands;
    size_t command_count;
    const char *pipes_dir_arg;
} tadmor_options_t;

int tadmor_parse_args(int argc, char **argv, tadmor_options_t *opts);

int tadmor_connect(tadmor_connection_t *conn, const char *pipes_dir);

void tadmor_close(tadmor_connection_t *conn);

int tadmor_send_request(tadmor_connection_t *conn, const proto_message_t *req);

int tadmor_receive_reply(tadmor_connection_t *conn, proto_message_t *rsp);

int tadmor_handle_reply(const tadmor_options_t *opts, const proto_message_t *rsp);

void tadmor_free_options(tadmor_options_t *opts);

#ifdef __cplusplus
}
#endif

#endif /* ERRAID_TADMOR_H */
