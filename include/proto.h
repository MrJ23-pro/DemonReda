#ifndef ERRAID_PROTO_H
#define ERRAID_PROTO_H

#include "common.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    message_header_t header;
    char payload[ERRAID_PIPE_MESSAGE_LIMIT];
} proto_message_t;

int proto_pack(message_type_t type, const char *json_payload, size_t payload_len, proto_message_t *out_msg);

int proto_unpack_header(const void *buffer, size_t length, message_header_t *out_header);

int proto_validate_header(const message_header_t *header);

int proto_read_message(int fd, proto_message_t *out_msg);

int proto_write_message(int fd, const proto_message_t *msg);

#ifdef __cplusplus
}
#endif

#endif /* ERRAID_PROTO_H */
