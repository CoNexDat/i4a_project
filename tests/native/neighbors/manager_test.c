/* Exercise the real coordinator/RPC code with five independent ESP contexts. */
#ifdef __TINYC__
#define _Static_assert(condition, message) typedef char native_assert[(condition) ? 1 : -1]
#endif
#include "../../../components/integration/neighbor_manager/src/neighbor_manager.c"

#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT
#endif
#define CHECK(expr) do { if (!(expr)) return __LINE__; } while (0)

typedef __typeof__(manager) manager_state_t;
static manager_state_t states[5];
static ring_share_t rings[5];
static int current;
static uint64_t clock_ms;
static bool drop_requests, drop_snapshots;
static nm_message_t last_snapshot;

static void switch_to(int next) {
    states[current] = manager;
    manager = states[next];
    current = next;
}

bool mutex_create(mutex_t *m) { *m = (void *)1; return true; }
bool mutex_lock(mutex_t *m) { return m && *m == (void *)1; }
bool mutex_unlock(mutex_t *m) { return m && *m == (void *)1; }
void mutex_destroy(mutex_t *m) { *m = NULL; }
int64_t esp_timer_get_time(void) { return (clock_ms + current * 100000) * 1000; }
void vTaskDelay(TickType_t ticks) { clock_ms += ticks; }
void rs_register_component(ring_share_t *rs, int component, ring_callback_t callback) {
    (void)rs; (void)component; (void)callback;
}

bool rs_broadcast(ring_share_t *rs, int component, const void *data, uint16_t len) {
    (void)rs; (void)component;
    int caller = current;
    if (caller != NM_CENTER) {
        if (drop_requests) return false;
        switch_to(NM_CENTER);
        on_message(NULL, data, len);
    } else {
        memcpy(&last_snapshot, data, len);
        if (!drop_snapshots) {
            for (int radio = 0; radio < NM_RADIOS; ++radio) {
                switch_to(radio);
                on_message(NULL, data, len);
                switch_to(NM_CENTER);
            }
        }
    }
    switch_to(caller);
    return true;
}

static void reset(void) {
    memset(states, 0, sizeof(states));
    memset(&manager, 0, sizeof(manager));
    current = 0;
    clock_ms = 0;
    drop_requests = drop_snapshots = false;
    for (int radio = 0; radio <= NM_CENTER; ++radio) {
        nm_init(&rings[radio], radio, "A00000000000");
        states[radio] = manager;
    }
    current = NM_CENTER;
}

static nm_link_id_t new_link(uint8_t sta, uint8_t ap, uint32_t token) {
    nm_link_id_t link = { .initiator_uuid = "A00000000000", .station_orientation = sta,
                         .ap_orientation = ap, .session = token };
    return link;
}

EXPORT int test_spi_reservations_snapshots_and_release(void) {
    reset();
    nm_link_id_t first = new_link(0, 1, 1), other = new_link(2, 3, 2);
    switch_to(0);
    CHECK(nm_reserve("B00000000000", &first));
    CHECK(nm_commit(&first) && nm_activate(&first));
    nm_message_t stale = last_snapshot;
    for (int radio = 0; radio < NM_RADIOS; ++radio) {
        switch_to(radio);
        CHECK(nm_is_blocked_uuid("B00000000000"));
        CHECK(!nm_is_blocked_uuid("C00000000000"));
    }
    switch_to(2);
    CHECK(!nm_reserve("B00000000000", &other));
    CHECK(nm_reserve("C00000000000", &other));
    CHECK(nm_commit(&other) && nm_activate(&other));
    switch_to(0);
    nm_release(&first);
    CHECK(!nm_is_blocked_uuid("B00000000000"));
    on_message(NULL, (const uint8_t *)&stale, sizeof(stale));
    CHECK(!nm_is_blocked_uuid("B00000000000"));
    CHECK(nm_is_blocked_uuid("C00000000000"));
    CHECK(nm_reserve("B00000000000", &first));
    CHECK(nm_commit(&first) && nm_activate(&first));
    clock_ms += 5000;
    CHECK(nm_renew(&first));
    CHECK(nm_owns(&first));
    return 0;
}

EXPORT int test_spi_loss_expiry_and_stale_cleanup(void) {
    reset();
    nm_link_id_t old = new_link(0, 1, 1), next = new_link(0, 1, 2);
    switch_to(0);
    drop_snapshots = true;
    CHECK(!nm_reserve("B00000000000", &old));
    CHECK(clock_ms >= NM_RPC_MS);
    CHECK(!nm_owns(&old));
    drop_snapshots = false;
    clock_ms += NM_PENDING_MS;
    switch_to(NM_CENTER);
    nm_tick();
    switch_to(0);
    CHECK(!nm_is_blocked_uuid("B00000000000"));
    CHECK(nm_reserve("B00000000000", &next));
    CHECK(nm_commit(&next) && nm_activate(&next));
    nm_release(&old);
    CHECK(nm_owns(&next));
    drop_requests = true;
    CHECK(!nm_renew(&next));
    drop_requests = false;
    clock_ms += NM_ACTIVE_MS;
    switch_to(NM_CENTER);
    nm_tick();
    switch_to(0);
    CHECK(!nm_owns(&next) && !nm_is_blocked_uuid("B00000000000"));
    return 0;
}

EXPORT int test_spi_incoming_identity_and_invalid_owner(void) {
    reset();
    nm_link_id_t incoming = { .initiator_uuid = "B00000000000", .station_orientation = 0,
                             .ap_orientation = 1, .session = 1 };
    switch_to(1);
    CHECK(nm_reserve("B00000000000", &incoming));
    CHECK(nm_commit(&incoming) && nm_activate(&incoming));
    switch_to(0);
    nm_link_id_t wrong = new_link(3, 1, 2);
    CHECK(!nm_reserve("C00000000000", &wrong));
    wrong = new_link(0, 1, 3);
    CHECK(!nm_reserve("A00000000000", &wrong));
    CHECK(!nm_reserve("bad", &wrong));
    CHECK(nm_is_blocked_uuid("B00000000000"));
    return 0;
}
