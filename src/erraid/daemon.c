#include "erraid.h"

#include "executor.h"
#include "notifier.h"
#include "proto.h"
#include "storage.h"
#include "utils.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#endif

typedef struct {
    command_t *commands;
    size_t count;
} command_array_t;

static void free_command(command_t *command) {
    if (command == NULL || command->argv == NULL) {
        return;
    }
    for (size_t i = 0; i < command->argc; ++i) {
        free(command->argv[i]);
        command->argv[i] = NULL;
    }
    free(command->argv);
    command->argv = NULL;
    command->argc = 0;
}

static void free_task_contents(task_t *task) {
    if (task == NULL) {
        return;
    }
    if (task->commands != NULL) {
        for (size_t i = 0; i < task->command_count; ++i) {
            free_command(&task->commands[i]);
        }
        free(task->commands);
        task->commands = NULL;
    }
    task->command_count = 0;
}

static void free_command_array(command_array_t *array) {
    if (array == NULL || array->commands == NULL) {
        return;
    }
    for (size_t i = 0; i < array->count; ++i) {
        free_command(&array->commands[i]);
    }
    free(array->commands);
    array->commands = NULL;
    array->count = 0;
}

static const char *skip_ws(const char *p) {
    if (p == NULL) {
        return NULL;
    }
    while (*p && isspace((unsigned char)*p)) {
        ++p;
    }
    return p;
}

static int parse_json_string_token(const char **cursor, char **out) {
    if (cursor == NULL || out == NULL || *cursor == NULL) {
        errno = EINVAL;
        return -1;
    }
    const char *p = *cursor;
    if (*p != '"') {
        errno = EINVAL;
        return -1;
    }
    ++p;

    size_t capacity = 32;
    size_t length = 0;
    char *buffer = malloc(capacity);
    if (buffer == NULL) {
        errno = ENOMEM;
        return -1;
    }
    buffer[0] = '\0';

    while (*p) {
        char ch = *p++;
        if (ch == '"') {
            buffer[length] = '\0';
            *cursor = p;
            *out = buffer;
            return 0;
        }
        if (ch == '\\') {
            if (*p == '\0') {
                free(buffer);
                errno = EINVAL;
                return -1;
            }
            char esc = *p++;
            switch (esc) {
                case '"': ch = '"'; break;
                case '\\': ch = '\\'; break;
                case '/': ch = '/'; break;
                case 'b': ch = '\b'; break;
                case 'f': ch = '\f'; break;
                case 'n': ch = '\n'; break;
                case 'r': ch = '\r'; break;
                case 't': ch = '\t'; break;
                default:
                    free(buffer);
                    errno = EINVAL;
                    return -1;
            }
        }
        if (length + 1 >= capacity) {
            size_t new_cap = capacity * 2;
            char *tmp = realloc(buffer, new_cap);
            if (tmp == NULL) {
                free(buffer);
                errno = ENOMEM;
                return -1;
            }
            buffer = tmp;
            capacity = new_cap;
        }
        buffer[length++] = ch;
    }

    free(buffer);
    errno = EINVAL;
    return -1;
}

static int buffer_append(char *buffer, size_t cap, size_t *offset, const char *fmt, ...);
static int send_error_response(erraid_context_t *ctx, const char *code, const char *message);
static int send_json_response(erraid_context_t *ctx, message_type_t type, const char *payload, size_t length);
static int send_status_ok(erraid_context_t *ctx, message_type_t type);
static int rebuild_plan(erraid_context_t *ctx);
static int json_extract_uint64(const char *json, const char *field, uint64_t *value);

static int parse_commands_array(const char **cursor, task_type_t type, command_array_t *out_array) {
    (void)type;
    if (cursor == NULL || *cursor == NULL || out_array == NULL) {
        errno = EINVAL;
        return -1;
    }

    const char *p = skip_ws(*cursor);
    if (*p != '[') {
        errno = EINVAL;
        return -1;
    }
    ++p;

    command_array_t result = {.commands = NULL, .count = 0};

    while (1) {
        p = skip_ws(p);
        if (*p == ']') {
            ++p;
            break;
        }
        if (*p != '[') {
            free_command_array(&result);
            errno = EINVAL;
            return -1;
        }
        ++p;

        command_t cmd;
        memset(&cmd, 0, sizeof(cmd));

        size_t capacity = 4;
        cmd.argv = calloc(capacity + 1, sizeof(char *));
        if (cmd.argv == NULL) {
            free_command_array(&result);
            errno = ENOMEM;
            return -1;
        }

        while (1) {
            p = skip_ws(p);
            if (*p == ']') {
                ++p;
                break;
            }
            if (*p != '"') {
                free_command(&cmd);
                free_command_array(&result);
                errno = EINVAL;
                return -1;
            }
            const char *str_cursor = p;
            char *element = NULL;
            if (parse_json_string_token(&str_cursor, &element) != 0) {
                free_command(&cmd);
                free_command_array(&result);
                return -1;
            }
            p = str_cursor;

            if (cmd.argc >= ERRAID_MAX_COMMAND_ARGS) {
                free(element);
                free_command(&cmd);
                free_command_array(&result);
                errno = E2BIG;
                return -1;
            }
            if (cmd.argc >= capacity) {
                size_t new_cap = capacity * 2;
                char **tmp = realloc(cmd.argv, (new_cap + 1) * sizeof(char *));
                if (tmp == NULL) {
                    free(element);
                    free_command(&cmd);
                    free_command_array(&result);
                    errno = ENOMEM;
                    return -1;
                }
                capacity = new_cap;
                cmd.argv = tmp;
            }
            cmd.argv[cmd.argc++] = element;
            cmd.argv[cmd.argc] = NULL;

            p = skip_ws(p);
            if (*p == ',') {
                ++p;
                continue;
            }
            if (*p == ']') {
                ++p;
                break;
            }
            free_command(&cmd);
            free_command_array(&result);
            errno = EINVAL;
            return -1;
        }

        if (result.count >= ERRAID_MAX_TASK_COMMANDS) {
            free_command(&cmd);
            free_command_array(&result);
            errno = E2BIG;
            return -1;
        }

        command_t *tmp = realloc(result.commands, (result.count + 1) * sizeof(command_t));
        if (tmp == NULL) {
            free_command(&cmd);
            free_command_array(&result);
            errno = ENOMEM;
            return -1;
        }
        result.commands = tmp;
        result.commands[result.count] = cmd;
        ++result.count;

        p = skip_ws(p);
        if (*p == ',') {
            ++p;
            continue;
        }
        if (*p == ']') {
            ++p;
            break;
        }
    }

    *cursor = p;
    *out_array = result;
    return 0;
}

static int find_field_pointer(const char *json, const char *key, const char **out_value) {
    if (json == NULL || key == NULL || out_value == NULL) {
        errno = EINVAL;
        return -1;
    }
    char pattern[64];
    int n = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (n < 0 || (size_t)n >= sizeof(pattern)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    const char *pos = strstr(json, pattern);
    if (pos == NULL) {
        errno = ENOENT;
        return -1;
    }
    pos += strlen(pattern);
    pos = skip_ws(pos);
    if (*pos != ':') {
        errno = EINVAL;
        return -1;
    }
    ++pos;
    pos = skip_ws(pos);
    *out_value = pos;
    return 0;
}

static int parse_schedule_field(const char *payload, task_type_t type, schedule_t *schedule) {
    if (schedule == NULL) {
        errno = EINVAL;
        return -1;
    }

    memset(schedule, 0, sizeof(*schedule));
    schedule->enabled = (type != TASK_TYPE_ABSTRACT);

    const char *value_ptr = NULL;
    if (find_field_pointer(payload, "schedule", &value_ptr) != 0) {
        if (type == TASK_TYPE_ABSTRACT) {
            schedule->enabled = false;
            errno = 0;
            return 0;
        }
        return -1;
    }

    if (strncmp(value_ptr, "null", 4) == 0) {
        if (type == TASK_TYPE_ABSTRACT) {
            schedule->enabled = false;
            return 0;
        }
        errno = EINVAL;
        return -1;
    }

    if (*value_ptr != '{') {
        errno = EINVAL;
        return -1;
    }

    const char *cursor = value_ptr + 1;
    char *minutes_str = NULL;
    char *hours_str = NULL;
    char *weekdays_str = NULL;

    for (int i = 0; i < 3; ++i) {
        cursor = skip_ws(cursor);
        char *field_name = NULL;
        if (*cursor != '"') {
            goto schedule_parse_error;
        }
        if (parse_json_string_token(&cursor, &field_name) != 0) {
            goto schedule_parse_error;
        }
        cursor = skip_ws(cursor);
        if (*cursor != ':') {
            free(field_name);
            goto schedule_parse_error;
        }
        ++cursor;
        cursor = skip_ws(cursor);
        if (*cursor != '"') {
            free(field_name);
            goto schedule_parse_error;
        }
        char *value = NULL;
        if (parse_json_string_token(&cursor, &value) != 0) {
            free(field_name);
            goto schedule_parse_error;
        }

        if (strcmp(field_name, "minutes") == 0) {
            minutes_str = value;
        } else if (strcmp(field_name, "hours") == 0) {
            hours_str = value;
        } else if (strcmp(field_name, "weekdays") == 0) {
            weekdays_str = value;
        } else {
            free(value);
        }
        free(field_name);

        cursor = skip_ws(cursor);
        if (*cursor == ',') {
            ++cursor;
            continue;
        }
        if (*cursor == '}') {
            ++cursor;
            break;
        }
    }

    if (minutes_str == NULL || hours_str == NULL || weekdays_str == NULL) {
        goto schedule_parse_error;
    }

    errno = 0;
    schedule->minute_mask = strtoull(minutes_str, NULL, 16);
    schedule->hour_mask = (uint32_t)strtoul(hours_str, NULL, 16);
    schedule->weekday_mask = (uint8_t)strtoul(weekdays_str, NULL, 16);

    free(minutes_str);
    free(hours_str);
    free(weekdays_str);
    return 0;

schedule_parse_error:
    free(minutes_str);
    free(hours_str);
    free(weekdays_str);
    errno = EINVAL;
    return -1;
}

static int parse_commands_field(const char *payload, task_type_t type, command_array_t *out_commands) {
    const char *value_ptr = NULL;
    if (find_field_pointer(payload, "commands", &value_ptr) != 0) {
        return -1;
    }

    if (parse_commands_array(&value_ptr, type, out_commands) != 0) {
        return -1;
    }

    if (type == TASK_TYPE_SIMPLE && out_commands->count != 1) {
        errno = EINVAL;
        free_command_array(out_commands);
        return -1;
    }
    if (type == TASK_TYPE_SEQUENCE && out_commands->count < 1) {
        errno = EINVAL;
        free_command_array(out_commands);
        return -1;
    }
    return 0;
}

static void wake_scheduler(erraid_context_t *ctx) {
    if (ctx == NULL) {
        return;
    }
    if (ctx->wake_pipe[1] >= 0) {
        const uint8_t byte = 0xFF;
        ssize_t rc = write(ctx->wake_pipe[1], &byte, sizeof(byte));
        (void)rc;
    }
}

static int context_add_task(erraid_context_t *ctx, task_t *task) {
    if (ctx == NULL || task == NULL) {
        errno = EINVAL;
        return -1;
    }
    task_t *tmp = realloc(ctx->tasks, (ctx->task_count + 1) * sizeof(task_t));
    if (tmp == NULL) {
        return -1;
    }
    ctx->tasks = tmp;
    ctx->tasks[ctx->task_count] = *task;
    ctx->task_count += 1;
    memset(task, 0, sizeof(*task));
    return 0;
}

static ssize_t context_find_task_index(const erraid_context_t *ctx, uint64_t task_id) {
    if (ctx == NULL) {
        return -1;
    }
    for (size_t i = 0; i < ctx->task_count; ++i) {
        if (ctx->tasks[i].task_id == task_id) {
            return (ssize_t)i;
        }
    }
    return -1;
}

static int context_remove_task(erraid_context_t *ctx, size_t index) {
    if (ctx == NULL || index >= ctx->task_count) {
        errno = EINVAL;
        return -1;
    }
    free_task_contents(&ctx->tasks[index]);
    if (index + 1 < ctx->task_count) {
        memmove(&ctx->tasks[index], &ctx->tasks[index + 1], (ctx->task_count - index - 1) * sizeof(task_t));
    }
    ctx->task_count -= 1;
    if (ctx->task_count == 0) {
        free(ctx->tasks);
        ctx->tasks = NULL;
    }
    return 0;
}

static task_type_t task_type_from_message(message_type_t msg_type) {
    switch (msg_type) {
        case MSG_REQ_CREATE_SIMPLE: return TASK_TYPE_SIMPLE;
        case MSG_REQ_CREATE_SEQUENCE: return TASK_TYPE_SEQUENCE;
        case MSG_REQ_CREATE_ABSTRACT: return TASK_TYPE_ABSTRACT;
        default: return TASK_TYPE_SIMPLE;
    }
}

static int handle_create_task(erraid_context_t *ctx, message_type_t msg_type, const char *payload) {
    task_type_t type = task_type_from_message(msg_type);

    schedule_t schedule;
    if (parse_schedule_field(payload, type, &schedule) != 0) {
        send_error_response(ctx, "INVALID_REQUEST", "Planification invalide");
        return -1;
    }

    command_array_t commands = {.commands = NULL, .count = 0};
    if (parse_commands_field(payload, type, &commands) != 0) {
        send_error_response(ctx, "INVALID_REQUEST", "Commandes invalides");
        return -1;
    }

    task_t new_task;
    memset(&new_task, 0, sizeof(new_task));
    new_task.type = type;
    new_task.schedule = schedule;
    new_task.last_run_epoch = -1;
    new_task.command_count = commands.count;
    new_task.commands = commands.commands;
    commands.commands = NULL;
    commands.count = 0;

    if (storage_allocate_task_id(&ctx->paths, &new_task.task_id) != 0) {
        free_task_contents(&new_task);
        send_error_response(ctx, "PERSISTENCE_ERROR", "Allocation d'identifiant impossible");
        return -1;
    }

    if (storage_write_task(&ctx->paths, &new_task) != 0) {
        free_task_contents(&new_task);
        send_error_response(ctx, "PERSISTENCE_ERROR", "Écriture de la tâche impossible");
        return -1;
    }

    if (context_add_task(ctx, &new_task) != 0) {
        storage_remove_task(&ctx->paths, new_task.task_id);
        free_task_contents(&new_task);
        send_error_response(ctx, "MEMORY_ERROR", "Ajout en mémoire impossible");
        return -1;
    }

    if (rebuild_plan(ctx) != 0) {
        int saved_errno = errno;
        erraid_reload_tasks(ctx);
        errno = saved_errno;
        send_error_response(ctx, "SCHEDULER_ERROR", "Reconstruction de plan impossible");
        return -1;
    }

    wake_scheduler(ctx);

    char payload_buf[128];
    size_t offset = 0;
    if (buffer_append(payload_buf, sizeof(payload_buf), &offset, "{\"status\":\"OK\",\"task_id\":%llu}",
                      (unsigned long long)ctx->tasks[ctx->task_count - 1].task_id) != 0) {
        send_error_response(ctx, "ENCODING_ERROR", "Construction de réponse impossible");
        return -1;
    }

    if (send_json_response(ctx, MSG_RSP_CREATE, payload_buf, offset) != 0) {
        return -1;
    }

    return 0;
}

static int handle_remove_task(erraid_context_t *ctx, const char *payload) {
    uint64_t task_id = 0;
    if (json_extract_uint64(payload, "task_id", &task_id) != 0) {
        send_error_response(ctx, "INVALID_REQUEST", "task_id manquant ou invalide");
        return -1;
    }

    ssize_t index = context_find_task_index(ctx, task_id);
    if (index < 0) {
        send_error_response(ctx, "TASK_NOT_FOUND", "Tâche inconnue");
        return -1;
    }

    if (storage_remove_task(&ctx->paths, task_id) != 0) {
        send_error_response(ctx, "PERSISTENCE_ERROR", "Suppression disque impossible");
        return -1;
    }

    if (context_remove_task(ctx, (size_t)index) != 0) {
        erraid_reload_tasks(ctx);
        send_error_response(ctx, "MEMORY_ERROR", "Suppression mémoire impossible");
        return -1;
    }

    if (rebuild_plan(ctx) != 0) {
        int saved_errno = errno;
        erraid_reload_tasks(ctx);
        errno = saved_errno;
        send_error_response(ctx, "SCHEDULER_ERROR", "Reconstruction de plan impossible");
        return -1;
    }

    wake_scheduler(ctx);
    return send_status_ok(ctx, MSG_RSP_REMOVE);
}

static int buffer_append(char *buffer, size_t buflen, size_t *offset, const char *fmt, ...) {
    if (buffer == NULL || offset == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (*offset >= buflen) {
        errno = ENOSPC;
        return -1;
    }

    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(buffer + *offset, buflen - *offset, fmt, args);
    va_end(args);

    if (written < 0) {
        return -1;
    }
    if ((size_t)written >= buflen - *offset) {
        errno = ENOSPC;
        return -1;
    }

    *offset += (size_t)written;
    return 0;
}

static void drain_fd(int fd) {
    char tmp[128];
    while (1) {
        ssize_t n = read(fd, tmp, sizeof(tmp));
        if (n <= 0) {
            if (n < 0 && (errno == EINTR)) {
                continue;
            }
            break;
        }
        if (n < (ssize_t)sizeof(tmp)) {
            break;
        }
    }
}

static int set_fd_flags(int fd, int flags) {
    int current = fcntl(fd, F_GETFL, 0);
    if (current < 0) {
        return -1;
    }
    if (fcntl(fd, F_SETFL, current | flags) < 0) {
        return -1;
    }
    return 0;
}

static int clear_fd_flags(int fd, int flags) {
    int current = fcntl(fd, F_GETFL, 0);
    if (current < 0) {
        return -1;
    }
    if (fcntl(fd, F_SETFL, current & ~flags) < 0) {
        return -1;
    }
    return 0;
}

static const char *task_type_to_string(task_type_t type) {
    switch (type) {
        case TASK_TYPE_SIMPLE: return "SIMPLE";
        case TASK_TYPE_SEQUENCE: return "SEQUENCE";
        case TASK_TYPE_ABSTRACT: return "ABSTRACT";
        default: return "UNKNOWN";
    }
}

static int json_extract_uint64(const char *json, const char *key, uint64_t *out_value) {
    if (json == NULL || key == NULL || out_value == NULL) {
        errno = EINVAL;
        return -1;
    }

    char pattern[64];
    int n = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (n < 0 || (size_t)n >= sizeof(pattern)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    const char *pos = strstr(json, pattern);
    if (pos == NULL) {
        errno = ENOENT;
        return -1;
    }
    pos += strlen(pattern);
    while (*pos != '\0' && isspace((unsigned char)*pos)) {
        ++pos;
    }
    if (*pos != ':') {
        errno = EINVAL;
        return -1;
    }
    ++pos;
    while (*pos != '\0' && isspace((unsigned char)*pos)) {
        ++pos;
    }
    if (!isdigit((unsigned char)*pos) && *pos != '+') {
        errno = EINVAL;
        return -1;
    }

    char *endptr = NULL;
    errno = 0;
    unsigned long long value = strtoull(pos, &endptr, 10);
    if (errno != 0 || endptr == pos) {
        if (errno == 0) {
            errno = EINVAL;
        }
        return -1;
    }
    *out_value = (uint64_t)value;
    return 0;
}

static int send_json_response(erraid_context_t *ctx, message_type_t type, const char *payload, size_t payload_len) {
    if (ctx == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (payload == NULL) {
        payload = "";
        payload_len = 0;
    }

    if (ctx->reply_fd < 0) {
        ctx->reply_fd = open(ctx->reply_pipe_path, O_RDWR | O_NONBLOCK);
        if (ctx->reply_fd < 0) {
            return -1;
        }
    }

    proto_message_t message;
    if (proto_pack(type, payload, payload_len, &message) != 0) {
        return -1;
    }

    if (proto_write_message(ctx->reply_fd, &message) != 0) {
        if (errno == EPIPE || errno == ENXIO) {
            close(ctx->reply_fd);
            ctx->reply_fd = -1;
            errno = 0;
            return 0;
        }
        return -1;
    }

    return 0;
}

static int send_status_ok(erraid_context_t *ctx, message_type_t type) {
    const char payload[] = "{\"status\":\"OK\"}";
    return send_json_response(ctx, type, payload, sizeof(payload) - 1);
}

static int send_error_response(erraid_context_t *ctx, const char *code, const char *message) {
    char payload[256];
    size_t offset = 0;
    if (buffer_append(payload, sizeof(payload), &offset, "{\"status\":\"ERROR\"")) {
        return -1;
    }
    if (code != NULL) {
        if (buffer_append(payload, sizeof(payload), &offset, ",\"code\":\"%s\"", code)) {
            return -1;
        }
    }
    if (message != NULL) {
        if (buffer_append(payload, sizeof(payload), &offset, ",\"message\":\"%s\"", message)) {
            return -1;
        }
    }
    if (buffer_append(payload, sizeof(payload), &offset, "}")) {
        return -1;
    }
    return send_json_response(ctx, MSG_RSP_ERROR, payload, offset);
}

static int respond_list_tasks(erraid_context_t *ctx) {
    char payload[ERRAID_PIPE_MESSAGE_LIMIT];
    size_t offset = 0;

    if (buffer_append(payload, sizeof(payload), &offset, "{\"status\":\"OK\",\"tasks\":[")) {
        return -1;
    }

    for (size_t i = 0; i < ctx->task_count; ++i) {
        const task_t *task = &ctx->tasks[i];
        if (i > 0) {
            if (buffer_append(payload, sizeof(payload), &offset, ",")) {
                return -1;
            }
        }
        char minutes[16];
        char hours[8];
        char weekdays[4];
        snprintf(minutes, sizeof(minutes), "%015llX", (unsigned long long)task->schedule.minute_mask);
        snprintf(hours, sizeof(hours), "%06X", task->schedule.hour_mask & 0xFFFFFFu);
        snprintf(weekdays, sizeof(weekdays), "%02X", task->schedule.weekday_mask & 0x7Fu);

        if (buffer_append(payload,
                          sizeof(payload),
                          &offset,
                          "{\"task_id\":%llu,\"type\":\"%s\",\"last_run\":%lld,"
                          "\"schedule\":{\"minutes\":\"%s\",\"hours\":\"%s\",\"weekdays\":\"%s\"}}",
                          (unsigned long long)task->task_id,
                          task_type_to_string(task->type),
                          (long long)task->last_run_epoch,
                          minutes,
                          hours,
                          weekdays)) {
            return -1;
        }
    }

    if (buffer_append(payload, sizeof(payload), &offset, "]}")) {
        return -1;
    }

    return send_json_response(ctx, MSG_RSP_LIST_TASKS, payload, offset);
}

static int respond_history(erraid_context_t *ctx, uint64_t task_id) {
    task_run_entry_t *entries = NULL;
    size_t entry_count = 0;
    if (storage_load_history(&ctx->paths, task_id, &entries, &entry_count) != 0) {
        return -1;
    }

    char payload[ERRAID_PIPE_MESSAGE_LIMIT];
    size_t offset = 0;
    if (buffer_append(payload, sizeof(payload), &offset, "{\"status\":\"OK\",\"history\":[")) {
        free(entries);
        return -1;
    }

    for (size_t i = 0; i < entry_count; ++i) {
        if (i > 0) {
            if (buffer_append(payload, sizeof(payload), &offset, ",")) {
                free(entries);
                return -1;
            }
        }
        if (buffer_append(payload,
                          sizeof(payload),
                          &offset,
                          "{\"epoch\":%lld,\"status\":%d,\"stdout_len\":%zu,\"stderr_len\":%zu}",
                          (long long)entries[i].epoch,
                          entries[i].status,
                          entries[i].stdout_len,
                          entries[i].stderr_len)) {
            free(entries);
            return -1;
        }
    }

    if (buffer_append(payload, sizeof(payload), &offset, "]}")) {
        free(entries);
        return -1;
    }

    free(entries);
    return send_json_response(ctx, MSG_RSP_LIST_HISTORY, payload, offset);
}

static int respond_stdio(erraid_context_t *ctx, uint64_t task_id, bool stdout_request) {
    void *stdout_buf = NULL;
    size_t stdout_len = 0;
    void *stderr_buf = NULL;
    size_t stderr_len = 0;

    if (storage_load_last_stdio(&ctx->paths, task_id, &stdout_buf, &stdout_len, &stderr_buf, &stderr_len) != 0) {
        if (stdout_buf != NULL) {
            free(stdout_buf);
        }
        if (stderr_buf != NULL) {
            free(stderr_buf);
        }
        return -1;
    }

    const void *target_buf = stdout_request ? stdout_buf : stderr_buf;
    size_t target_len = stdout_request ? stdout_len : stderr_len;

    size_t required = ((target_len + 2) / 3) * 4;
    if (required + 64 >= ERRAID_PIPE_MESSAGE_LIMIT) {
        free(stdout_buf);
        free(stderr_buf);
        errno = EMSGSIZE;
        return -1;
    }

    char payload[ERRAID_PIPE_MESSAGE_LIMIT];
    size_t offset = 0;
    char encoded[ERRAID_PIPE_MESSAGE_LIMIT];
    size_t encoded_len = sizeof(encoded);
    if (utils_base64_encode(target_buf, target_len, encoded, &encoded_len) != 0) {
        free(stdout_buf);
        free(stderr_buf);
        return -1;
    }

    if (buffer_append(payload,
                      sizeof(payload),
                      &offset,
                      "{\"status\":\"OK\",\"%s\":\"%s\"}",
                      stdout_request ? "stdout" : "stderr",
                      encoded)) {
        free(stdout_buf);
        free(stderr_buf);
        return -1;
    }

    free(stdout_buf);
    free(stderr_buf);
    return send_json_response(ctx,
                              stdout_request ? MSG_RSP_GET_STDOUT : MSG_RSP_GET_STDERR,
                              payload,
                              offset);
}

static int ensure_plan_capacity(erraid_context_t *ctx, size_t capacity) {
    if (capacity <= ctx->plan_capacity) {
        return 0;
    }
    size_t new_capacity = ctx->plan_capacity == 0 ? 4 : ctx->plan_capacity;
    while (new_capacity < capacity) {
        new_capacity *= 2;
    }
    schedule_entry_t *tmp = realloc(ctx->plan, new_capacity * sizeof(schedule_entry_t));
    if (tmp == NULL) {
        errno = ENOMEM;
        return -1;
    }
    ctx->plan = tmp;
    ctx->plan_capacity = new_capacity;
    return 0;
}

static int rebuild_plan(erraid_context_t *ctx) {
    if (ensure_plan_capacity(ctx, ctx->task_count) != 0) {
        return -1;
    }
    if (ctx->task_count == 0) {
        ctx->plan_count = 0;
        return 0;
    }

    int rc = scheduler_compute_plan(ctx->tasks,
                                    ctx->task_count,
                                    time(NULL),
                                    ctx->plan,
                                    ctx->plan_capacity);
    if (rc < 0) {
        return -1;
    }
    ctx->plan_count = (size_t)rc;
    return 0;
}

static int run_task_instance(erraid_context_t *ctx, schedule_entry_t *entry, int64_t when) {
    if (entry->task_index >= ctx->task_count) {
        errno = EINVAL;
        return -1;
    }
    task_t *task = &ctx->tasks[entry->task_index];
    if (!task->schedule.enabled || task->command_count == 0) {
        entry->next_epoch = -1;
        return 0;
    }

    executor_result_t result;
    memset(&result, 0, sizeof(result));

    int exec_rc = executor_run_task(task, &result);

    task_run_entry_t hist_entry;
    hist_entry.epoch = when;
    hist_entry.status = (exec_rc == 0) ? result.status : -1;
    hist_entry.stdout_len = result.stdout_len;
    hist_entry.stderr_len = result.stderr_len;

    const void *stdout_payload = result.stdout_buf;
    size_t stdout_len = result.stdout_len;
    const void *stderr_payload = result.stderr_buf;
    size_t stderr_len = result.stderr_len;

    if (exec_rc != 0) {
        stdout_payload = NULL;
        stdout_len = 0;
        stderr_payload = NULL;
        stderr_len = 0;
    }

    storage_append_history(&ctx->paths,
                           task->task_id,
                           &hist_entry,
                           stdout_payload,
                           stdout_len,
                           stderr_payload,
                           stderr_len);

    if (result.stdout_truncated || result.stderr_truncated) {
        const char *log_dir = ctx->logs_dir;
        (void)log_dir; /* rotation future : stub */
    }

    task->last_run_epoch = when;
    storage_write_task(&ctx->paths, task);

    executor_result_free(&result);

    entry->next_epoch = scheduler_next_occurrence(&task->schedule, when);
    return 0;
}

static int process_due_tasks(erraid_context_t *ctx) {
    while (!ctx->should_quit) {
        int64_t now;
        if (utils_now_epoch(&now) != 0) {
            return -1;
        }

        bool executed = false;
        for (size_t i = 0; i < ctx->plan_count; ++i) {
            schedule_entry_t *entry = &ctx->plan[i];
            if (entry->next_epoch >= 0 && entry->next_epoch <= now) {
                executed = true;
                run_task_instance(ctx, entry, now);
            }
        }

        if (!executed) {
            break;
        }
    }
    return 0;
}

static int process_requests(erraid_context_t *ctx) {
    while (1) {
        proto_message_t request;
        int rc = proto_read_message(ctx->request_fd, &request);
        if (rc != 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                errno = 0;
                return 0;
            }
            return -1;
        }

        erraid_handle_message(ctx, &request);

        struct pollfd pfd;
        memset(&pfd, 0, sizeof(pfd));
        pfd.fd = ctx->request_fd;
        pfd.events = POLLIN;
        int poll_rc = poll(&pfd, 1, 0);
        if (poll_rc <= 0) {
            break;
        }
        if (!(pfd.revents & POLLIN)) {
            break;
        }
    }
    return 0;
}

static int64_t next_deadline(const erraid_context_t *ctx) {
    int64_t best = -1;
    for (size_t i = 0; i < ctx->plan_count; ++i) {
        int64_t ts = ctx->plan[i].next_epoch;
        if (ts < 0) {
            continue;
        }
        if (best < 0 || ts < best) {
            best = ts;
        }
    }
    return best;
}

static int build_paths(erraid_context_t *ctx, const char *run_dir) {
    if (run_dir != NULL) {
        size_t len = strlen(run_dir);
        if (len == 0 || len >= sizeof(ctx->root_dir)) {
            errno = ENAMETOOLONG;
            return -1;
        }
        memcpy(ctx->root_dir, run_dir, len + 1);
    } else {
        const char *user = getenv("USER");
        if (user == NULL || user[0] == '\0') {
            user = "user";
        }
        int n = snprintf(ctx->root_dir,
                         sizeof(ctx->root_dir),
                         "%s/%s%s",
                         ERRAID_DEFAULT_RUNDIR_PREFIX,
                         user,
                         ERRAID_DEFAULT_RUNDIR_SUFFIX);
        if (n < 0 || (size_t)n >= sizeof(ctx->root_dir)) {
            errno = ENAMETOOLONG;
            return -1;
        }
    }

    if (utils_join_path(ctx->root_dir, ERRAID_TASKS_DIR_NAME, ctx->tasks_dir, sizeof(ctx->tasks_dir)) != 0) {
        return -1;
    }
    if (utils_join_path(ctx->root_dir, ERRAID_LOGS_DIR_NAME, ctx->logs_dir, sizeof(ctx->logs_dir)) != 0) {
        return -1;
    }
    if (utils_join_path(ctx->root_dir, ERRAID_STATE_DIR_NAME, ctx->state_dir, sizeof(ctx->state_dir)) != 0) {
        return -1;
    }
    if (utils_join_path(ctx->root_dir, ERRAID_PIPES_DIR_NAME, ctx->pipes_dir, sizeof(ctx->pipes_dir)) != 0) {
        return -1;
    }
    if (utils_join_path(ctx->pipes_dir, ERRAID_PIPE_REQUEST_NAME, ctx->request_pipe_path, sizeof(ctx->request_pipe_path)) != 0) {
        return -1;
    }
    if (utils_join_path(ctx->pipes_dir, ERRAID_PIPE_REPLY_NAME, ctx->reply_pipe_path, sizeof(ctx->reply_pipe_path)) != 0) {
        return -1;
    }

    ctx->paths.root_dir = ctx->root_dir;
    ctx->paths.tasks_dir = ctx->tasks_dir;
    ctx->paths.logs_dir = ctx->logs_dir;
    ctx->paths.state_dir = ctx->state_dir;
    ctx->paths.pipes_dir = ctx->pipes_dir;
    return 0;
}

int erraid_init(erraid_context_t *ctx, const char *run_dir) {
    if (ctx == NULL) {
        errno = EINVAL;
        return -1;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->request_fd = -1;
    ctx->reply_fd = -1;
    ctx->request_dummy_fd = -1;
    ctx->wake_pipe[0] = -1;
    ctx->wake_pipe[1] = -1;

    if (build_paths(ctx, run_dir) != 0) {
        return -1;
    }

    if (storage_init_directories(&ctx->paths) != 0) {
        return -1;
    }

    if (mkfifo(ctx->request_pipe_path, 0600) != 0) {
        if (errno != EEXIST) {
            return -1;
        }
    }
    if (mkfifo(ctx->reply_pipe_path, 0600) != 0) {
        if (errno != EEXIST) {
            return -1;
        }
    }

    if (pipe(ctx->wake_pipe) != 0) {
        return -1;
    }
    if (set_fd_flags(ctx->wake_pipe[0], O_NONBLOCK) != 0) {
        return -1;
    }
    if (set_fd_flags(ctx->wake_pipe[1], O_NONBLOCK) != 0) {
        return -1;
    }

    ctx->request_fd = open(ctx->request_pipe_path, O_RDONLY | O_NONBLOCK);
    if (ctx->request_fd < 0) {
        return -1;
    }
    ctx->request_dummy_fd = open(ctx->request_pipe_path, O_WRONLY | O_NONBLOCK);
    if (ctx->request_dummy_fd < 0) {
        return -1;
    }
    if (clear_fd_flags(ctx->request_fd, O_NONBLOCK) != 0) {
        return -1;
    }

    ctx->reply_fd = open(ctx->reply_pipe_path, O_RDWR | O_NONBLOCK);
    if (ctx->reply_fd < 0) {
        return -1;
    }

    if (erraid_reload_tasks(ctx) != 0) {
        return -1;
    }

    return 0;
}

void erraid_shutdown(erraid_context_t *ctx) {
    if (ctx == NULL) {
        return;
    }

    if (ctx->request_fd >= 0) {
        close(ctx->request_fd);
        ctx->request_fd = -1;
    }
    if (ctx->request_dummy_fd >= 0) {
        close(ctx->request_dummy_fd);
        ctx->request_dummy_fd = -1;
    }
    if (ctx->reply_fd >= 0) {
        close(ctx->reply_fd);
        ctx->reply_fd = -1;
    }
    if (ctx->wake_pipe[0] >= 0) {
        close(ctx->wake_pipe[0]);
        ctx->wake_pipe[0] = -1;
    }
    if (ctx->wake_pipe[1] >= 0) {
        close(ctx->wake_pipe[1]);
        ctx->wake_pipe[1] = -1;
    }

    storage_free_tasks(ctx->tasks, ctx->task_count);
    ctx->tasks = NULL;
    ctx->task_count = 0;

    free(ctx->plan);
    ctx->plan = NULL;
    ctx->plan_capacity = 0;
    ctx->plan_count = 0;
}

int erraid_reload_tasks(erraid_context_t *ctx) {
    if (ctx == NULL) {
        errno = EINVAL;
        return -1;
    }

    storage_free_tasks(ctx->tasks, ctx->task_count);
    ctx->tasks = NULL;
    ctx->task_count = 0;

    task_t *tasks = NULL;
    size_t count = 0;
    if (storage_load_tasks(&ctx->paths, &tasks, &count) != 0) {
        return -1;
    }

    ctx->tasks = tasks;
    ctx->task_count = count;

    if (rebuild_plan(ctx) != 0) {
        return -1;
    }

    return 0;
}

int erraid_handle_message(erraid_context_t *ctx, const proto_message_t *request) {
    if (ctx == NULL || request == NULL) {
        errno = EINVAL;
        return -1;
    }

    message_type_t type = (message_type_t)request->header.type;
    const char *payload = request->payload;

    switch (type) {
        case MSG_PING:
            return send_status_ok(ctx, MSG_PONG);
        case MSG_REQ_LIST_TASKS:
            if (respond_list_tasks(ctx) != 0) {
                return send_error_response(ctx, "LIST_FAILED", "Impossible de lister les tâches");
            }
            return 0;
        case MSG_REQ_CREATE_SIMPLE:
        case MSG_REQ_CREATE_SEQUENCE:
        case MSG_REQ_CREATE_ABSTRACT:
            return handle_create_task(ctx, type, payload);
        case MSG_REQ_REMOVE:
            return handle_remove_task(ctx, payload);
        case MSG_REQ_LIST_HISTORY: {
            uint64_t task_id = 0;
            if (json_extract_uint64(payload, "task_id", &task_id) != 0) {
                return send_error_response(ctx, "INVALID_REQUEST", "task_id manquant");
            }
            if (respond_history(ctx, task_id) != 0) {
                return send_error_response(ctx, "HISTORY_FAILED", "Lecture de l'historique impossible");
            }
            return 0;
        }
        case MSG_REQ_GET_STDOUT: {
            uint64_t task_id = 0;
            if (json_extract_uint64(payload, "task_id", &task_id) != 0) {
                return send_error_response(ctx, "INVALID_REQUEST", "task_id manquant");
            }
            if (respond_stdio(ctx, task_id, true) != 0) {
                return send_error_response(ctx, "STDOUT_FAILED", "Impossible de charger stdout");
            }
            return 0;
        }
        case MSG_REQ_GET_STDERR: {
            uint64_t task_id = 0;
            if (json_extract_uint64(payload, "task_id", &task_id) != 0) {
                return send_error_response(ctx, "INVALID_REQUEST", "task_id manquant");
            }
            if (respond_stdio(ctx, task_id, false) != 0) {
                return send_error_response(ctx, "STDERR_FAILED", "Impossible de charger stderr");
            }
            return 0;
        }
        case MSG_REQ_SHUTDOWN:
            ctx->should_quit = true;
            send_status_ok(ctx, MSG_RSP_SHUTDOWN);
            return 0;
        default:
            return send_error_response(ctx, "UNKNOWN_REQUEST", "Type de message inconnu");
    }
}

int erraid_schedule_loop(erraid_context_t *ctx) {
    if (ctx == NULL) {
        errno = EINVAL;
        return -1;
    }

    struct pollfd fds[2];

    while (!ctx->should_quit) {
        int64_t now;
        if (utils_now_epoch(&now) != 0) {
            return -1;
        }

        int64_t deadline = next_deadline(ctx);
        int timeout_ms = -1;
        if (deadline >= 0) {
            int64_t diff = deadline - now;
            if (diff < 0) {
                diff = 0;
            }
            if (diff > (int64_t)INT_MAX / 1000) {
                diff = (int64_t)INT_MAX / 1000;
            }
            timeout_ms = (int)(diff * 1000);
        }

        memset(fds, 0, sizeof(fds));
        fds[0].fd = ctx->request_fd;
        fds[0].events = POLLIN;
        fds[1].fd = ctx->wake_pipe[0];
        fds[1].events = POLLIN;

        int rc = poll(fds, ARRAY_SIZE(fds), timeout_ms);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }

        if (rc > 0) {
            if (fds[1].revents & POLLIN) {
                drain_fd(ctx->wake_pipe[0]);
            }
            if (fds[0].revents & POLLIN) {
                process_requests(ctx);
            }
        }

        if (ctx->should_quit) {
            break;
        }

        process_due_tasks(ctx);
    }

    return 0;
}

int erraid_run(erraid_context_t *ctx) {
    if (ctx == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (notifier_install(ctx) != 0) {
        return -1;
    }

    int rc = erraid_schedule_loop(ctx);

    notifier_uninstall();

    return rc;
}
