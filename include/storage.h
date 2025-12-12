#ifndef ERRAID_STORAGE_H
#define ERRAID_STORAGE_H

#include "common.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *root_dir;
    const char *tasks_dir;
    const char *logs_dir;
    const char *state_dir;
    const char *pipes_dir;
} storage_paths_t;

int storage_init_directories(const storage_paths_t *paths);

int storage_load_tasks(const storage_paths_t *paths, task_t **tasks_out, size_t *count_out);

int storage_write_task(const storage_paths_t *paths, const task_t *task);

int storage_remove_task(const storage_paths_t *paths, uint64_t task_id);

int storage_append_history(const storage_paths_t *paths,
                           uint64_t task_id,
                           const task_run_entry_t *entry,
                           const void *stdout_buf,
                           size_t stdout_len,
                           const void *stderr_buf,
                           size_t stderr_len);

int storage_load_history(const storage_paths_t *paths,
                         uint64_t task_id,
                         task_run_entry_t **entries_out,
                         size_t *entry_count_out);

int storage_load_last_stdio(const storage_paths_t *paths,
                            uint64_t task_id,
                            void **stdout_buf,
                            size_t *stdout_len,
                            void **stderr_buf,
                            size_t *stderr_len);

int storage_allocate_task_id(const storage_paths_t *paths, uint64_t *task_id_out);

void storage_free_tasks(task_t *tasks, size_t count);

#ifdef __cplusplus
}
#endif

#endif /* ERRAID_STORAGE_H */
