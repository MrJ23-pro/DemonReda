#include "erraid.h"

#include <errno.h>
#include <getopt.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

static void usage(const char *progname) {
    log_fd(STDERR_FILENO, "Usage : %s [-r RUNDIR]\n", progname);
}

int main(int argc, char **argv) {
    const char *run_dir = NULL;

    int opt;
    while ((opt = getopt(argc, argv, "hr:")) != -1) {
        switch (opt) {
            case 'r':
                run_dir = optarg;
                break;
            case 'h':
                usage(argv[0]);
                return EXIT_SUCCESS;
            default:
                usage(argv[0]);
                return EXIT_FAILURE;
        }
    }

    erraid_context_t ctx;
    if (erraid_init(&ctx, run_dir) != 0) {
        log_fd(STDERR_FILENO, "erraid: initialisation échouée (%s)\n", strerror(errno));
        return EXIT_FAILURE;
    }

    int rc = erraid_run(&ctx);
    if (rc != 0) {
        log_fd(STDERR_FILENO, "erraid: exécution échouée (%s)\n", strerror(errno));
    }

    erraid_shutdown(&ctx);

    return (rc == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
