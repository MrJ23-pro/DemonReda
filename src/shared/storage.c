#include "storage.h"

#include "utils.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static int ensure_directory(const char *path, mode_t mode) {
    struct stat st;
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            return 0;
        }
        errno = ENOTDIR;
        return -1;
    }

    if (errno != ENOENT) {
        return -1;
    }

    if (mkdir(path, mode) != 0) {
        return -1;
    }
    return 0;
}

typedef struct {
    char *name;
    int64_t epoch;
    unsigned counter;
} snapshot_entry_t;

static int build_snapshot_name(char *buffer, size_t size, int64_t epoch, unsigned counter, const char *ext) {
    if (counter == 0) {
        return snprintf(buffer, size, "snapshot-%lld.%s", (long long)epoch, ext) >= (int)size ? -1 : 0;
    }
    return snprintf(buffer, size, "snapshot-%lld-%u.%s", (long long)epoch, counter, ext) >= (int)size ? -1 : 0;
}

static int parse_snapshot_filename(const char *name, const char *ext, int64_t *epoch_out, unsigned *counter_out) {
    if (strncmp(name, "snapshot-", 9) != 0) {
        return -1;
    }
    const char *dot = strrchr(name, '.');
    if (dot == NULL || strcmp(dot + 1, ext) != 0) {
        return -1;
    }

    const char *cursor = name + 9;
    char *endptr = NULL;
    errno = 0;
    long long epoch = strtoll(cursor, &endptr, 10);
    if (errno != 0 || endptr == cursor) {
        return -1;
    }

    unsigned counter = 0;
    if (*endptr == '-') {
        char *counter_end = NULL;
        errno = 0;
        unsigned long tmp = strtoul(endptr + 1, &counter_end, 10);
        if (errno != 0 || counter_end == endptr + 1) {
            return -1;
        }
        counter = (unsigned)tmp;
        endptr = counter_end;
    }

    if (endptr != dot) {
        return -1;
    }

    if (epoch_out != NULL) {
        *epoch_out = (int64_t)epoch;
    }
    if (counter_out != NULL) {
        *counter_out = counter;
    }
    return 0;
}

static int snapshot_compare_desc(const void *a, const void *b) {
    const snapshot_entry_t *sa = (const snapshot_entry_t *)a;
    const snapshot_entry_t *sb = (const snapshot_entry_t *)b;
    if (sa->epoch != sb->epoch) {
        return (sb->epoch > sa->epoch) ? 1 : -1;
    }
    if (sa->counter != sb->counter) {
        return (int)sb->counter - (int)sa->counter;
    }
    return strcmp(sa->name, sb->name);
}

static int prune_snapshots(const char *log_dir, const char *ext) {
    DIR *dir = opendir(log_dir);
    if (dir == NULL) {
        if (errno == ENOENT) {
            errno = 0;
            return 0;
        }
        return -1;
    }

    snapshot_entry_t *entries = NULL;
    size_t count = 0;
    size_t capacity = 0;

    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') {
            continue;
        }
        int64_t epoch = 0;
        unsigned counter = 0;
        if (parse_snapshot_filename(de->d_name, ext, &epoch, &counter) != 0) {
            continue;
        }

        if (count >= capacity) {
            size_t new_cap = (capacity == 0) ? 8 : (capacity * 2);
            snapshot_entry_t *tmp = realloc(entries, new_cap * sizeof(snapshot_entry_t));
            if (tmp == NULL) {
                closedir(dir);
                for (size_t i = 0; i < count; ++i) {
                    free(entries[i].name);
                }
                free(entries);
                errno = ENOMEM;
                return -1;
            }
            capacity = new_cap;
            entries = tmp;
        }

        entries[count].name = strdup(de->d_name);
        if (entries[count].name == NULL) {
            closedir(dir);
            for (size_t i = 0; i < count; ++i) {
                free(entries[i].name);
            }
            free(entries);
            errno = ENOMEM;
            return -1;
        }
        entries[count].epoch = epoch;
        entries[count].counter = counter;
        ++count;
    }
    closedir(dir);

    if (count <= ERRAID_STDIO_SNAPSHOT_COUNT) {
        for (size_t i = 0; i < count; ++i) {
            free(entries[i].name);
        }
        free(entries);
        return 0;
    }

    qsort(entries, count, sizeof(snapshot_entry_t), snapshot_compare_desc);

    for (size_t i = ERRAID_STDIO_SNAPSHOT_COUNT; i < count; ++i) {
        char path[PATH_MAX];
        if (utils_join_path(log_dir, entries[i].name, path, sizeof(path)) == 0) {
            unlink(path);
        }
        free(entries[i].name);
    }

    for (size_t i = 0; i < ERRAID_STDIO_SNAPSHOT_COUNT && i < count; ++i) {
        free(entries[i].name);
    }
    free(entries);
    return 0;
}

static int rotate_stdio_snapshot(const char *log_dir,
                                 const char *base_filename,
                                 const char *ext,
                                 int64_t epoch) {
    char base_path[PATH_MAX];
    if (utils_join_path(log_dir, base_filename, base_path, sizeof(base_path)) != 0) {
        return -1;
    }

    struct stat st;
    if (stat(base_path, &st) != 0) {
        if (errno == ENOENT) {
            errno = 0;
            return 0;
        }
        return -1;
    }
    if (st.st_size == 0) {
        return 0;
    }

    char snapshot_name[64];
    char snapshot_path[PATH_MAX];
    unsigned counter = 0;
    while (counter < 1000) {
        if (build_snapshot_name(snapshot_name, sizeof(snapshot_name), epoch, counter, ext) != 0) {
            return -1;
        }
        if (utils_join_path(log_dir, snapshot_name, snapshot_path, sizeof(snapshot_path)) != 0) {
            return -1;
        }
        if (access(snapshot_path, F_OK) != 0) {
            if (rename(base_path, snapshot_path) != 0) {
                return -1;
            }
            break;
        }
        ++counter;
    }

    if (counter >= 1000) {
        /* fallback: delete oldest if rename impossible */
        unlink(base_path);
    }

    if (prune_snapshots(log_dir, ext) != 0) {
        return -1;
    }
    return 0;
}

static int read_file_alloc(const char *path, char **buffer_out, size_t *length_out) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        return -1;
    }
    size_t size = (size_t)st.st_size;
    char *data = malloc(size + 1);
    if (data == NULL) {
        close(fd);
        errno = ENOMEM;
        return -1;
    }
    size_t total = 0;
    while (total < size) {
        ssize_t n = read(fd, data + total, size - total);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            free(data);
            close(fd);
            return -1;
        }
        if (n == 0) {
            break;
        }
        total += (size_t)n;
    }
    close(fd);
    data[total] = '\0';
    *buffer_out = data;
    if (length_out) {
        *length_out = total;
    }
    return 0;
}

static int write_all_fd(int fd, const char *buffer, size_t length) {
    size_t total = 0;
    while (total < length) {
        ssize_t n = write(fd, buffer + total, length - total);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        total += (size_t)n;
    }
    return 0;
}

static int write_line_fd(int fd, const char *line) {
    size_t len = strlen(line);
    return write_all_fd(fd, line, len);
}

static bool has_suffix(const char *name, const char *suffix) {
    size_t len_name = strlen(name);
    size_t len_suffix = strlen(suffix);
    if (len_name < len_suffix) {
        return false;
    }
    return memcmp(name + len_name - len_suffix, suffix, len_suffix) == 0;
}

static char *trim_whitespace(char *str) {
    while (*str && (unsigned char)(*str) <= ' ') {
        ++str;
    }
    char *end = str + strlen(str);
    while (end > str && (unsigned char)end[-1] <= ' ') {
        *--end = '\0';
    }
    return str;
}

static int parse_uint64(const char *line, uint64_t *value_out) {
    char *endptr = NULL;
    errno = 0;
    unsigned long long value = strtoull(line, &endptr, 10);
    if (errno != 0 || endptr == line || *trim_whitespace(endptr) != '\0') {
        if (errno == 0) {
            errno = EINVAL;
        }
        return -1;
    }
    *value_out = (uint64_t)value;
    return 0;
}

static int parse_int64(const char *line, int64_t *value_out) {
    char *endptr = NULL;
    errno = 0;
    long long value = strtoll(line, &endptr, 10);
    if (errno != 0 || endptr == line || *trim_whitespace(endptr) != '\0') {
        if (errno == 0) {
            errno = EINVAL;
        }
        return -1;
    }
    *value_out = (int64_t)value;
    return 0;
}

static int parse_hex64(const char *line, uint64_t *value_out) {
    char *endptr = NULL;
    errno = 0;
    unsigned long long value = strtoull(line, &endptr, 16);
    if (errno != 0 || endptr == line || *trim_whitespace(endptr) != '\0') {
        if (errno == 0) {
            errno = EINVAL;
        }
        return -1;
    }
    *value_out = (uint64_t)value;
    return 0;
}

static int parse_hex32(const char *line, uint32_t *value_out) {
    uint64_t tmp;
    if (parse_hex64(line, &tmp) != 0) {
        return -1;
    }
    *value_out = (uint32_t)tmp;
    return 0;
}

static int parse_hex8(const char *line, uint8_t *value_out) {
    uint64_t tmp;
    if (parse_hex64(line, &tmp) != 0) {
        return -1;
    }
    *value_out = (uint8_t)tmp;
    return 0;
}

static int append_char(char **buffer, size_t *capacity, size_t *length, char ch) {
    if (*length + 1 >= *capacity) {
        size_t new_cap = (*capacity == 0) ? 32 : (*capacity * 2);
        char *tmp = realloc(*buffer, new_cap);
        if (tmp == NULL) {
            errno = ENOMEM;
            return -1;
        }
        *buffer = tmp;
        *capacity = new_cap;
    }
    (*buffer)[(*length)++] = ch;
    (*buffer)[*length] = '\0';
    return 0;
}

static int parse_json_string(const char **cursor, char **out) {
    size_t capacity = 32;
    size_t length = 0;
    char *buffer = malloc(capacity);
    if (buffer == NULL) {
        errno = ENOMEM;
        return -1;
    }
    buffer[0] = '\0';
    const char *p = *cursor;
    while (*p) {
        char ch = *p++;
        if (ch == '"') {
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
                case 'n': ch = '\n'; break;
                case 'r': ch = '\r'; break;
                case 't': ch = '\t'; break;
                default:
                    free(buffer);
                    errno = EINVAL;
                    return -1;
            }
        }
        if (append_char(&buffer, &capacity, &length, ch) != 0) {
            free(buffer);
            return -1;
        }
    }
    free(buffer);
    errno = EINVAL;
    return -1;
}

static int parse_command_line(const char *line, command_t *command) {
    const char *p = line;
    while (*p && isspace((unsigned char)*p)) {
        ++p;
    }
    if (*p != '[') {
        errno = EINVAL;
        return -1;
    }
    ++p;
    size_t argc = 0;
    size_t capacity = 4;
    char **argv = calloc(capacity + 1, sizeof(char *));
    if (argv == NULL) {
        errno = ENOMEM;
        return -1;
    }
    char *element = NULL;
    while (1) {
        while (*p && isspace((unsigned char)*p)) {
            ++p;
        }
        if (*p == ']') {
            ++p;
            break;
        }
        if (*p != '"') {
            goto error;
        }
        ++p;
        if (parse_json_string(&p, &element) != 0) {
            goto error;
        }
        while (*p && isspace((unsigned char)*p)) {
            ++p;
        }
        if (*p == ',') {
            ++p;
        } else if (*p == ']') {
            ++p;
        } else {
            goto error_element;
        }
        if (argc >= capacity) {
            size_t new_cap = capacity * 2;
            char **tmp = realloc(argv, (new_cap + 1) * sizeof(char *));
            if (tmp == NULL) {
                goto error_element;
            }
            capacity = new_cap;
            argv = tmp;
        }
        argv[argc++] = element;
        element = NULL;
    }
    while (*p && isspace((unsigned char)*p)) {
        ++p;
    }
    if (*p != '\0') {
        goto error;
    }
    argv[argc] = NULL;
    command->argv = argv;
    command->argc = argc;
    return 0;

error_element:
    if (element != NULL) {
        free(element);
    }
error:
    if (argv != NULL) {
        for (size_t i = 0; i < argc; ++i) {
            free(argv[i]);
        }
        free(argv);
    }
    errno = EINVAL;
    return -1;
}

static void free_command(command_t *command) {
    if (command->argv != NULL) {
        for (size_t i = 0; i < command->argc; ++i) {
            free(command->argv[i]);
        }
        free(command->argv);
    }
    command->argv = NULL;
    command->argc = 0;
}

static void free_task(task_t *task) {
    if (task->commands != NULL) {
        for (size_t i = 0; i < task->command_count; ++i) {
            free_command(&task->commands[i]);
        }
        free(task->commands);
        task->commands = NULL;
    }
}

static int parse_task_file(const char *buffer, task_t *task) {
    char *content = strdup(buffer);
    if (content == NULL) {
        errno = ENOMEM;
        return -1;
    }
    char *lines[128];
    size_t line_count = 0;
    char *line = content;
    while (line != NULL && *line != '\0' && line_count < 128) {
        char *newline = strchr(line, '\n');
        if (newline != NULL) {
            *newline = '\0';
        }
        lines[line_count++] = trim_whitespace(line);
        if (newline == NULL) {
            break;
        }
        line = newline + 1;
    }
    if (line_count < 8) {
        free(content);
        errno = EINVAL;
        return -1;
    }

    uint64_t task_id = 0;
    if (parse_uint64(lines[0], &task_id) != 0) {
        free(content);
        return -1;
    }
    task->task_id = task_id;

    if (strcmp(lines[1], "SIMPLE") == 0) {
        task->type = TASK_TYPE_SIMPLE;
    } else if (strcmp(lines[1], "SEQUENCE") == 0) {
        task->type = TASK_TYPE_SEQUENCE;
    } else if (strcmp(lines[1], "ABSTRACT") == 0) {
        task->type = TASK_TYPE_ABSTRACT;
    } else {
        free(content);
        errno = EINVAL;
        return -1;
    }

    uint64_t command_count = 0;
    if (parse_uint64(lines[2], &command_count) != 0) {
        free(content);
        return -1;
    }
    task->command_count = (size_t)command_count;
    if (command_count > 0) {
        task->commands = calloc(command_count, sizeof(command_t));
        if (task->commands == NULL) {
            free(content);
            errno = ENOMEM;
            return -1;
        }
    } else {
        task->commands = NULL;
    }

    size_t index = 3;
    for (size_t i = 0; i < command_count; ++i) {
        if (index >= line_count) {
            free(content);
            errno = EINVAL;
            free_task(task);
            return -1;
        }
        if (parse_command_line(lines[index++], &task->commands[i]) != 0) {
            free(content);
            free_task(task);
            return -1;
        }
    }

    if (index + 5 > line_count) {
        free(content);
        free_task(task);
        errno = EINVAL;
        return -1;
    }

    if (parse_hex64(lines[index++], &task->schedule.minute_mask) != 0) {
        free(content);
        free_task(task);
        return -1;
    }
    if (parse_hex32(lines[index++], &task->schedule.hour_mask) != 0) {
        free(content);
        free_task(task);
        return -1;
    }
    if (parse_hex8(lines[index++], &task->schedule.weekday_mask) != 0) {
        free(content);
        free_task(task);
        return -1;
    }

    uint64_t flags = 0;
    if (parse_uint64(lines[index++], &flags) != 0) {
        free(content);
        free_task(task);
        return -1;
    }
    (void)flags;

    if (parse_int64(lines[index++], &task->last_run_epoch) != 0) {
        free(content);
        free_task(task);
        return -1;
    }

    task->schedule.enabled = (task->type != TASK_TYPE_ABSTRACT);

    free(content);
    return 0;
}

static int write_command_line_fd(int fd, const command_t *command) {
    char buffer[4096];
    size_t pos = 0;
    buffer[pos++] = '[';
    for (size_t i = 0; i < command->argc; ++i) {
        if (i > 0) {
            buffer[pos++] = ',';
        }
        buffer[pos++] = '"';
        const char *arg = command->argv[i];
        for (; *arg; ++arg) {
            char ch = *arg;
            if (ch == '"' || ch == '\\') {
                if (pos + 2 >= sizeof(buffer)) {
                    errno = ENOSPC;
                    return -1;
                }
                buffer[pos++] = '\\';
                buffer[pos++] = ch;
            } else if (ch == '\n') {
                if (pos + 2 >= sizeof(buffer)) {
                    errno = ENOSPC;
                    return -1;
                }
                buffer[pos++] = '\\';
                buffer[pos++] = 'n';
            } else if (ch == '\r') {
                if (pos + 2 >= sizeof(buffer)) {
                    errno = ENOSPC;
                    return -1;
                }
                buffer[pos++] = '\\';
                buffer[pos++] = 'r';
            } else if (ch == '\t') {
                if (pos + 2 >= sizeof(buffer)) {
                    errno = ENOSPC;
                    return -1;
                }
                buffer[pos++] = '\\';
                buffer[pos++] = 't';
            } else {
                if (pos + 1 >= sizeof(buffer)) {
                    errno = ENOSPC;
                    return -1;
                }
                buffer[pos++] = ch;
            }
        }
        if (pos + 2 >= sizeof(buffer)) {
            errno = ENOSPC;
            return -1;
        }
        buffer[pos++] = '"';
    }
    if (pos + 2 >= sizeof(buffer)) {
        errno = ENOSPC;
        return -1;
    }
    buffer[pos++] = ']';
    buffer[pos++] = '\n';
    return write_all_fd(fd, buffer, pos);
}

int storage_init_directories(const storage_paths_t *paths) {
    if (paths == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (ensure_directory(paths->root_dir, 0700) != 0) {
        return -1;
    }
    if (ensure_directory(paths->tasks_dir, 0700) != 0) {
        return -1;
    }
    if (ensure_directory(paths->logs_dir, 0700) != 0) {
        return -1;
    }
    if (ensure_directory(paths->state_dir, 0700) != 0) {
        return -1;
    }
    if (ensure_directory(paths->pipes_dir, 0700) != 0) {
        return -1;
    }
    return 0;
}

int storage_load_tasks(const storage_paths_t *paths, task_t **tasks_out, size_t *count_out) {
    if (paths == NULL || tasks_out == NULL || count_out == NULL) {
        errno = EINVAL;
        return -1;
    }

    DIR *dir = opendir(paths->tasks_dir);
    if (dir == NULL) {
        return -1;
    }

    task_t *tasks = NULL;
    size_t capacity = 0;
    size_t count = 0;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (strcmp(entry->d_name, "next_id") == 0) {
            continue;
        }
        if (!has_suffix(entry->d_name, ".task")) {
            continue;
        }

        if (count >= capacity) {
            size_t new_cap = (capacity == 0) ? 4 : (capacity * 2);
            task_t *tmp = realloc(tasks, new_cap * sizeof(task_t));
            if (tmp == NULL) {
                closedir(dir);
                storage_free_tasks(tasks, count);
                errno = ENOMEM;
                return -1;
            }
            capacity = new_cap;
            tasks = tmp;
        }

        memset(&tasks[count], 0, sizeof(task_t));

        char path[PATH_MAX];
        if (utils_join_path(paths->tasks_dir, entry->d_name, path, sizeof(path)) != 0) {
            closedir(dir);
            storage_free_tasks(tasks, count);
            return -1;
        }

        char *file_buffer = NULL;
        if (read_file_alloc(path, &file_buffer, NULL) != 0) {
            closedir(dir);
            storage_free_tasks(tasks, count);
            return -1;
        }

        if (parse_task_file(file_buffer, &tasks[count]) != 0) {
            free(file_buffer);
            closedir(dir);
            storage_free_tasks(tasks, count);
            return -1;
        }
        free(file_buffer);
        ++count;
    }
    closedir(dir);

    *tasks_out = tasks;
    *count_out = count;
    return 0;
}

static int write_task_file(const storage_paths_t *paths, const task_t *task, const char *final_path) {
    (void)paths;
    char tmp_path[PATH_MAX];
    if (utils_join_path(final_path, ".tmp", tmp_path, sizeof(tmp_path)) != 0) {
        return -1;
    }

    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        return -1;
    }

    char line[128];
    int n = snprintf(line, sizeof(line), "%llu\n", (unsigned long long)task->task_id);
    if (n < 0 || (size_t)n >= sizeof(line) || write_all_fd(fd, line, (size_t)n) != 0) {
        close(fd);
        unlink(tmp_path);
        return -1;
    }

    const char *type_str = (task->type == TASK_TYPE_SIMPLE)
                               ? "SIMPLE"
                               : (task->type == TASK_TYPE_SEQUENCE) ? "SEQUENCE" : "ABSTRACT";
    if (write_line_fd(fd, type_str) != 0 || write_line_fd(fd, "\n") != 0) {
        close(fd);
        unlink(tmp_path);
        return -1;
    }

    n = snprintf(line, sizeof(line), "%zu\n", task->command_count);
    if (n < 0 || (size_t)n >= sizeof(line) || write_all_fd(fd, line, (size_t)n) != 0) {
        close(fd);
        unlink(tmp_path);
        return -1;
    }

    for (size_t i = 0; i < task->command_count; ++i) {
        if (write_command_line_fd(fd, &task->commands[i]) != 0) {
            close(fd);
            unlink(tmp_path);
            return -1;
        }
    }

    n = snprintf(line, sizeof(line), "%015llX\n", (unsigned long long)task->schedule.minute_mask);
    if (n < 0 || (size_t)n >= sizeof(line) || write_all_fd(fd, line, (size_t)n) != 0) {
        close(fd);
        unlink(tmp_path);
        return -1;
    }
    n = snprintf(line, sizeof(line), "%06X\n", task->schedule.hour_mask & 0xFFFFFFu);
    if (n < 0 || (size_t)n >= sizeof(line) || write_all_fd(fd, line, (size_t)n) != 0) {
        close(fd);
        unlink(tmp_path);
        return -1;
    }
    n = snprintf(line, sizeof(line), "%02X\n", task->schedule.weekday_mask & 0x7Fu);
    if (n < 0 || (size_t)n >= sizeof(line) || write_all_fd(fd, line, (size_t)n) != 0) {
        close(fd);
        unlink(tmp_path);
        return -1;
    }
    if (write_line_fd(fd, "0\n") != 0) {
        close(fd);
        unlink(tmp_path);
        return -1;
    }
    n = snprintf(line, sizeof(line), "%lld\n", (long long)task->last_run_epoch);
    if (n < 0 || (size_t)n >= sizeof(line) || write_all_fd(fd, line, (size_t)n) != 0) {
        close(fd);
        unlink(tmp_path);
        return -1;
    }

    if (fsync(fd) != 0) {
        close(fd);
        unlink(tmp_path);
        return -1;
    }
    if (close(fd) != 0) {
        unlink(tmp_path);
        return -1;
    }
    if (rename(tmp_path, final_path) != 0) {
        unlink(tmp_path);
        return -1;
    }
    return 0;
}

int storage_write_task(const storage_paths_t *paths, const task_t *task) {
    if (paths == NULL || task == NULL) {
        errno = EINVAL;
        return -1;
    }
    char filename[64];
    int n = snprintf(filename, sizeof(filename), "%llu.task", (unsigned long long)task->task_id);
    if (n < 0 || (size_t)n >= sizeof(filename)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    char final_path[PATH_MAX];
    if (utils_join_path(paths->tasks_dir, filename, final_path, sizeof(final_path)) != 0) {
        return -1;
    }
    return write_task_file(paths, task, final_path);
}

int storage_remove_task(const storage_paths_t *paths, uint64_t task_id) {
    if (paths == NULL) {
        errno = EINVAL;
        return -1;
    }
    char filename[64];
    int n = snprintf(filename, sizeof(filename), "%llu.task", (unsigned long long)task_id);
    if (n < 0 || (size_t)n >= sizeof(filename)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    char task_path[PATH_MAX];
    if (utils_join_path(paths->tasks_dir, filename, task_path, sizeof(task_path)) != 0) {
        return -1;
    }
    if (unlink(task_path) != 0) {
        return -1;
    }

    char idbuf[32];
    n = snprintf(idbuf, sizeof(idbuf), "%llu", (unsigned long long)task_id);
    if (n < 0 || (size_t)n >= sizeof(idbuf)) {
        return -1;
    }

    char log_dir[PATH_MAX];
    if (utils_join_path(paths->logs_dir, idbuf, log_dir, sizeof(log_dir)) != 0) {
        return -1;
    }
    char history_path[PATH_MAX];
    if (utils_join_path(log_dir, "history.log", history_path, sizeof(history_path)) != 0) {
        return -1;
    }
    char stdout_path[PATH_MAX];
    if (utils_join_path(log_dir, "last.stdout", stdout_path, sizeof(stdout_path)) != 0) {
        return -1;
    }
    char stderr_path[PATH_MAX];
    if (utils_join_path(log_dir, "last.stderr", stderr_path, sizeof(stderr_path)) != 0) {
        return -1;
    }

    unlink(history_path);
    unlink(stdout_path);
    unlink(stderr_path);
    rmdir(log_dir);

    return 0;
}

int storage_append_history(const storage_paths_t *paths,
                           uint64_t task_id,
                           const task_run_entry_t *entry,
                           const void *stdout_buf,
                           size_t stdout_len,
                           const void *stderr_buf,
                           size_t stderr_len) {
    if (paths == NULL || entry == NULL) {
        errno = EINVAL;
        return -1;
    }

    char idbuf[32];
    int n = snprintf(idbuf, sizeof(idbuf), "%llu", (unsigned long long)task_id);
    if (n < 0 || (size_t)n >= sizeof(idbuf)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    char log_dir[PATH_MAX];
    if (utils_join_path(paths->logs_dir, idbuf, log_dir, sizeof(log_dir)) != 0) {
        return -1;
    }
    if (ensure_directory(log_dir, 0700) != 0) {
        return -1;
    }

    if (rotate_stdio_snapshot(log_dir, "last.stdout", "stdout", entry->epoch) != 0) {
        return -1;
    }
    if (rotate_stdio_snapshot(log_dir, "last.stderr", "stderr", entry->epoch) != 0) {
        return -1;
    }

    char stdout_path[PATH_MAX];
    if (utils_join_path(log_dir, "last.stdout", stdout_path, sizeof(stdout_path)) != 0) {
        return -1;
    }
    char stderr_path[PATH_MAX];
    if (utils_join_path(log_dir, "last.stderr", stderr_path, sizeof(stderr_path)) != 0) {
        return -1;
    }

    int fd_out = open(stdout_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd_out < 0) {
        return -1;
    }
    if (stdout_len > 0 && stdout_buf != NULL) {
        if (write_all_fd(fd_out, (const char *)stdout_buf, stdout_len) != 0) {
            close(fd_out);
            return -1;
        }
    }
    if (fsync(fd_out) != 0) {
        close(fd_out);
        return -1;
    }
    close(fd_out);

    int fd_err = open(stderr_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd_err < 0) {
        return -1;
    }
    if (stderr_len > 0 && stderr_buf != NULL) {
        if (write_all_fd(fd_err, (const char *)stderr_buf, stderr_len) != 0) {
            close(fd_err);
            return -1;
        }
    }
    if (fsync(fd_err) != 0) {
        close(fd_err);
        return -1;
    }
    close(fd_err);

    char history_path[PATH_MAX];
    if (utils_join_path(log_dir, "history.log", history_path, sizeof(history_path)) != 0) {
        return -1;
    }

    int fd_hist = open(history_path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd_hist < 0) {
        return -1;
    }
    char line[128];
    n = snprintf(line,
                 sizeof(line),
                 "%lld %d %zu %zu\n",
                 (long long)entry->epoch,
                 (int)entry->status,
                 stdout_len,
                 stderr_len);
    if (n < 0 || (size_t)n >= sizeof(line) || write_all_fd(fd_hist, line, (size_t)n) != 0) {
        close(fd_hist);
        return -1;
    }
    if (fsync(fd_hist) != 0) {
        close(fd_hist);
        return -1;
    }
    close(fd_hist);

    return 0;
}

static int parse_history_entry(const char *line, task_run_entry_t *entry) {
    char *dup = strdup(line);
    if (dup == NULL) {
        errno = ENOMEM;
        return -1;
    }
    char *token = strtok(dup, " ");
    if (token == NULL || parse_int64(token, &entry->epoch) != 0) {
        free(dup);
        return -1;
    }
    token = strtok(NULL, " ");
    if (token == NULL) {
        free(dup);
        errno = EINVAL;
        return -1;
    }
    char *endptr = NULL;
    long status = strtol(token, &endptr, 10);
    if (endptr == token || *endptr != '\0') {
        free(dup);
        errno = EINVAL;
        return -1;
    }
    entry->status = (int32_t)status;

    token = strtok(NULL, " ");
    if (token == NULL) {
        free(dup);
        errno = EINVAL;
        return -1;
    }
    unsigned long long stdout_len = strtoull(token, &endptr, 10);
    if (endptr == token || *endptr != '\0') {
        free(dup);
        errno = EINVAL;
        return -1;
    }
    entry->stdout_len = (size_t)stdout_len;

    token = strtok(NULL, " ");
    if (token == NULL) {
        free(dup);
        errno = EINVAL;
        return -1;
    }
    unsigned long long stderr_len = strtoull(token, &endptr, 10);
    if (endptr == token || *endptr != '\0') {
        free(dup);
        errno = EINVAL;
        return -1;
    }
    entry->stderr_len = (size_t)stderr_len;

    free(dup);
    return 0;
}

int storage_load_history(const storage_paths_t *paths,
                         uint64_t task_id,
                         task_run_entry_t **entries_out,
                         size_t *entry_count_out) {
    if (paths == NULL || entries_out == NULL || entry_count_out == NULL) {
        errno = EINVAL;
        return -1;
    }

    char idbuf[32];
    int n = snprintf(idbuf, sizeof(idbuf), "%llu", (unsigned long long)task_id);
    if (n < 0 || (size_t)n >= sizeof(idbuf)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    char history_path[PATH_MAX];
    if (utils_join_path3(paths->logs_dir, idbuf, "history.log", history_path, sizeof(history_path)) != 0) {
        return -1;
    }

    char *buffer = NULL;
    if (read_file_alloc(history_path, &buffer, NULL) != 0) {
        if (errno == ENOENT) {
            *entries_out = NULL;
            *entry_count_out = 0;
            errno = 0;
            return 0;
        }
        return -1;
    }

    size_t capacity = 8;
    size_t count = 0;
    task_run_entry_t *entries = malloc(capacity * sizeof(task_run_entry_t));
    if (entries == NULL) {
        free(buffer);
        errno = ENOMEM;
        return -1;
    }

    char *cursor = buffer;
    while (cursor && *cursor) {
        char *newline = strchr(cursor, '\n');
        if (newline != NULL) {
            *newline = '\0';
        }
        if (*cursor != '\0') {
            if (count >= capacity) {
                size_t new_cap = capacity * 2;
                task_run_entry_t *tmp = realloc(entries, new_cap * sizeof(task_run_entry_t));
                if (tmp == NULL) {
                    free(entries);
                    free(buffer);
                    errno = ENOMEM;
                    return -1;
                }
                capacity = new_cap;
                entries = tmp;
            }
            if (parse_history_entry(cursor, &entries[count]) != 0) {
                free(entries);
                free(buffer);
                return -1;
            }
            ++count;
        }
        if (newline == NULL) {
            break;
        }
        cursor = newline + 1;
    }

    free(buffer);
    *entries_out = entries;
    *entry_count_out = count;
    return 0;
}

static int read_stdio_file(const char *path, void **buffer_out, size_t *length_out) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT) {
            *buffer_out = NULL;
            *length_out = 0;
            errno = 0;
            return 0;
        }
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        return -1;
    }
    size_t size = (size_t)st.st_size;
    void *buffer = NULL;
    if (size > 0) {
        buffer = malloc(size);
        if (buffer == NULL) {
            close(fd);
            errno = ENOMEM;
            return -1;
        }
        size_t total = 0;
        while (total < size) {
            ssize_t n = read(fd, (char *)buffer + total, size - total);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                free(buffer);
                close(fd);
                return -1;
            }
            if (n == 0) {
                break;
            }
            total += (size_t)n;
        }
        if (total != size) {
            free(buffer);
            close(fd);
            errno = EIO;
            return -1;
        }
    }
    close(fd);
    *buffer_out = buffer;
    *length_out = size;
    return 0;
}

int storage_load_last_stdio(const storage_paths_t *paths,
                            uint64_t task_id,
                            void **stdout_buf,
                            size_t *stdout_len,
                            void **stderr_buf,
                            size_t *stderr_len) {
    if (paths == NULL || stdout_buf == NULL || stderr_buf == NULL || stdout_len == NULL ||
        stderr_len == NULL) {
        errno = EINVAL;
        return -1;
    }

    char idbuf[32];
    int n = snprintf(idbuf, sizeof(idbuf), "%llu", (unsigned long long)task_id);
    if (n < 0 || (size_t)n >= sizeof(idbuf)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    char stdout_path[PATH_MAX];
    if (utils_join_path3(paths->logs_dir, idbuf, "last.stdout", stdout_path, sizeof(stdout_path)) != 0) {
        return -1;
    }
    char stderr_path[PATH_MAX];
    if (utils_join_path3(paths->logs_dir, idbuf, "last.stderr", stderr_path, sizeof(stderr_path)) != 0) {
        return -1;
    }

    if (read_stdio_file(stdout_path, stdout_buf, stdout_len) != 0) {
        return -1;
    }
    if (read_stdio_file(stderr_path, stderr_buf, stderr_len) != 0) {
        if (*stdout_buf != NULL) {
            free(*stdout_buf);
            *stdout_buf = NULL;
        }
        return -1;
    }
    return 0;
}

int storage_allocate_task_id(const storage_paths_t *paths, uint64_t *task_id_out) {
    if (paths == NULL || task_id_out == NULL) {
        errno = EINVAL;
        return -1;
    }
    char id_path[PATH_MAX];
    if (utils_join_path(paths->tasks_dir, "next_id", id_path, sizeof(id_path)) != 0) {
        return -1;
    }

    int fd = open(id_path, O_RDWR | O_CREAT, 0600);
    if (fd < 0) {
        return -1;
    }

    char buffer[64];
    ssize_t bytes = read(fd, buffer, sizeof(buffer) - 1);
    if (bytes < 0) {
        close(fd);
        return -1;
    }
    buffer[bytes >= 0 ? bytes : 0] = '\0';

    uint64_t next_id = 1;
    if (bytes > 0) {
        if (parse_uint64(buffer, &next_id) != 0) {
            close(fd);
            return -1;
        }
    }

    uint64_t allocated = next_id;
    ++next_id;

    if (lseek(fd, 0, SEEK_SET) < 0 || ftruncate(fd, 0) != 0) {
        close(fd);
        return -1;
    }
    int n = snprintf(buffer, sizeof(buffer), "%llu\n", (unsigned long long)next_id);
    if (n < 0 || write_all_fd(fd, buffer, (size_t)n) != 0) {
        close(fd);
        return -1;
    }
    if (fsync(fd) != 0) {
        close(fd);
        return -1;
    }
    close(fd);

    *task_id_out = allocated;
    return 0;
}

void storage_free_tasks(task_t *tasks, size_t count) {
    if (tasks == NULL) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        free_task(&tasks[i]);
    }
    free(tasks);
}
