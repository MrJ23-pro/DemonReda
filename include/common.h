#ifndef ERRAID_COMMON_H
#define ERRAID_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define ERRAID_DEFAULT_RUNDIR_PREFIX "/tmp"
#define ERRAID_DEFAULT_RUNDIR_SUFFIX "/erraid"

#define ERRAID_PIPES_DIR_NAME "pipes"
#define ERRAID_PIPE_REQUEST_NAME "erraid-request-pipe"
#define ERRAID_PIPE_REPLY_NAME "erraid-reply-pipe"

#define ERRAID_TASKS_DIR_NAME "tasks"
#define ERRAID_LOGS_DIR_NAME "logs"
#define ERRAID_STATE_DIR_NAME "state"

#define ERRAID_MAX_COMMAND_ARGS 16
#define ERRAID_MAX_TASK_COMMANDS 16
#define ERRAID_MAX_STDIO_SNAPSHOT 65536
#define ERRAID_STDIO_SNAPSHOT_COUNT 5
#define ERRAID_PIPE_MESSAGE_LIMIT 4096

#define ERRAID_MAGIC 0x44495245u /* "ERID" */
#define ERRAID_PROTO_VERSION 0x01u

typedef enum {
    TASK_TYPE_SIMPLE = 0,
    TASK_TYPE_SEQUENCE = 1,
    TASK_TYPE_ABSTRACT = 2,
} task_type_t;

typedef struct {
    char **argv; /* tableau NULL-terminé, argv[argc] == NULL */
    size_t argc;
} command_t;

typedef struct {
    uint64_t minute_mask; /* 60 bits utilisés */
    uint32_t hour_mask;   /* 24 bits utilisés */
    uint8_t weekday_mask; /* 7 bits utilisés */
    bool enabled;         /* false pour les tâches abstraites */
} schedule_t;

typedef struct {
    uint64_t task_id;
    task_type_t type;
    command_t *commands;
    size_t command_count;
    schedule_t schedule;
    int64_t last_run_epoch;
} task_t;

typedef struct {
    int64_t epoch;
    int32_t status;
    size_t stdout_len;
    size_t stderr_len;
} task_run_entry_t;

typedef enum {
    MSG_PING = 0x01,
    MSG_PONG = 0x02,
    MSG_REQ_LIST_TASKS = 0x10,
    MSG_RSP_LIST_TASKS = 0x11,
    MSG_REQ_CREATE_SIMPLE = 0x20,
    MSG_REQ_CREATE_SEQUENCE = 0x21,
    MSG_REQ_CREATE_ABSTRACT = 0x22,
    MSG_RSP_CREATE = 0x23,
    MSG_REQ_REMOVE = 0x30,
    MSG_RSP_REMOVE = 0x31,
    MSG_REQ_LIST_HISTORY = 0x40,
    MSG_RSP_LIST_HISTORY = 0x41,
    MSG_REQ_GET_STDOUT = 0x50,
    MSG_RSP_GET_STDOUT = 0x51,
    MSG_REQ_GET_STDERR = 0x52,
    MSG_RSP_GET_STDERR = 0x53,
    MSG_REQ_SHUTDOWN = 0x60,
    MSG_RSP_SHUTDOWN = 0x61,
    MSG_RSP_ERROR = 0x7F,
} message_type_t;

typedef struct {
    uint32_t magic;
    uint8_t version;
    uint8_t type;
    uint16_t reserved;
    uint32_t payload_length;
} __attribute__((packed)) message_header_t;

#endif /* ERRAID_COMMON_H */
