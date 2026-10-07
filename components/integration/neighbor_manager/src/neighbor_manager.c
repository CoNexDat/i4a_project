#include "neighbor_manager/neighbor_manager.h"

#include <string.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "os/os.h"

#define NM_CENTER NM_RADIOS
#define NM_RPC_MS 3000

typedef enum { NM_RESERVE = 1, NM_COMMIT, NM_ACTIVATE, NM_RENEW, NM_RELEASE, NM_SNAPSHOT } nm_command_t;

typedef struct {
    uint32_t request;
    uint32_t revision;
    uint8_t command;
    uint8_t owner;
    uint8_t accepted;
    char peer_uuid[NM_UUID_LEN + 1];
    nm_link_id_t link;
    nm_registry_t registry;
} nm_message_t;

_Static_assert(sizeof(nm_message_t) <= RS_MAX_BROADCAST_LEN, "Neighbor snapshot exceeds SPI MTU");

static struct {
    ring_share_t *rs;
    mutex_t lock;
    mutex_t rpc_lock;
    nm_registry_t registry;
    char uuid[NM_UUID_LEN + 1];
    uint8_t orientation;
    uint32_t next_request;
    uint32_t revision;
    uint32_t waiting_request;
    bool answered;
    bool accepted;
} manager;

static uint64_t now_ms(void) { return (uint64_t)esp_timer_get_time() / 1000; }

/* Caller holds manager.lock. Deadlines are sent as durations, not center uptime. */
static void snapshot(nm_message_t *msg, uint64_t now) {
    msg->revision = ++manager.revision;
    msg->command = NM_SNAPSHOT;
    msg->registry = manager.registry;
    for (unsigned i = 0; i < NM_RADIOS; ++i) {
        nm_claim_t *claim = &msg->registry.claims[i];
        claim->deadline_ms = claim->deadline_ms > now ? claim->deadline_ms - now : 0;
    }
}

static bool request_valid(const nm_message_t *msg) {
    if (msg->owner >= NM_RADIOS || !nm_uuid_valid(msg->peer_uuid) ||
        !memcmp(msg->peer_uuid, manager.uuid, NM_UUID_LEN + 1))
        return false;
    if (!memcmp(msg->link.initiator_uuid, manager.uuid, NM_UUID_LEN + 1))
        return msg->owner == msg->link.station_orientation;
    return !memcmp(msg->link.initiator_uuid, msg->peer_uuid, NM_UUID_LEN + 1) &&
           msg->owner == msg->link.ap_orientation;
}

static void on_message(void *ctx, const uint8_t *data, uint16_t len) {
    (void)ctx;
    if (len != sizeof(nm_message_t)) return;
    nm_message_t msg;
    memcpy(&msg, data, sizeof(msg));
    uint64_t now = now_ms();
    if (msg.command == NM_SNAPSHOT) {
        if (manager.orientation == NM_CENTER) return;
        WITH_LOCK(&manager.lock, {
            bool fresh = (int32_t)(msg.revision - manager.revision) > 0;
            if (fresh) {
                manager.revision = msg.revision;
                manager.registry = msg.registry;
                for (unsigned i = 0; i < NM_RADIOS; ++i)
                    manager.registry.claims[i].deadline_ms += now;
            }
            if (fresh && msg.owner == manager.orientation && msg.request == manager.waiting_request) {
                manager.accepted = msg.accepted;
                manager.answered = true;
            }
        });
        return;
    }
    if (manager.orientation != NM_CENTER || msg.owner >= NM_RADIOS) return;

    WITH_LOCK(&manager.lock, {
        nm_registry_expire(&manager.registry, now);
        switch (msg.command) {
            case NM_RESERVE:
                msg.accepted = request_valid(&msg) &&
                    nm_registry_reserve(&manager.registry, msg.owner, msg.peer_uuid, &msg.link, now);
                break;
            case NM_COMMIT:
                msg.accepted = nm_registry_advance(&manager.registry, msg.owner, &msg.link, NM_COMMITTED, now);
                break;
            case NM_ACTIVATE:
            case NM_RENEW:
                msg.accepted = nm_registry_advance(&manager.registry, msg.owner, &msg.link, NM_ACTIVE, now);
                break;
            case NM_RELEASE:
                msg.accepted = nm_registry_release(&manager.registry, msg.owner, &msg.link);
                break;
            default:
                msg.accepted = false;
                break;
        }
        snapshot(&msg, now);
    });
    rs_broadcast(manager.rs, RS_NEIGHBOR_MANAGER, &msg, sizeof(msg));
}

bool nm_init(ring_share_t *rs, uint8_t orientation, const char *node_uuid) {
    if (!rs || orientation > NM_CENTER || !nm_uuid_valid(node_uuid)) return false;
    memset(&manager, 0, sizeof(manager));
    if (!mutex_create(&manager.lock)) return false;
    if (!mutex_create(&manager.rpc_lock)) {
        mutex_destroy(&manager.lock);
        return false;
    }
    manager.rs = rs;
    manager.orientation = orientation;
    memcpy(manager.uuid, node_uuid, NM_UUID_LEN + 1);
    rs_register_component(rs, RS_NEIGHBOR_MANAGER,
                          (ring_callback_t){ .callback = on_message, .context = NULL });
    return true;
}

/* Do not call from a sibling callback: that task delivers the RPC response. */
static bool rpc(nm_command_t command, const char *peer_uuid, const nm_link_id_t *link) {
    if (!manager.rs || manager.orientation >= NM_RADIOS) return false;
    if (!link || (command == NM_RESERVE && !nm_uuid_valid(peer_uuid))) return false;
    bool accepted = false;
    mutex_lock(&manager.rpc_lock);
    nm_message_t msg = { .command = command, .owner = manager.orientation, .link = *link };
    if (peer_uuid) memcpy(msg.peer_uuid, peer_uuid, NM_UUID_LEN + 1);
    WITH_LOCK(&manager.lock, {
        if (++manager.next_request == 0) ++manager.next_request;
        msg.request = manager.next_request;
        manager.waiting_request = msg.request;
        manager.answered = false;
    });
    uint64_t deadline = now_ms() + NM_RPC_MS;
    if (rs_broadcast(manager.rs, RS_NEIGHBOR_MANAGER, &msg, sizeof(msg))) {
        while (now_ms() < deadline) {
            bool answered;
            WITH_LOCK(&manager.lock, {
                answered = manager.answered;
                accepted = answered && manager.accepted;
            });
            if (answered) break;
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    WITH_LOCK(&manager.lock, { manager.waiting_request = 0; });
    mutex_unlock(&manager.rpc_lock);
    return accepted;
}

bool nm_reserve(const char *peer_uuid, const nm_link_id_t *link) { return rpc(NM_RESERVE, peer_uuid, link); }
bool nm_commit(const nm_link_id_t *link) { return rpc(NM_COMMIT, NULL, link); }
bool nm_activate(const nm_link_id_t *link) { return rpc(NM_ACTIVATE, NULL, link); }
bool nm_renew(const nm_link_id_t *link) { return rpc(NM_RENEW, NULL, link); }
void nm_release(const nm_link_id_t *link) { (void)rpc(NM_RELEASE, NULL, link); }

bool nm_owns(const nm_link_id_t *link) {
    bool owned;
    if (!manager.rs) return false;
    WITH_LOCK(&manager.lock, {
        owned = nm_registry_owns(&manager.registry, manager.orientation, link, now_ms());
    });
    return owned;
}

bool nm_is_blocked_uuid(const char *uuid) {
    bool blocked;
    if (!manager.rs) return true;
    WITH_LOCK(&manager.lock, { blocked = nm_registry_blocks(&manager.registry, uuid, now_ms()); });
    return blocked;
}

void nm_tick(void) {
    if (!manager.rs || manager.orientation != NM_CENTER) return;
    nm_message_t msg = { .owner = NM_CENTER };
    bool changed;
    WITH_LOCK(&manager.lock, {
        uint64_t now = now_ms();
        changed = nm_registry_expire(&manager.registry, now);
        snapshot(&msg, now);
    });
    if (changed) rs_broadcast(manager.rs, RS_NEIGHBOR_MANAGER, &msg, sizeof(msg));
}
