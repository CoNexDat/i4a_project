#include "../../../components/integration/channel_manager/src/channel_manager.c"

#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT
#endif
#define CHECK(expr) do { if (!(expr)) return __LINE__; } while (0)

static uint64_t clock_ms;
static nm_registry_t registry;
static ring_share_t ring;
bool mutex_create(mutex_t *m) { *m = (void *)1; return true; }
bool mutex_lock(mutex_t *m) { return m && *m == (void *)1; }
bool mutex_unlock(mutex_t *m) { return m && *m == (void *)1; }
int64_t esp_timer_get_time(void) { return clock_ms * 1000; }
uint32_t esp_random(void) { return 1; }
bool node_is_network_provided(void) { return true; }
void node_disable_sta(void) { }
bool nm_is_blocked_uuid(const char *uuid) { return nm_registry_blocks(&registry, uuid, clock_ms); }
void rs_register_component(ring_share_t *rs, int component, ring_callback_t callback) {
    (void)rs; (void)component; (void)callback;
}
bool rs_broadcast(ring_share_t *rs, int component, const void *msg, uint16_t len) {
    (void)rs; (void)component; (void)msg; (void)len; return true;
}

EXPORT int test_scan_cooldown_and_channel_independence(void) {
    memset(&registry, 0, sizeof(registry));
    memset(full_networks, 0, sizeof(full_networks));
    memset(full_deadlines, 0, sizeof(full_deadlines));
    full_network_index = 0;
    clock_ms = 0;
    cm_init(&ring, 0);
    const char *north = "I4A_N_B00000000000", *south = "I4A_S_B00000000000";
    CHECK(!cm_is_blocked_uuid(north));
    cm_block_full_ap(north);
    CHECK(cm_is_blocked_uuid(north) && !cm_is_blocked_uuid(south));
    clock_ms = FULL_AP_COOLDOWN_MS;
    CHECK(!cm_is_blocked_uuid(north));
    CHECK(cm_is_blocked_uuid("bad"));
    CHECK(cm_provide_to_siblings(1, north));
    CHECK(!cm_is_blocked_uuid(north));
    cm_message_t channels = {0};
    memcpy(channels.network_name, north, strlen(north) + 1);
    on_sibling_message(NULL, (const uint8_t *)&channels, sizeof(channels));
    CHECK(!cm_is_blocked_uuid(north));
    nm_link_id_t link = { .initiator_uuid = "A00000000000", .station_orientation = 1,
                         .ap_orientation = 0, .session = 1 };
    CHECK(nm_registry_reserve(&registry, 1, "B00000000000", &link, clock_ms));
    CHECK(cm_is_blocked_uuid(north) && cm_is_blocked_uuid(south));
    CHECK(nm_registry_release(&registry, 1, &link));
    CHECK(!cm_is_blocked_uuid(north) && !cm_is_blocked_uuid(south));
    return 0;
}
