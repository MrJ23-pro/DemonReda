#include "executor.h"

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PIPE_READ
#define PIPE_READ 0
#define PIPE_WRITE 1
#endif

static int append_buffer(char **buffer,
                         size_t *length,
                         size_t *capacity,
                         const char *data,
                         size_t data_len,
                         size_t limit,
                         bool *truncated) {
    if (data == NULL || data_len == 0) {
        return 0;
    }

    if (limit > 0 && *length >= limit) {
        if (truncated != NULL) {
            *truncated = true;
        }
        return 0;
    }

    size_t copy_len = data_len;
    if (limit > 0 && *length + copy_len > limit) {
        copy_len = limit - *length;
        if (truncated != NULL) {
            *truncated = true;
        }
    }
    if (copy_len == 0) {
        return 0;
    }

    if (*capacity < *length + data_len + 1) {
        size_t new_cap = (*capacity == 0) ? (data_len + 1) : *capacity;
        while (new_cap < *length + copy_len + 1) {
            new_cap *= 2;
        }
        char *tmp = realloc(*buffer, new_cap);
        if (tmp == NULL) {
            errno = ENOMEM;
            return -1;
        }
        *buffer = tmp;
        *capacity = new_cap;
    }
    memcpy(*buffer + *length, data, copy_len);
    *length += copy_len;
    (*buffer)[*length] = '\0';
    return 0;
}

static int read_fd_into_buffer(int fd, char **buffer, size_t *length, size_t limit, bool *truncated) {
    size_t capacity = (*buffer != NULL) ? (*length + 1) : 0;
    char temp[4096];

    for (;;) {
        ssize_t n = read(fd, temp, sizeof(temp));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            break;
        }
        size_t copy_len = (size_t)n;
        if (limit > 0) {
            size_t remaining = (limit > *length) ? (limit - *length) : 0;
            if (remaining < copy_len) {
                copy_len = remaining;
            }
        }

        if (append_buffer(buffer,
                          length,
                          &capacity,
                          temp,
                          copy_len,
                          limit,
                          truncated) != 0) {
            return -1;
        }
        if (limit > 0 && *length >= limit) {
            if (truncated != NULL && !*truncated) {
                *truncated = true;
            }
        }
    }

    if (*buffer == NULL) {
        *buffer = calloc(1, 1);
        if (*buffer == NULL) {
            errno = ENOMEM;
            return -1;
        }
        *length = 0;
        capacity = 1;
    }

    return 0;
}

static int run_single_command(const command_t *command,
                              int *status_out,
                              char **stdout_buf,
                              size_t *stdout_len,
                              bool *stdout_truncated,
                              char **stderr_buf,
                              size_t *stderr_len,
                              bool *stderr_truncated) {
    int stdout_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};

    if (command == NULL || command->argv == NULL || command->argc == 0) {
        errno = EINVAL;
        return -1;
    }

    if (pipe(stdout_pipe) != 0) {
        return -1;
    }
    if (pipe(stderr_pipe) != 0) {
        close(stdout_pipe[PIPE_READ]);
        close(stdout_pipe[PIPE_WRITE]);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(stdout_pipe[PIPE_READ]);
        close(stdout_pipe[PIPE_WRITE]);
        close(stderr_pipe[PIPE_READ]);
        close(stderr_pipe[PIPE_WRITE]);
        return -1;
    }

    if (pid == 0) {
        if (dup2(stdout_pipe[PIPE_WRITE], STDOUT_FILENO) < 0) {
            _exit(127);
        }
        if (dup2(stderr_pipe[PIPE_WRITE], STDERR_FILENO) < 0) {
            _exit(127);
        }

        close(stdout_pipe[PIPE_READ]);
        close(stdout_pipe[PIPE_WRITE]);
        close(stderr_pipe[PIPE_READ]);
        close(stderr_pipe[PIPE_WRITE]);

        execvp(command->argv[0], command->argv);
        _exit(127);
    }

    close(stdout_pipe[PIPE_WRITE]);
    close(stderr_pipe[PIPE_WRITE]);

    char *out_buf = NULL;
    char *err_buf = NULL;
    size_t out_len = 0;
    size_t err_len = 0;
    bool out_truncated = false;
    bool err_truncated = false;

    int rc = 0;
    if (read_fd_into_buffer(stdout_pipe[PIPE_READ], &out_buf, &out_len, ERRAID_MAX_STDIO_SNAPSHOT, &out_truncated) != 0) {
        rc = -1;
    }
    close(stdout_pipe[PIPE_READ]);

    if (rc == 0 &&
        read_fd_into_buffer(stderr_pipe[PIPE_READ], &err_buf, &err_len, ERRAID_MAX_STDIO_SNAPSHOT, &err_truncated) != 0) {
        rc = -1;
    }
    close(stderr_pipe[PIPE_READ]);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        rc = -1;
    }

    if (rc != 0) {
        free(out_buf);
        free(err_buf);
        return -1;
    }

    *stdout_buf = out_buf;
    *stderr_buf = err_buf;
    *stdout_len = out_len;
    *stderr_len = err_len;
    if (stdout_truncated != NULL) {
        *stdout_truncated = out_truncated;
    }
    if (stderr_truncated != NULL) {
        *stderr_truncated = err_truncated;
    }

    if (status_out != NULL) {
        if (WIFEXITED(status)) {
            *status_out = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            *status_out = 128 + WTERMSIG(status);
        } else {
            *status_out = status;
        }
    }

    return 0;
}

int executor_run_task(const task_t *task, executor_result_t *result) {
    if (task == NULL || result == NULL) {
        errno = EINVAL;
        return -1;
    }

    memset(result, 0, sizeof(*result));

    if (task->command_count == 0 || task->commands == NULL) {
        result->status = 0;
        return 0;
    }

    char *stdout_buf = NULL;
    size_t stdout_len = 0;
    size_t stdout_cap = 0;
    char *stderr_buf = NULL;
    size_t stderr_len = 0;
    size_t stderr_cap = 0;
    bool stdout_truncated = false;
    bool stderr_truncated = false;

    for (size_t i = 0; i < task->command_count; ++i) {
        char *cmd_out = NULL;
        char *cmd_err = NULL;
        size_t cmd_out_len = 0;
        size_t cmd_err_len = 0;
        int status = 0;

        if (run_single_command(&task->commands[i],
                               &status,
                               &cmd_out,
                               &cmd_out_len,
                               &stdout_truncated,
                               &cmd_err,
                               &cmd_err_len,
                               &stderr_truncated) != 0) {
            free(cmd_out);
            free(cmd_err);
            free(stdout_buf);
            free(stderr_buf);
            return -1;
        }

        if (append_buffer(&stdout_buf,
                          &stdout_len,
                          &stdout_cap,
                          cmd_out,
                          cmd_out_len,
                          ERRAID_MAX_STDIO_SNAPSHOT,
                          &stdout_truncated) != 0) {
            free(cmd_out);
            free(cmd_err);
            free(stdout_buf);
            free(stderr_buf);
            return -1;
        }
        if (append_buffer(&stderr_buf,
                          &stderr_len,
                          &stderr_cap,
                          cmd_err,
                          cmd_err_len,
                          ERRAID_MAX_STDIO_SNAPSHOT,
                          &stderr_truncated) != 0) {
            free(cmd_out);
            free(cmd_err);
            free(stdout_buf);
            free(stderr_buf);
            return -1;
        }

        free(cmd_out);
        free(cmd_err);
        result->status = status;

        if (task->type == TASK_TYPE_SIMPLE) {
            break;
        }
    }

    result->stdout_buf = stdout_buf;
    result->stderr_buf = stderr_buf;
    result->stdout_len = stdout_len;
    result->stderr_len = stderr_len;
    result->stdout_truncated = stdout_truncated;
    result->stderr_truncated = stderr_truncated;

    return 0;
}

void executor_result_free(executor_result_t *result) {
    if (result == NULL) {
        return;
    }
    free(result->stdout_buf);
    free(result->stderr_buf);
    result->stdout_buf = NULL;
    result->stderr_buf = NULL;
    result->stdout_len = 0;
    result->stderr_len = 0;
}
