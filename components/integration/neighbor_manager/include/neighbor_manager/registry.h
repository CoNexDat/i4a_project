#ifndef I4A_NEIGHBOR_REGISTRY_H
#define I4A_NEIGHBOR_REGISTRY_H

#include <stdbool.h>
#include <stdint.h>

#define NM_UUID_LEN 12
#define NM_RADIOS 4
#define NM_PENDING_MS 20000
#define NM_ACTIVE_MS 30000

/* The Station creates this identifier; both endpoints use the same ordering. */
typedef struct {
    char initiator_uuid[NM_UUID_LEN + 1];
    uint32_t session;
    uint8_t station_orientation;
    uint8_t ap_orientation;
} nm_link_id_t;

typedef enum {
    NM_FREE = 0,
    NM_RESERVED,
    NM_COMMITTED,
    NM_ACTIVE,
} nm_phase_t;

typedef struct {
    char peer_uuid[NM_UUID_LEN + 1];
    nm_link_id_t link;
    uint64_t deadline_ms;
    uint8_t phase;
} nm_claim_t;

typedef struct {
    nm_claim_t claims[NM_RADIOS];
} nm_registry_t;

bool nm_uuid_valid(const char *uuid);
bool nm_parse_ssid(const char *ssid, char uuid[NM_UUID_LEN + 1], uint8_t *orientation);
bool nm_link_equal(const nm_link_id_t *a, const nm_link_id_t *b);
bool nm_registry_expire(nm_registry_t *registry, uint64_t now_ms);
bool nm_registry_reserve(nm_registry_t *registry, uint8_t owner,
                         const char *peer_uuid, const nm_link_id_t *link, uint64_t now_ms);
bool nm_registry_advance(nm_registry_t *registry, uint8_t owner,
                         const nm_link_id_t *link, nm_phase_t phase, uint64_t now_ms);
bool nm_registry_release(nm_registry_t *registry, uint8_t owner, const nm_link_id_t *link);
bool nm_registry_owns(const nm_registry_t *registry, uint8_t owner,
                      const nm_link_id_t *link, uint64_t now_ms);
bool nm_registry_blocks(const nm_registry_t *registry, const char *uuid, uint64_t now_ms);

#endif
