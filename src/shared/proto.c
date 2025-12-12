#include "proto.h"

#include "utils.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>

static int read_exact(int fd, void *buffer, size_t length) {
    size_t offset = 0;
    char *dst = (char *)buffer;
    while (offset < length) {
        ssize_t n = read(fd, dst + offset, length - offset);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            errno = EIO;
            return -1;
        }
        offset += (size_t)n;
    }
    return 0;
}

static int write_exact(int fd, const void *buffer, size_t length) {
    size_t offset = 0;
    const char *src = (const char *)buffer;
    while (offset < length) {
        ssize_t n = write(fd, src + offset, length - offset);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        offset += (size_t)n;
    }
    return 0;
}

int proto_pack(message_type_t type, const char *json_payload, size_t payload_len, proto_message_t *out_msg) {
    if (out_msg == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (payload_len > ERRAID_PIPE_MESSAGE_LIMIT - 1) {
        errno = ENOSPC;
        return -1;
    }

    out_msg->header.magic = ERRAID_MAGIC;
    out_msg->header.version = ERRAID_PROTO_VERSION;
    out_msg->header.type = (uint8_t)type;
    out_msg->header.reserved = 0;
    out_msg->header.payload_length = (uint32_t)payload_len;

    if (payload_len > 0 && json_payload != NULL) {
        memcpy(out_msg->payload, json_payload, payload_len);
    }
    out_msg->payload[payload_len] = '\0';

    return 0;
}

int proto_unpack_header(const void *buffer, size_t length, message_header_t *out_header) {
    if (buffer == NULL || out_header == NULL || length < sizeof(message_header_t)) {
        errno = EINVAL;
        return -1;
    }
    memcpy(out_header, buffer, sizeof(message_header_t));
    return 0;
}

int proto_validate_header(const message_header_t *header) {
    if (header == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (header->magic != ERRAID_MAGIC) {
        errno = EPROTO;
        return -1;
    }
    if (header->version != ERRAID_PROTO_VERSION) {
        errno = EPROTO;
        return -1;
    }
    if (header->payload_length >= ERRAID_PIPE_MESSAGE_LIMIT) {
        errno = EOVERFLOW;
        return -1;
    }
    return 0;
}

int proto_read_message(int fd, proto_message_t *out_msg) {
    if (out_msg == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (read_exact(fd, &out_msg->header, sizeof(message_header_t)) != 0) {
        return -1;
    }

    if (proto_validate_header(&out_msg->header) != 0) {
        return -1;
    }

    size_t payload_len = (size_t)out_msg->header.payload_length;
    if (payload_len > 0) {
        if (read_exact(fd, out_msg->payload, payload_len) != 0) {
            return -1;
        }
    }

    out_msg->payload[payload_len] = '\0';
    return 0;
}

int proto_write_message(int fd, const proto_message_t *msg) {
    if (msg == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (proto_validate_header(&msg->header) != 0) {
        return -1;
    }

    if (write_exact(fd, &msg->header, sizeof(message_header_t)) != 0) {
        return -1;
    }

    if (msg->header.payload_length > 0) {
        if (write_exact(fd, msg->payload, msg->header.payload_length) != 0) {
            return -1;
        }
    }

    return 0;
}
