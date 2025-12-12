#ifndef ERRAID_NOTIFIER_H
#define ERRAID_NOTIFIER_H

#include "erraid.h"

#ifdef __cplusplus
extern "C" {
#endif

int notifier_install(erraid_context_t *ctx);

void notifier_uninstall(void);

#ifdef __cplusplus
}
#endif

#endif /* ERRAID_NOTIFIER_H */
