#ifndef ERRAID_UTILS_H
#define ERRAID_UTILS_H

#include "common.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int utils_join_path(const char *a, const char *b, char *buffer, size_t buflen);

int utils_join_path3(const char *a, const char *b, const char *c, char *buffer, size_t buflen);

int utils_read_all(int fd, void *buffer, size_t max_len, ssize_t *bytes_read);

int utils_write_all(int fd, const void *buffer, size_t length);

int utils_base64_encode(const void *input, size_t input_len, char *output, size_t *output_len);

int utils_base64_decode(const char *input, void *output, size_t *output_len);

int utils_parse_uint64(const char *str, uint64_t *out);

int utils_parse_int64(const char *str, int64_t *out);

int utils_now_epoch(int64_t *out_epoch);

#ifdef __cplusplus
}
#endif

#endif /* ERRAID_UTILS_H */
