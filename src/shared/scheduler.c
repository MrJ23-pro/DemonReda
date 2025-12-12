#include "scheduler.h"

#include "utils.h"

#include <errno.h>
#include <time.h>

static bool is_minute_allowed(const schedule_t *schedule, int minute) {
    if (minute < 0 || minute >= 60) {
        return false;
    }
    return (schedule->minute_mask >> minute) & 0x1u;
}

static bool is_hour_allowed(const schedule_t *schedule, int hour) {
    if (hour < 0 || hour >= 24) {
        return false;
    }
    return (schedule->hour_mask >> hour) & 0x1u;
}

static bool is_weekday_allowed(const schedule_t *schedule, int weekday) {
    if (weekday < 0 || weekday >= 7) {
        return false;
    }
    return (schedule->weekday_mask >> weekday) & 0x1u;
}

int64_t scheduler_next_occurrence(const schedule_t *schedule, int64_t from_epoch) {
    if (schedule == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (!schedule->enabled) {
        return -1;
    }

    if (from_epoch < 0) {
        from_epoch = 0;
    }

    int64_t start_epoch = from_epoch - (from_epoch % 60) + 60;

    const int max_iterations = 366 * 24 * 60; /* un an d'avance */
    int iterations = 0;

    for (int64_t current = start_epoch; iterations < max_iterations; current += 60, ++iterations) {
        time_t t = (time_t)current;
        struct tm tm_local;
        if (localtime_r(&t, &tm_local) == NULL) {
            return -1;
        }

        if (!is_weekday_allowed(schedule, tm_local.tm_wday)) {
            continue;
        }
        if (!is_hour_allowed(schedule, tm_local.tm_hour)) {
            continue;
        }
        if (!is_minute_allowed(schedule, tm_local.tm_min)) {
            continue;
        }

        return current;
    }

    errno = EOVERFLOW;
    return -1;
}

int scheduler_compute_plan(const task_t *tasks,
                           size_t task_count,
                           int64_t reference_epoch,
                           schedule_entry_t *entries,
                           size_t entry_capacity) {
    if ((task_count > 0 && (tasks == NULL || entries == NULL)) || entry_capacity < task_count) {
        errno = EINVAL;
        return -1;
    }

    for (size_t i = 0; i < task_count; ++i) {
        entries[i].task_id = tasks[i].task_id;
        entries[i].task_index = i;
        entries[i].next_epoch = scheduler_next_occurrence(&tasks[i].schedule, reference_epoch);
    }

    return (int)task_count;
}
