#ifndef I4A_NEIGHBOR_MANAGER_H
#define I4A_NEIGHBOR_MANAGER_H

#include "neighbor_manager/registry.h"
#include "ring_share/ring_share.h"

/* The center is the only writer; all radios retain snapshots for scan filtering. */
bool nm_init(ring_share_t *rs, uint8_t orientation, const char *node_uuid);
bool nm_reserve(const char *peer_uuid, const nm_link_id_t *link);
bool nm_commit(const nm_link_id_t *link);
bool nm_activate(const nm_link_id_t *link);
bool nm_renew(const nm_link_id_t *link);
void nm_release(const nm_link_id_t *link);
bool nm_owns(const nm_link_id_t *link);
bool nm_is_blocked_uuid(const char *uuid);
void nm_tick(void);

#endif
