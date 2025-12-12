#ifndef ERRAID_SCHEDULER_H
#define ERRAID_SCHEDULER_H

#include "common.h"

#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t task_id;
    size_t task_index;
    int64_t next_epoch; /* -1 si aucune occurrence trouvée */
} schedule_entry_t;

int64_t scheduler_next_occurrence(const schedule_t *schedule, int64_t from_epoch);

int scheduler_compute_plan(const task_t *tasks,
                           size_t task_count,
                           int64_t reference_epoch,
                           schedule_entry_t *entries,
                           size_t entry_capacity);

#ifdef __cplusplus
}
#endif

#endif /* ERRAID_SCHEDULER_H */
