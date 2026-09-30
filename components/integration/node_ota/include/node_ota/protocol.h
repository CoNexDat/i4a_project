#ifndef NODE_OTA_PROTOCOL_H
#define NODE_OTA_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NODE_OTA_VERSION 1
#define NODE_OTA_CENTER 4
#define NODE_OTA_ROLES 0x1f
#define NODE_OTA_HOST 0xff
#define NODE_OTA_REPLY 0x80
#define NODE_OTA_CHUNK 448
#define NODE_OTA_HEADER 18
#define NODE_OTA_MAX_PACKET (NODE_OTA_HEADER + NODE_OTA_CHUNK + 4)
#define NODE_OTA_PREFIX "I4AOTA:"

enum node_ota_op {
    NODE_OTA_INFO = 1,
    NODE_OTA_BEGIN,
    NODE_OTA_DATA,
    NODE_OTA_END,
    NODE_OTA_SELECT,
    NODE_OTA_ABORT,
    NODE_OTA_REBOOT,
};

enum node_ota_phase {
    NODE_OTA_IDLE,
    NODE_OTA_RECEIVING,
    NODE_OTA_READY,
    NODE_OTA_SELECTED,
};

/* Integers are little endian on the wire. No compiler-dependent struct layout. */
typedef struct {
    uint8_t op, targets, sender;
    uint32_t session, sequence, offset;
    uint16_t length;
    uint8_t data[NODE_OTA_CHUNK];
} node_ota_packet_t;

uint32_t node_ota_get_u32(const uint8_t *p);
void node_ota_put_u32(uint8_t *p, uint32_t value);
uint32_t node_ota_crc32(const uint8_t *data, size_t length);
size_t node_ota_encode(const node_ota_packet_t *packet, uint8_t *wire);
bool node_ota_decode(const uint8_t *wire, size_t length, node_ota_packet_t *packet);

#endif
