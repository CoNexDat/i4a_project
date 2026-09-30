#include "node_ota/protocol.h"
#include <string.h>

uint32_t node_ota_get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

void node_ota_put_u32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = value >> (8 * i);
}

uint32_t node_ota_crc32(const uint8_t *data, size_t length)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1)));
    }
    return ~crc;
}

size_t node_ota_encode(const node_ota_packet_t *p, uint8_t *wire)
{
    if (p->length > NODE_OTA_CHUNK) return 0;
    wire[0] = NODE_OTA_VERSION;
    wire[1] = p->op;
    wire[2] = p->targets;
    wire[3] = p->sender;
    node_ota_put_u32(wire + 4, p->session);
    node_ota_put_u32(wire + 8, p->sequence);
    node_ota_put_u32(wire + 12, p->offset);
    wire[16] = p->length;
    wire[17] = p->length >> 8;
    memcpy(wire + NODE_OTA_HEADER, p->data, p->length);
    size_t length = NODE_OTA_HEADER + p->length;
    node_ota_put_u32(wire + length, node_ota_crc32(wire, length));
    return length + 4;
}

bool node_ota_decode(const uint8_t *wire, size_t length, node_ota_packet_t *p)
{
    if (length < NODE_OTA_HEADER + 4 || length > NODE_OTA_MAX_PACKET ||
        wire[0] != NODE_OTA_VERSION) return false;
    uint16_t payload = wire[16] | (uint16_t)wire[17] << 8;
    if (length != NODE_OTA_HEADER + (size_t)payload + 4 ||
        node_ota_get_u32(wire + length - 4) != node_ota_crc32(wire, length - 4))
        return false;
    p->op = wire[1];
    p->targets = wire[2];
    p->sender = wire[3];
    p->session = node_ota_get_u32(wire + 4);
    p->sequence = node_ota_get_u32(wire + 8);
    p->offset = node_ota_get_u32(wire + 12);
    p->length = payload;
    memcpy(p->data, wire + NODE_OTA_HEADER, payload);
    return true;
}
