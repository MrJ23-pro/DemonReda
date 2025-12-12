#include "tadmor.h"

#include "proto.h"
#include "utils.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static void log_fd(int fd, const char *fmt, ...) {
    char buffer[256];
    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    if (written < 0) {
        return;
    }
    size_t len = (size_t)written;
    if (len >= sizeof(buffer)) {
        len = sizeof(buffer) - 1;
    }
    utils_write_all(fd, buffer, len);
}

static int ensure_default_pipes_dir(char *buffer, size_t size) {
    const char *user = getenv("USER");
    if (user == NULL || user[0] == '\0') {
        user = "user";
    }
    int n = snprintf(buffer, size, "%s/%s%s/%s",
                     ERRAID_DEFAULT_RUNDIR_PREFIX,
                     user,
                     ERRAID_DEFAULT_RUNDIR_SUFFIX,
                     ERRAID_PIPES_DIR_NAME);
    if (n < 0 || (size_t)n >= size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static int open_fifo(const char *path, int flags) {
    int fd = open(path, flags);
    if (fd < 0) {
        return -1;
    }
    if (flags & O_NONBLOCK) {
        int current = fcntl(fd, F_GETFL, 0);
        if (current < 0) {
            close(fd);
            return -1;
        }
        if (fcntl(fd, F_SETFL, current & ~O_NONBLOCK) < 0) {
            close(fd);
            return -1;
        }
    }
    return fd;
}

int tadmor_connect(tadmor_connection_t *conn, const char *pipes_dir_arg) {
    if (conn == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(conn, 0, sizeof(*conn));
    conn->request_fd = -1;
    conn->reply_fd = -1;

    if (pipes_dir_arg != NULL) {
        if (strlen(pipes_dir_arg) >= sizeof(conn->pipes_dir)) {
            errno = ENAMETOOLONG;
            return -1;
        }
        strcpy(conn->pipes_dir, pipes_dir_arg);
    } else {
        if (ensure_default_pipes_dir(conn->pipes_dir, sizeof(conn->pipes_dir)) != 0) {
            return -1;
        }
    }

    if (utils_join_path(conn->pipes_dir, ERRAID_PIPE_REQUEST_NAME, conn->request_pipe, sizeof(conn->request_pipe)) != 0) {
        return -1;
    }
    if (utils_join_path(conn->pipes_dir, ERRAID_PIPE_REPLY_NAME, conn->reply_pipe, sizeof(conn->reply_pipe)) != 0) {
        return -1;
    }

    conn->request_fd = open_fifo(conn->request_pipe, O_WRONLY);
    if (conn->request_fd < 0) {
        return -1;
    }
    conn->reply_fd = open_fifo(conn->reply_pipe, O_RDONLY | O_NONBLOCK);
    if (conn->reply_fd < 0) {
        close(conn->request_fd);
        conn->request_fd = -1;
        return -1;
    }
    return 0;
}

void tadmor_close(tadmor_connection_t *conn) {
    if (conn == NULL) {
        return;
    }
    if (conn->request_fd >= 0) {
        close(conn->request_fd);
        conn->request_fd = -1;
    }
    if (conn->reply_fd >= 0) {
        close(conn->reply_fd);
        conn->reply_fd = -1;
    }
}

int tadmor_send_request(tadmor_connection_t *conn, const proto_message_t *req) {
    if (conn == NULL || req == NULL) {
        errno = EINVAL;
        return -1;
    }
    return proto_write_message(conn->request_fd, req);
}

int tadmor_receive_reply(tadmor_connection_t *conn, proto_message_t *rsp) {
    if (conn == NULL || rsp == NULL) {
        errno = EINVAL;
        return -1;
    }
    return proto_read_message(conn->reply_fd, rsp);
}

static int decode_base64_field(const char *json, const char *field, int fd) {
    const char *ptr = strstr(json, field);
    if (ptr == NULL) {
        log_fd(STDERR_FILENO, "Champ %s absent\n", field);
        return -1;
    }
    ptr += strlen(field);
    while (*ptr && *ptr != '"') {
        ++ptr;
    }
    if (*ptr != '"') {
        log_fd(STDERR_FILENO, "Champ %s mal formé\n", field);
        return -1;
    }
    ++ptr;
    const char *end = strchr(ptr, '"');
    if (end == NULL) {
        log_fd(STDERR_FILENO, "Champ %s mal formé\n", field);
        return -1;
    }

    size_t encoded_len = (size_t)(end - ptr);
    size_t decoded_len = encoded_len;
    char *decoded = malloc(decoded_len);
    if (decoded == NULL) {
        perror("malloc");
        return -1;
    }
    if (utils_base64_decode(ptr, decoded, &decoded_len) != 0) {
        perror("base64");
        free(decoded);
        return -1;
    }
    utils_write_all(fd, decoded, decoded_len);
    free(decoded);
    return 0;
}

int tadmor_handle_reply(const tadmor_options_t *opts, const proto_message_t *rsp) {
    (void)opts;
    if (rsp == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (rsp->header.type == MSG_RSP_ERROR) {
        log_fd(STDERR_FILENO, "Erreur: %s\n", rsp->payload);
        return -1;
    }

    utils_write_all(STDOUT_FILENO, rsp->payload, rsp->header.payload_length);
    utils_write_all(STDOUT_FILENO, "\n", 1);

    if (opts != NULL && opts->opt_stdout) {
        decode_base64_field(rsp->payload, "\"stdout\":\"", STDOUT_FILENO);
    } else if (opts != NULL && opts->opt_stderr) {
        decode_base64_field(rsp->payload, "\"stderr\":\"", STDERR_FILENO);
    }

    return 0;
}

int tadmor_parse_args(int argc, char **argv, tadmor_options_t *opts) {
    if (opts == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(opts, 0, sizeof(*opts));
    optind = 1;
    opterr = 0;

    int opt;
    while ((opt = getopt(argc, argv, "lqcsnr:x:o:e:p:m:H:w:")) != -1) {
        switch (opt) {
            case 'l': opts->opt_list = true; break;
            case 'q': opts->opt_shutdown = true; break;
            case 'c': opts->opt_create_simple = true; break;
            case 's': opts->opt_create_sequence = true; break;
            case 'n': opts->opt_create_abstract = true; break;
            case 'r':
            case 'x':
            case 'o':
            case 'e':
                opts->opt_remove |= (opt == 'r');
                opts->opt_history |= (opt == 'x');
                opts->opt_stdout |= (opt == 'o');
                opts->opt_stderr |= (opt == 'e');
                if (utils_parse_uint64(optarg, &opts->task_id) != 0) {
                    return -1;
                }
                break;
            case 'p': opts->pipes_dir_arg = optarg; break;
            case 'm':
                if (strlen(optarg) != 15) {
                    errno = EINVAL;
                    return -1;
                }
                strcpy(opts->minutes, optarg);
                opts->has_schedule = true;
                break;
            case 'H':
                if (strlen(optarg) != 6) {
                    errno = EINVAL;
                    return -1;
                }
                strcpy(opts->hours, optarg);
                opts->has_schedule = true;
                break;
            case 'w':
                if (strlen(optarg) != 2) {
                    errno = EINVAL;
                    return -1;
                }
                strcpy(opts->weekdays, optarg);
                opts->has_schedule = true;
                break;
            default:
                return -1;
        }
    }

    int operations = 0;
    operations += opts->opt_list;
    operations += opts->opt_shutdown;
    operations += opts->opt_create_simple;
    operations += opts->opt_create_sequence;
    operations += opts->opt_create_abstract;
    operations += opts->opt_remove;
    operations += opts->opt_history;
    operations += opts->opt_stdout;
    operations += opts->opt_stderr;

    if (operations != 1) {
        errno = EINVAL;
        return -1;
    }

    if (opts->opt_create_simple || opts->opt_create_sequence || opts->opt_create_abstract) {
        size_t cmd_count = 0;
        size_t capacity = 1;
        opts->commands = calloc(capacity, sizeof(command_t));
        if (opts->commands == NULL) {
            return -1;
        }

        command_t current;
        memset(&current, 0, sizeof(current));
        current.argv = calloc(ERRAID_MAX_COMMAND_ARGS + 1, sizeof(char *));
        if (current.argv == NULL) {
            return -1;
        }

        while (optind < argc) {
            if (strcmp(argv[optind], "--") == 0) {
                if (current.argc == 0) {
                    ++optind;
                    continue;
                }
                if (opts->opt_create_simple && cmd_count >= 1) {
                    errno = EINVAL;
                    return -1;
                }
                if (cmd_count >= capacity) {
                    capacity *= 2;
                    command_t *tmp = realloc(opts->commands, capacity * sizeof(command_t));
                    if (tmp == NULL) {
                        for (size_t j = 0; j < current.argc; ++j) {
                            free(current.argv[j]);
                        }
                        free(current.argv);
                        return -1;
                    }
                    opts->commands = tmp;
                }
                opts->commands[cmd_count++] = current;
                opts->command_count = cmd_count;
                memset(&current, 0, sizeof(current));
                current.argv = calloc(ERRAID_MAX_COMMAND_ARGS + 1, sizeof(char *));
                if (current.argv == NULL) {
                    return -1;
                }
                ++optind;
                continue;
            }

            if (current.argc >= ERRAID_MAX_COMMAND_ARGS) {
                errno = E2BIG;
                return -1;
            }
            current.argv[current.argc] = strdup(argv[optind]);
            if (current.argv[current.argc] == NULL) {
                return -1;
            }
            ++current.argc;
            current.argv[current.argc] = NULL;
            ++optind;
        }

        if (current.argc > 0 || cmd_count == 0) {
            if (cmd_count >= capacity) {
                capacity *= 2;
                command_t *tmp = realloc(opts->commands, capacity * sizeof(command_t));
                if (tmp == NULL) {
                    return -1;
                }
                if (!opts->has_schedule) {
                    errno = EINVAL;
                    return -1;
                }
            }
            if (opts->opt_create_sequence) {
                if (opts->command_count == 0) {
                    errno = EINVAL;
                    return -1;
                }
                for (size_t i = 0; i < opts->command_count; ++i) {
                    if (opts->commands[i].argc == 0) {
                        errno = EINVAL;
                        return -1;
                    }
                }
                if (!opts->has_schedule) {
                    errno = EINVAL;
                    return -1;
                }
            }
            if (opts->opt_create_abstract) {
                if (opts->command_count > 0) {
                    for (size_t i = 0; i < opts->command_count; ++i) {
                        if (opts->commands[i].argc == 0) {
                            errno = EINVAL;
                            return -1;
                        }
                    }
                }
            }
        } else {
            if (optind < argc) {
                errno = EINVAL;
                return -1;
            }
        }

        return 0;
    }

    return 0;
}

void tadmor_free_options(tadmor_options_t *opts) {
    if (opts == NULL) {
        return;
    }
    if (opts->commands != NULL) {
        for (size_t i = 0; i < opts->command_count; ++i) {
            for (size_t j = 0; j < opts->commands[i].argc; ++j) {
                free(opts->commands[i].argv[j]);
            }
            free(opts->commands[i].argv);
        }
        free(opts->commands);
    }
    memset(opts, 0, sizeof(*opts));
}
