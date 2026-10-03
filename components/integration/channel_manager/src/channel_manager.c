#include <string.h>
#include "channel_manager/channel_manager.h"
#include "esp_random.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_err.h"
#include "neighbor_manager/neighbor_manager.h"
#include "os/os.h"

#define CHANNELS 5
#define MAX_PEERS 4
#define MAX_NETWORK_NAME_LENGTH 33
#define FULL_AP_COOLDOWN_MS 30000

static const char *TAG = "channel_manager";

static const uint8_t formation_1[CHANNELS] = {1, 7, 4, 10, 11};
static const uint8_t formation_2[CHANNELS] = {5, 11, 8, 2, 11};

static char full_networks[MAX_PEERS][MAX_NETWORK_NAME_LENGTH];
static uint64_t full_deadlines[MAX_PEERS];
static mutex_t full_lock;
static uint8_t full_network_index = 0; // For keeping track of the current array index

static channel_manager_t channel_manager = { 0 };
static channel_manager_t *cm = &channel_manager;

typedef struct {
    uint8_t orientation;
    uint8_t channels[CHANNELS];
    char network_name[MAX_NETWORK_NAME_LENGTH];
} cm_message_t;

static void on_sibling_message(void *ctx, const uint8_t *msg, uint16_t len) {
    if (len != sizeof(cm_message_t)) {
        ESP_LOGW(TAG, "Ignoring sibling message with wrong length: %d", len);
        return;
    }

    const cm_message_t *packet = (const cm_message_t *)msg;
    // After provision the device has already assigned channels
    if(!node_is_network_provided()) {
        node_disable_sta(); // Disable STA to avoid multiple connections during initial node discovery
        cm->suggested_channel = packet->channels[cm->orientation];
        ESP_LOGI(TAG, "Updated suggested channel=%d", cm->suggested_channel);
    }

}

void cm_init(ring_share_t *rs, node_device_orientation_t orientation) {
    ESP_ERROR_CHECK(mutex_create(&full_lock) ? ESP_OK : ESP_ERR_NO_MEM);
    cm->rs = rs;
    cm->orientation = orientation;
    cm->suggested_channel = formation_1[cm->orientation];

    rs_register_component(
        cm->rs, RS_CHANNEL_MANAGER,
        (ring_callback_t){
            .callback = on_sibling_message,
            .context = cm,
        }
    );

    ESP_LOGI(TAG, "Channel manager initialized");
}

static const uint8_t *select_formation(uint8_t connected_channel) {
    // Check the first 4 channels of formation_1
    for (int i = 0; i < 4; i++) {
        if (formation_1[i] == connected_channel) {
            return formation_2; // swap to the other formation
        }
    }
    // If not in formation_1, use formation_1 explicitly
    return formation_1;
}

bool cm_provide_to_siblings(uint8_t connected_channel,  const char *network_name) {
    if (cm->rs == NULL) {
        ESP_LOGW(TAG, "Channel manager not initialized. Skipping broadcast.");
        return false;
    }

    const uint8_t *selected_formation = select_formation(connected_channel);
    cm->suggested_channel = selected_formation[cm->orientation];

    cm_message_t msg = {0};
    msg.orientation = cm->orientation;
    memcpy(msg.channels, selected_formation, CHANNELS);
    strncpy(msg.network_name, network_name, MAX_NETWORK_NAME_LENGTH - 1);

    bool broadcast = rs_broadcast(cm->rs, RS_CHANNEL_MANAGER, (uint8_t *)&msg, sizeof(msg));
    if (broadcast) {
        ESP_LOGI(TAG, "Provided channels to siblings, connected channel=%d, ssid=%s", connected_channel, network_name);
    }

    return broadcast;
}

void cm_block_full_ap(const char *network_name) {
    char uuid[NM_UUID_LEN + 1];
    if (!nm_parse_ssid(network_name, uuid, NULL)) return;
    WITH_LOCK(&full_lock, {
        strncpy(full_networks[full_network_index], network_name, MAX_NETWORK_NAME_LENGTH - 1);
        full_deadlines[full_network_index] = (uint64_t)esp_timer_get_time() / 1000 + FULL_AP_COOLDOWN_MS;
        full_network_index = (full_network_index + 1) % MAX_PEERS;
    });
}

// Expose the suggested channel
uint8_t cm_get_suggested_channel(void) {
    if (cm->rs == NULL) {
        uint8_t random_channel = (uint8_t)((esp_random() % 11) + 1);
        return random_channel;
    } else {
        return cm->suggested_channel;
    }
}

// Check for blocked UUIDs
bool cm_is_blocked_uuid(const char *network_name) {
    char uuid[NM_UUID_LEN + 1];
    if (!nm_parse_ssid(network_name, uuid, NULL) || nm_is_blocked_uuid(uuid)) return true;
    bool blocked = false;
    uint64_t now = (uint64_t)esp_timer_get_time() / 1000;
    WITH_LOCK(&full_lock, {
        for (int i = 0; i < MAX_PEERS; i++) {
            if (full_deadlines[i] > now && strcmp(network_name, full_networks[i]) == 0)
                blocked = true;
        }
    });
    return blocked;
}
