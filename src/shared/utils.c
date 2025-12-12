#include "utils.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static int sanitize_join_inputs(const char *a, const char *b, char *buffer, size_t buflen) {
    if (buffer == NULL || buflen == 0 || a == NULL || b == NULL) {
        errno = EINVAL;
        return -1;
    }
    buffer[0] = '\0';
    return 0;
}

int utils_join_path(const char *a, const char *b, char *buffer, size_t buflen) {
    if (sanitize_join_inputs(a, b, buffer, buflen) != 0) {
        return -1;
    }

    size_t len_a = strlen(a);
    size_t len_b = strlen(b);

    while (len_b > 0 && b[0] == '/') {
        ++b;
        --len_b;
    }

    bool need_sep = (len_a > 0 && a[len_a - 1] != '/');

    size_t total_len = len_a + (need_sep ? 1 : 0) + len_b;
    if (total_len + 1 > buflen) {
        errno = ENAMETOOLONG;
        return -1;
    }

    memcpy(buffer, a, len_a);
    size_t pos = len_a;
    if (need_sep) {
        buffer[pos++] = '/';
    }
    memcpy(buffer + pos, b, len_b);
    pos += len_b;
    buffer[pos] = '\0';

    return 0;
}

int utils_join_path3(const char *a, const char *b, const char *c, char *buffer, size_t buflen) {
    if (sanitize_join_inputs(a, b, buffer, buflen) != 0 || c == NULL) {
        return -1;
    }

    char tmp[PATH_MAX];
    if (utils_join_path(a, b, tmp, sizeof(tmp)) != 0) {
        return -1;
    }

    return utils_join_path(tmp, c, buffer, buflen);
}

int utils_read_all(int fd, void *buffer, size_t max_len, ssize_t *bytes_read) {
    if (buffer == NULL) {
        errno = EINVAL;
        return -1;
    }
    size_t total = 0;
    char *dst = (char *)buffer;

    while (total < max_len) {
        ssize_t n = read(fd, dst + total, max_len - total);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            break;
        }
        total += (size_t)n;
    }

    if (bytes_read != NULL) {
        *bytes_read = (ssize_t)total;
    }

    if (total == max_len) {
        ssize_t n = read(fd, dst, 1);
        if (n > 0) {
            errno = EOVERFLOW;
            return -1;
        }
    }

    return 0;
}

int utils_write_all(int fd, const void *buffer, size_t length) {
    const char *src = (const char *)buffer;
    size_t total = 0;

    while (total < length) {
        ssize_t n = write(fd, src + total, length - total);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        total += (size_t)n;
    }

    return 0;
}

static const char base64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int utils_base64_encode(const void *input, size_t input_len, char *output, size_t *output_len) {
    if (output == NULL || output_len == NULL) {
        errno = EINVAL;
        return -1;
    }

    size_t needed = ((input_len + 2) / 3) * 4;
    if (*output_len < needed + 1) {
        errno = ENOSPC;
        return -1;
    }

    const unsigned char *data = (const unsigned char *)input;
    size_t out_pos = 0;
    for (size_t i = 0; i < input_len; i += 3) {
        unsigned int octet_a = i < input_len ? data[i] : 0;
        unsigned int octet_b = (i + 1) < input_len ? data[i + 1] : 0;
        unsigned int octet_c = (i + 2) < input_len ? data[i + 2] : 0;

        unsigned int triple = (octet_a << 16) | (octet_b << 8) | octet_c;

        output[out_pos++] = base64_table[(triple >> 18) & 0x3F];
        output[out_pos++] = base64_table[(triple >> 12) & 0x3F];
        output[out_pos++] = (i + 1) < input_len ? base64_table[(triple >> 6) & 0x3F] : '=';
        output[out_pos++] = (i + 2) < input_len ? base64_table[triple & 0x3F] : '=';
    }

    output[out_pos] = '\0';
    *output_len = out_pos;

    return 0;
}

static int base64_inverse(char c) {
    if (c >= 'A' && c <= 'Z') { return c - 'A'; }
    if (c >= 'a' && c <= 'z') { return c - 'a' + 26; }
    if (c >= '0' && c <= '9') { return c - '0' + 52; }
    if (c == '+') { return 62; }
    if (c == '/') { return 63; }
    return -1;
}

int utils_base64_decode(const char *input, void *output, size_t *output_len) {
    if (input == NULL || output == NULL || output_len == NULL) {
        errno = EINVAL;
        return -1;
    }

    size_t len = strlen(input);
    if (len % 4 != 0) {
        errno = EINVAL;
        return -1;
    }

    size_t padding = 0;
    if (len >= 1 && input[len - 1] == '=') { ++padding; }
    if (len >= 2 && input[len - 2] == '=') { ++padding; }

    size_t expected = (len / 4) * 3 - padding;
    if (*output_len < expected) {
        errno = ENOSPC;
        return -1;
    }

    unsigned char *dst = (unsigned char *)output;
    size_t out_pos = 0;

    for (size_t i = 0; i < len; i += 4) {
        int sextet_a = base64_inverse(input[i]);
        int sextet_b = base64_inverse(input[i + 1]);
        int sextet_c = input[i + 2] == '=' ? -1 : base64_inverse(input[i + 2]);
        int sextet_d = input[i + 3] == '=' ? -1 : base64_inverse(input[i + 3]);

        if (sextet_a < 0 || sextet_b < 0 || (sextet_c < 0 && input[i + 2] != '=') ||
            (sextet_d < 0 && input[i + 3] != '=')) {
            errno = EINVAL;
            return -1;
        }

        unsigned int triple = (unsigned int)(sextet_a << 18) | (unsigned int)(sextet_b << 12);
        if (input[i + 2] != '=') {
            triple |= (unsigned int)(sextet_c << 6);
        }
        if (input[i + 3] != '=') {
            triple |= (unsigned int)sextet_d;
        }

        dst[out_pos++] = (unsigned char)((triple >> 16) & 0xFFu);
        if (input[i + 2] != '=') {
            dst[out_pos++] = (unsigned char)((triple >> 8) & 0xFFu);
        }
        if (input[i + 3] != '=') {
            dst[out_pos++] = (unsigned char)(triple & 0xFFu);
        }
    }

    *output_len = out_pos;
    return 0;
}

int utils_parse_uint64(const char *str, uint64_t *out) {
    if (str == NULL || out == NULL) {
        errno = EINVAL;
        return -1;
    }
    char *endptr = NULL;
    errno = 0;
    unsigned long long value = strtoull(str, &endptr, 10);
    if (errno != 0 || endptr == str || *endptr != '\0') {
        if (errno == 0) {
            errno = EINVAL;
        }
        return -1;
    }
    *out = (uint64_t)value;
    return 0;
}

int utils_parse_int64(const char *str, int64_t *out) {
    if (str == NULL || out == NULL) {
        errno = EINVAL;
        return -1;
    }
    char *endptr = NULL;
    errno = 0;
    long long value = strtoll(str, &endptr, 10);
    if (errno != 0 || endptr == str || *endptr != '\0') {
        if (errno == 0) {
            errno = EINVAL;
        }
        return -1;
    }
    *out = (int64_t)value;
    return 0;
}

int utils_now_epoch(int64_t *out_epoch) {
    if (out_epoch == NULL) {
        errno = EINVAL;
        return -1;
    }

    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        return -1;
    }
    *out_epoch = (int64_t)ts.tv_sec;
    return 0;
}
