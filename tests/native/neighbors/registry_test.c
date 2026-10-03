#include "neighbor_manager/registry.h"
#include <string.h>

#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT
#endif
#define CHECK(expr) do { if (!(expr)) return __LINE__; } while (0)

static const char *A = "A00000000000", *B = "B00000000000", *C = "C00000000000";

static nm_link_id_t link_id(const char *uuid, uint8_t sta, uint8_t ap, uint32_t session) {
    nm_link_id_t link = { .session = session, .station_orientation = sta, .ap_orientation = ap };
    memcpy(link.initiator_uuid, uuid, NM_UUID_LEN + 1);
    return link;
}

static bool establish(nm_registry_t *registry, uint8_t owner, const char *peer, const nm_link_id_t *link) {
    return nm_registry_reserve(registry, owner, peer, link, 0) &&
           nm_registry_advance(registry, owner, link, NM_COMMITTED, 1) &&
           nm_registry_advance(registry, owner, link, NM_ACTIVE, 2);
}

EXPORT int test_symmetry_and_other_neighbors(void) {
    nm_registry_t a = {0}, b = {0};
    nm_link_id_t original = link_id(A, 0, 1, 1);
    CHECK(establish(&a, 0, B, &original));
    CHECK(establish(&b, 1, A, &original));
    CHECK(nm_registry_blocks(&a, B, 3) && nm_registry_blocks(&b, A, 3));
    for (unsigned i = 0; i < NM_RADIOS; ++i) {
        nm_link_id_t inverse = link_id(B, i, 2, 2);
        CHECK(!nm_registry_reserve(&b, i, A, &inverse, 3));
        CHECK(!nm_registry_reserve(&a, 2, B, &inverse, 3));
    }
    nm_link_id_t third = link_id(C, 2, 3, 3);
    CHECK(establish(&a, 3, C, &third));
    CHECK(nm_registry_owns(&a, 0, &original, 4));
    CHECK(nm_registry_owns(&a, 3, &third, 4));
    CHECK(nm_registry_release(&a, 0, &original));
    CHECK(nm_registry_release(&b, 1, &original));
    CHECK(!nm_registry_blocks(&a, B, 4) && !nm_registry_blocks(&b, A, 4));
    return 0;
}

EXPORT int test_pending_tie_break_and_confirmed_priority(void) {
    nm_registry_t a = {0}, b = {0};
    nm_link_id_t preferred = link_id(A, 0, 1, 20), inverse = link_id(B, 2, 3, 10);
    CHECK(nm_registry_reserve(&a, 3, B, &inverse, 0));
    CHECK(nm_registry_reserve(&b, 1, A, &preferred, 0));
    CHECK(nm_registry_reserve(&a, 0, B, &preferred, 1));
    CHECK(!nm_registry_reserve(&b, 2, A, &inverse, 1));
    CHECK(!nm_registry_owns(&a, 3, &inverse, 2));
    CHECK(!nm_registry_release(&a, 3, &inverse));
    CHECK(nm_registry_owns(&a, 0, &preferred, 2));
    memset(&a, 0, sizeof(a));
    CHECK(establish(&a, 3, B, &inverse));
    CHECK(!nm_registry_reserve(&a, 0, B, &preferred, 3));
    CHECK(nm_registry_owns(&a, 3, &inverse, 4));
    return 0;
}

EXPORT int test_expiry_renewal_and_stale_sessions(void) {
    nm_registry_t registry = {0};
    nm_link_id_t old = link_id(A, 0, 1, 1), next = link_id(A, 0, 1, 2);
    CHECK(nm_registry_reserve(&registry, 0, B, &old, 0));
    CHECK(!nm_registry_advance(&registry, 0, &old, NM_ACTIVE, 1));
    CHECK(!nm_registry_owns(&registry, 0, &old, NM_PENDING_MS));
    CHECK(nm_registry_reserve(&registry, 0, B, &next, NM_PENDING_MS));
    CHECK(!nm_registry_release(&registry, 0, &old));
    CHECK(!nm_registry_advance(&registry, 0, &old, NM_COMMITTED, NM_PENDING_MS + 1));
    CHECK(nm_registry_advance(&registry, 0, &next, NM_COMMITTED, NM_PENDING_MS + 1));
    CHECK(nm_registry_advance(&registry, 0, &next, NM_ACTIVE, NM_PENDING_MS + 2));
    CHECK(nm_registry_advance(&registry, 0, &next, NM_ACTIVE, NM_PENDING_MS + NM_ACTIVE_MS));
    CHECK(!nm_registry_expire(&registry, NM_PENDING_MS + NM_ACTIVE_MS + 1));
    CHECK(nm_registry_expire(&registry, NM_PENDING_MS + 2 * NM_ACTIVE_MS));
    CHECK(!nm_registry_blocks(&registry, B, NM_PENDING_MS + 2 * NM_ACTIVE_MS));
    CHECK(!nm_registry_advance(&registry, 0, &next, NM_ACTIVE, NM_PENDING_MS + 2 * NM_ACTIVE_MS));
    return 0;
}

EXPORT int test_identity_validation(void) {
    char uuid[NM_UUID_LEN + 1];
    uint8_t orientation;
    CHECK(nm_parse_ssid("I4A_N_A00000000000", uuid, &orientation) && orientation == 0);
    CHECK(!strcmp(uuid, A));
    CHECK(nm_parse_ssid("I4A_W_000000000000", uuid, &orientation) && orientation == 3);
    CHECK(!nm_parse_ssid("I4A_N_A0000000000", uuid, NULL));
    CHECK(!nm_parse_ssid("I4A_N_A000000000000", uuid, NULL));
    CHECK(!nm_parse_ssid("I4A_X_A00000000000", uuid, NULL));
    CHECK(!nm_parse_ssid("xxI4A_N_A00000000000", uuid, NULL));
    CHECK(!nm_parse_ssid("I4A_N_a00000000000", uuid, NULL));
    CHECK(!nm_uuid_valid("bad") && !nm_uuid_valid(NULL));
    nm_registry_t registry = {0};
    nm_link_id_t invalid = link_id(A, 4, 1, 1);
    CHECK(!nm_registry_reserve(&registry, 0, B, &invalid, 0));
    invalid = link_id(A, 0, 1, 0);
    CHECK(!nm_registry_reserve(&registry, 0, B, &invalid, 0));
    return 0;
}

typedef struct {
    nm_registry_t node[2];
    unsigned progress[2][2];
    bool failed[2];
} race_t;

static unsigned explored;
static const uint8_t owners[2][2] = { {0, 1}, {3, 2} };

/* Explore every endpoint ordering with reserve/commit/activate barriers. */
static int explore(race_t state, const nm_link_id_t links[2], unsigned depth) {
    for (unsigned link = 0; link < 2; ++link) {
        if (state.failed[link]) continue;
        for (unsigned node = 0; node < 2; ++node) {
            unsigned phase = state.progress[link][node];
            if (phase >= 3 || state.progress[link][1 - node] < phase) continue;
            race_t next = state;
            bool accepted = phase == 0 ?
                nm_registry_reserve(&next.node[node], owners[link][node], node ? A : B, &links[link], depth) :
                nm_registry_advance(&next.node[node], owners[link][node], &links[link],
                                    phase == 1 ? NM_COMMITTED : NM_ACTIVE, depth);
            if (!accepted) {
                next.failed[link] = true;
                for (unsigned n = 0; n < 2; ++n)
                    nm_registry_release(&next.node[n], owners[link][n], &links[link]);
            } else ++next.progress[link][node];
            int result = explore(next, links, depth + 1);
            if (result) return result;
        }
    }
    unsigned admitted = 0;
    for (unsigned link = 0; link < 2; ++link) {
        if (!state.failed[link] && state.progress[link][0] == 3 && state.progress[link][1] == 3)
            ++admitted;
    }
    CHECK(admitted <= 1);
    ++explored;
    return 0;
}

EXPORT int test_all_simultaneous_endpoint_orders(void) {
    race_t initial = {0};
    nm_link_id_t links[2] = { link_id(A, 0, 1, 1), link_id(B, 2, 3, 2) };
    explored = 0;
    int result = explore(initial, links, 0);
    if (result) return result;
    CHECK(explored > 100);
    return 0;
}
