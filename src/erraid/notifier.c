#include "notifier.h"

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

static erraid_context_t *g_ctx = NULL;
static struct sigaction g_old_int;
static struct sigaction g_old_term;
static struct sigaction g_old_pipe;
static int g_installed = 0;

static void wake_daemon(void) {
    if (g_ctx == NULL) {
        return;
    }
    if (g_ctx->wake_pipe[1] >= 0) {
        const uint8_t byte = 0xFF;
        ssize_t rc = write(g_ctx->wake_pipe[1], &byte, sizeof(byte));
        (void)rc;
    }
}

static void handle_shutdown(int signo) {
    if (g_ctx != NULL) {
        g_ctx->should_quit = true;
    }
    wake_daemon();
    (void)signo;
}

static void handle_pipe(int signo) {
    (void)signo;
    wake_daemon();
}

int notifier_install(erraid_context_t *ctx) {
    if (ctx == NULL) {
        errno = EINVAL;
        return -1;
    }
    g_ctx = ctx;

    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = handle_shutdown;
    sigemptyset(&act.sa_mask);

    if (sigaction(SIGINT, &act, &g_old_int) != 0) {
        return -1;
    }
    if (sigaction(SIGTERM, &act, &g_old_term) != 0) {
        sigaction(SIGINT, &g_old_int, NULL);
        return -1;
    }

    memset(&act, 0, sizeof(act));
    act.sa_handler = handle_pipe;
    sigemptyset(&act.sa_mask);
    if (sigaction(SIGPIPE, &act, &g_old_pipe) != 0) {
        sigaction(SIGINT, &g_old_int, NULL);
        sigaction(SIGTERM, &g_old_term, NULL);
        return -1;
    }

    g_installed = 1;
    return 0;
}

void notifier_uninstall(void) {
    if (!g_installed) {
        return;
    }

    sigaction(SIGINT, &g_old_int, NULL);
    sigaction(SIGTERM, &g_old_term, NULL);
    sigaction(SIGPIPE, &g_old_pipe, NULL);
    g_ctx = NULL;
    g_installed = 0;
}
