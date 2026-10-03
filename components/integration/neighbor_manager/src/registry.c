#include "neighbor_manager/registry.h"

#include <string.h>

bool nm_uuid_valid(const char *uuid) {
    if (!uuid) return false;
    for (unsigned i = 0; i < NM_UUID_LEN; ++i) {
        if (!((uuid[i] >= '0' && uuid[i] <= '9') || (uuid[i] >= 'A' && uuid[i] <= 'F')))
            return false;
    }
    return uuid[NM_UUID_LEN] == '\0';
}

bool nm_parse_ssid(const char *ssid, char uuid[NM_UUID_LEN + 1], uint8_t *orientation) {
    if (!ssid || strlen(ssid) != 6 + NM_UUID_LEN || memcmp(ssid, "I4A_", 4) || ssid[5] != '_')
        return false;
    const char *directions = "NSEW";
    const char *direction = strchr(directions, ssid[4]);
    if (!direction) return false;
    memcpy(uuid, ssid + 6, NM_UUID_LEN);
    uuid[NM_UUID_LEN] = '\0';
    if (!nm_uuid_valid(uuid)) return false;
    if (orientation) *orientation = (uint8_t)(direction - directions);
    return true;
}

bool nm_link_equal(const nm_link_id_t *a, const nm_link_id_t *b) {
    return a->session == b->session &&
           a->station_orientation == b->station_orientation &&
           a->ap_orientation == b->ap_orientation &&
           memcmp(a->initiator_uuid, b->initiator_uuid, NM_UUID_LEN + 1) == 0;
}

static int link_compare(const nm_link_id_t *a, const nm_link_id_t *b) {
    int uuid_order = memcmp(a->initiator_uuid, b->initiator_uuid, NM_UUID_LEN);
    if (uuid_order) return uuid_order;
    if (a->station_orientation != b->station_orientation)
        return a->station_orientation < b->station_orientation ? -1 : 1;
    if (a->ap_orientation != b->ap_orientation)
        return a->ap_orientation < b->ap_orientation ? -1 : 1;
    return a->session < b->session ? -1 : a->session != b->session;
}

bool nm_registry_expire(nm_registry_t *registry, uint64_t now_ms) {
    bool changed = false;
    for (unsigned i = 0; i < NM_RADIOS; ++i) {
        nm_claim_t *claim = &registry->claims[i];
        if (claim->phase != NM_FREE && claim->deadline_ms <= now_ms) {
            memset(claim, 0, sizeof(*claim));
            changed = true;
        }
    }
    return changed;
}

bool nm_registry_owns(const nm_registry_t *registry, uint8_t owner,
                      const nm_link_id_t *link, uint64_t now_ms) {
    return owner < NM_RADIOS && registry->claims[owner].phase != NM_FREE &&
           registry->claims[owner].deadline_ms > now_ms &&
           nm_link_equal(&registry->claims[owner].link, link);
}

bool nm_registry_blocks(const nm_registry_t *registry, const char *uuid, uint64_t now_ms) {
    if (!nm_uuid_valid(uuid)) return true;
    for (unsigned i = 0; i < NM_RADIOS; ++i) {
        const nm_claim_t *claim = &registry->claims[i];
        if (claim->phase != NM_FREE && claim->deadline_ms > now_ms &&
            memcmp(claim->peer_uuid, uuid, NM_UUID_LEN + 1) == 0)
            return true;
    }
    return false;
}

bool nm_registry_reserve(nm_registry_t *registry, uint8_t owner,
                         const char *peer_uuid, const nm_link_id_t *link, uint64_t now_ms) {
    if (owner >= NM_RADIOS || !nm_uuid_valid(peer_uuid) || !nm_uuid_valid(link->initiator_uuid) ||
        !link->session || link->station_orientation >= NM_RADIOS || link->ap_orientation >= NM_RADIOS)
        return false;

    nm_registry_expire(registry, now_ms);
    nm_claim_t *slot = &registry->claims[owner];
    if (slot->phase != NM_FREE) {
        /* Retransmission is harmless; an old session cannot replace its successor. */
        return nm_link_equal(&slot->link, link) &&
               memcmp(slot->peer_uuid, peer_uuid, NM_UUID_LEN + 1) == 0;
    }

    int duplicate = -1;
    for (unsigned i = 0; i < NM_RADIOS; ++i) {
        nm_claim_t *claim = &registry->claims[i];
        if (claim->phase == NM_FREE || memcmp(claim->peer_uuid, peer_uuid, NM_UUID_LEN + 1))
            continue;
        /* Only pending reservations can be displaced. Once committed, keep the link. */
        if (claim->phase != NM_RESERVED || link_compare(link, &claim->link) >= 0)
            return false;
        duplicate = (int)i;
    }
    if (duplicate >= 0) memset(&registry->claims[duplicate], 0, sizeof(nm_claim_t));
    memset(slot, 0, sizeof(*slot));
    memcpy(slot->peer_uuid, peer_uuid, NM_UUID_LEN + 1);
    slot->link = *link;
    slot->phase = NM_RESERVED;
    slot->deadline_ms = now_ms + NM_PENDING_MS;
    return true;
}

bool nm_registry_advance(nm_registry_t *registry, uint8_t owner,
                         const nm_link_id_t *link, nm_phase_t phase, uint64_t now_ms) {
    if (!nm_registry_owns(registry, owner, link, now_ms)) return false;
    nm_claim_t *claim = &registry->claims[owner];
    if (phase < NM_COMMITTED || phase > NM_ACTIVE || phase < claim->phase ||
        phase > claim->phase + 1)
        return false;
    claim->phase = phase;
    claim->deadline_ms = now_ms + (phase == NM_ACTIVE ? NM_ACTIVE_MS : NM_PENDING_MS);
    return true;
}

bool nm_registry_release(nm_registry_t *registry, uint8_t owner, const nm_link_id_t *link) {
    if (owner >= NM_RADIOS || registry->claims[owner].phase == NM_FREE ||
        !nm_link_equal(&registry->claims[owner].link, link))
        return false;
    memset(&registry->claims[owner], 0, sizeof(nm_claim_t));
    return true;
}
