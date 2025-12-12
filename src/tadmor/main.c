#include "tadmor.h"

#include "proto.h"
#include "utils.h"

#include <errno.h>
#include <getopt.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(const char *progname) {
    static const char help_tail[] =
        "  -l                 Lister les tâches\n"
        "  -q                 Demander l'arrêt du démon\n"
        "  -c                 Créer une tâche simple\n"
        "  -s                 Créer une tâche séquentielle\n"
        "  -n                 Créer une tâche abstraite\n"
        "  -r TASKID          Supprimer une tâche\n"
        "  -x TASKID          Afficher l'historique d'une tâche\n"
        "  -o TASKID          Afficher le dernier stdout\n"
        "  -e TASKID          Afficher le dernier stderr\n"
        "  -p DIR             Répertoire des pipes\n"
        "  -m MASK            Masque des minutes (hexadécimal, 15 caractères)\n"
        "  -H MASK            Masque des heures (hexadécimal, 6 caractères)\n"
        "  -w MASK            Masque des jours (hexadécimal, 2 caractères)\n"
        "  [commande ...]     Commande(s) et arguments, séparées par '--' pour les séquences\n";
    log_fd(STDERR_FILENO, "Usage : %s [options]\n", progname);
    utils_write_all(STDERR_FILENO, help_tail, sizeof(help_tail) - 1);
}

static int build_request_payload(const tadmor_options_t *opts, message_type_t *out_type, char *payload, size_t payload_cap, size_t *payload_len);

static int buffer_append(char *buffer, size_t cap, size_t *offset, const char *fmt, ...) {
    if (buffer == NULL || offset == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (*offset >= cap) {
        errno = ENOSPC;
        return -1;
    }

    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(buffer + *offset, cap - *offset, fmt, args);
    va_end(args);

    if (written < 0) {
        return -1;
    }
    if ((size_t)written >= cap - *offset) {
        errno = ENOSPC;
        return -1;
    }
    *offset += (size_t)written;
    return 0;
}

static int json_escape_string(const char *input, char *buffer, size_t cap, size_t *offset) {
    if (buffer_append(buffer, cap, offset, "\"") != 0) {
        return -1;
    }
    for (const unsigned char *p = (const unsigned char *)input; p != NULL && *p != '\0'; ++p) {
        unsigned char ch = *p;
        switch (ch) {
            case '\\':
                if (buffer_append(buffer, cap, offset, "\\\\") != 0) {
                    return -1;
                }
                break;
            case '\"':
                if (buffer_append(buffer, cap, offset, "\\\"") != 0) {
                    return -1;
                }
                break;
            case '\b':
                if (buffer_append(buffer, cap, offset, "\\b") != 0) {
                    return -1;
                }
                break;
            case '\f':
                if (buffer_append(buffer, cap, offset, "\\f") != 0) {
                    return -1;
                }
                break;
            case '\n':
                if (buffer_append(buffer, cap, offset, "\\n") != 0) {
                    return -1;
                }
                break;
            case '\r':
                if (buffer_append(buffer, cap, offset, "\\r") != 0) {
                    return -1;
                }
                break;
            case '\t':
                if (buffer_append(buffer, cap, offset, "\\t") != 0) {
                    return -1;
                }
                break;
            default:
                if (ch < 0x20) {
                    if (buffer_append(buffer, cap, offset, "\\u%04X", ch) != 0) {
                        return -1;
                    }
                } else {
                    if (buffer_append(buffer, cap, offset, "%c", ch) != 0) {
                        return -1;
                    }
                }
                break;
        }
    }
    if (buffer_append(buffer, cap, offset, "\"") != 0) {
        return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    tadmor_options_t opts;
    memset(&opts, 0, sizeof(opts));

    if (tadmor_parse_args(argc, argv, &opts) != 0) {
        usage(argv[0]);
        tadmor_free_options(&opts);
        return EXIT_FAILURE;
    }

    char payload[ERRAID_PIPE_MESSAGE_LIMIT];
    size_t payload_len = 0;
    message_type_t type = MSG_PING;

    if (build_request_payload(&opts, &type, payload, sizeof(payload), &payload_len) != 0) {
        log_fd(STDERR_FILENO, "tadmor: impossible de construire la requête (%s)\n", strerror(errno));
        tadmor_free_options(&opts);
        return EXIT_FAILURE;
    }

    tadmor_connection_t conn;
    memset(&conn, 0, sizeof(conn));
    if (tadmor_connect(&conn, opts.pipes_dir_arg) != 0) {
        log_fd(STDERR_FILENO, "tadmor: connexion aux pipes impossible (%s)\n", strerror(errno));
        tadmor_free_options(&opts);
        return EXIT_FAILURE;
    }

    proto_message_t message;
    if (proto_pack(type, payload, payload_len, &message) != 0) {
        log_fd(STDERR_FILENO, "tadmor: assemblage du message impossible (%s)\n", strerror(errno));
        tadmor_close(&conn);
        tadmor_free_options(&opts);
        return EXIT_FAILURE;
    }

    if (tadmor_send_request(&conn, &message) != 0) {
        log_fd(STDERR_FILENO, "tadmor: envoi de la requête impossible (%s)\n", strerror(errno));
        tadmor_close(&conn);
        tadmor_free_options(&opts);
        return EXIT_FAILURE;
    }

    if (tadmor_receive_reply(&conn, &reply) != 0) {
        log_fd(STDERR_FILENO, "tadmor: lecture de la réponse impossible (%s)\n", strerror(errno));
        tadmor_close(&conn);
        tadmor_free_options(&opts);
        return EXIT_FAILURE;
    }

    if (tadmor_handle_reply(&opts, &reply) != 0) {
        tadmor_close(&conn);
        tadmor_free_options(&opts);
        return EXIT_FAILURE;
    }

    tadmor_close(&conn);
    tadmor_free_options(&opts);
    return EXIT_SUCCESS;
}

static int build_commands_array(const tadmor_options_t *opts, char *buffer, size_t cap, size_t *offset) {
    if (buffer_append(buffer, cap, offset, "\"commands\":[") != 0) {
        return -1;
    }
    for (size_t i = 0; i < opts->command_count; ++i) {
        if (i > 0) {
            if (buffer_append(buffer, cap, offset, ",") != 0) {
                return -1;
            }
        }
        if (buffer_append(buffer, cap, offset, "[") != 0) {
            return -1;
        }
        const command_t *cmd = &opts->commands[i];
        for (size_t j = 0; j < cmd->argc; ++j) {
            if (j > 0) {
                if (buffer_append(buffer, cap, offset, ",") != 0) {
                    return -1;
                }
            }
            if (json_escape_string(cmd->argv[j], buffer, cap, offset) != 0) {
                return -1;
            }
        }
        if (buffer_append(buffer, cap, offset, "]") != 0) {
            return -1;
        }
    }
    if (buffer_append(buffer, cap, offset, "]") != 0) {
        return -1;
    }
    return 0;
}

static int build_schedule_object(const tadmor_options_t *opts, char *buffer, size_t cap, size_t *offset) {
    if (!opts->has_schedule) {
        return buffer_append(buffer, cap, offset, "\"schedule\":null");
    }
    if (buffer_append(buffer,
                      cap,
                      offset,
                      "\"schedule\":{\"minutes\":\"%s\",\"hours\":\"%s\",\"weekdays\":\"%s\"}",
                      opts->minutes,
                      opts->hours,
                      opts->weekdays) != 0) {
        return -1;
    }
    return 0;
}

static int build_request_payload(const tadmor_options_t *opts,
                                 message_type_t *out_type,
                                 char *payload,
                                 size_t payload_cap,
                                 size_t *payload_len) {
    size_t offset = 0;

    if (opts->opt_list) {
        *out_type = MSG_REQ_LIST_TASKS;
        if (buffer_append(payload, payload_cap, &offset, "{}") != 0) {
            return -1;
        }
    } else if (opts->opt_shutdown) {
        *out_type = MSG_REQ_SHUTDOWN;
        if (buffer_append(payload, payload_cap, &offset, "{}") != 0) {
            return -1;
        }
    } else if (opts->opt_remove) {
        *out_type = MSG_REQ_REMOVE;
        if (buffer_append(payload,
                          payload_cap,
                          &offset,
                          "{\"task_id\":%llu}",
                          (unsigned long long)opts->task_id) != 0) {
            return -1;
        }
    } else if (opts->opt_history) {
        *out_type = MSG_REQ_LIST_HISTORY;
        if (buffer_append(payload,
                          payload_cap,
                          &offset,
                          "{\"task_id\":%llu}",
                          (unsigned long long)opts->task_id) != 0) {
            return -1;
        }
    } else if (opts->opt_stdout) {
        *out_type = MSG_REQ_GET_STDOUT;
        if (buffer_append(payload,
                          payload_cap,
                          &offset,
                          "{\"task_id\":%llu}",
                          (unsigned long long)opts->task_id) != 0) {
            return -1;
        }
    } else if (opts->opt_stderr) {
        *out_type = MSG_REQ_GET_STDERR;
        if (buffer_append(payload,
                          payload_cap,
                          &offset,
                          "{\"task_id\":%llu}",
                          (unsigned long long)opts->task_id) != 0) {
            return -1;
        }
    } else if (opts->opt_create_simple || opts->opt_create_sequence || opts->opt_create_abstract) {
        if (buffer_append(payload, payload_cap, &offset, "{") != 0) {
            return -1;
        }
        if (build_commands_array(opts, payload, payload_cap, &offset) != 0) {
            return -1;
        }
        if (buffer_append(payload, payload_cap, &offset, ",") != 0) {
            return -1;
        }
        if (build_schedule_object(opts, payload, payload_cap, &offset) != 0) {
            return -1;
        }
        if (buffer_append(payload, payload_cap, &offset, "}") != 0) {
            return -1;
        }

        if (opts->opt_create_simple) {
            *out_type = MSG_REQ_CREATE_SIMPLE;
        } else if (opts->opt_create_sequence) {
            *out_type = MSG_REQ_CREATE_SEQUENCE;
        } else {
            *out_type = MSG_REQ_CREATE_ABSTRACT;
        }
    } else {
        errno = EINVAL;
        return -1;
    }

    *payload_len = offset;
    return 0;
}
