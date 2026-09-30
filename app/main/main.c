#include "esp_log.h"
#include <inttypes.h>
#include "lwip/esp_netif_net_stack.h"
#include "esp_netif_net_stack.h"
#include "wireless/wireless.h"
#include "siblings/siblings.h"
#include "ring_share/ring_share.h"
#include "shared_state/shared_state.h"
#include "sync/sync.h"
#include "routing_config/routing_config.h"
#include "routing/routing.h"
#include "internal_messages.h"
#include "routing_hooks.h"
#include "callbacks.h"
#include "task_config.h"
#include "info_manager/info_manager.h"
#include "node.h"
#include "node_ota/node_ota.h"

#define ROOT_NETWORK 0x0A000000  // 10.0.0.0
#define ROOT_MASK 0xFF000000 // 255.0.0.0

#define ROUTING_ORIENTATION_OFFSET 1

static const char *TAG = "main";

static sync_t _sync = { 0 };
static shared_state_t ss = { 0 };

struct netif *custom_ip4_route_src_hook(const ip4_addr_t *src, const ip4_addr_t *dest) {
    uint32_t src_ip = lwip_ntohl(ip4_addr_get_u32(src));
    uint32_t dst_ip = lwip_ntohl(ip4_addr_get_u32(dest));
    return node_do_routing(src_ip, dst_ip);
}

void routing_task(void *pvParameters) {
    routing_t *rt = (routing_t *)pvParameters;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        rt_on_tick(rt, 1000);
    }
}

void app_main(void) {
    node_ota_watch_boot();
    node_ota_boot_trace("node_setup.begin");
    node_setup();
    node_ota_boot_trace("node_setup.end");
    node_ota_boot_trace("serial.begin");
    ESP_ERROR_CHECK(node_ota_start_serial());
    node_ota_boot_trace("serial.end");

    wireless_t *wl = node_get_wireless_instance();
    ring_share_t *rs = node_get_rs_instance();
    routing_t *rt = node_get_rt_instance();
    node_device_orientation_t orientation = node_get_device_orientation();
    bool is_center_root = node_is_device_center_root();

    sync_init(&_sync, rs, orientation + ROUTING_ORIENTATION_OFFSET);
    ss_init(&ss, &_sync, rs, orientation + ROUTING_ORIENTATION_OFFSET);
    rt_create(rt, rs, wl, &_sync, &ss, orientation + ROUTING_ORIENTATION_OFFSET);

    if(orientation == NODE_DEVICE_ORIENTATION_CENTER){
        if(is_center_root){
            node_set_routing_hook(ROUTING_HOOK_ROOT_CENTER);
            rt_init_root(rt, ROOT_NETWORK, ROOT_MASK);
        } else {
            node_set_routing_hook(ROUTING_HOOK_HOME);
            rt_init_home(rt);
        }
    } else {
        node_set_routing_hook(ROUTING_HOOK_FORWARDER);
        rt_init_forwarder(rt);
    }
    
    node_ota_boot_trace("rt_on_start.begin");
    rt_on_start(rt);
    node_ota_boot_trace("rt_on_start.end");
    node_ota_boot_trace("rt_on_tick.begin");
    rt_on_tick(rt, 1);
    node_ota_boot_trace("rt_on_tick.end");

    if(orientation == NODE_DEVICE_ORIENTATION_CENTER && is_center_root){
        node_ota_boot_trace("wifi_ap.begin");
        node_set_as_ap(ROOT_NETWORK, ROOT_MASK);
        node_ota_boot_trace("wifi_ap.end");
    }

    if(orientation != NODE_DEVICE_ORIENTATION_CENTER && !is_center_root){
        node_ota_boot_trace("wifi_sta.begin");
        node_set_as_sta();
        node_ota_boot_trace("wifi_sta.end");
    }
    
    node_ota_boot_trace("routing_task.begin");
    BaseType_t routing_started = xTaskCreatePinnedToCore(
        routing_task,
        "routing_task",
        TASK_ROUTING_STACK,
        rt,
        TASK_ROUTING_PRIORITY,
        NULL,
        TASK_ROUTING_CORE
    );
    ESP_ERROR_CHECK(routing_started == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    node_ota_boot_trace("routing_task.end");
    ESP_ERROR_CHECK(node_ota_confirm_boot());
    node_ota_boot_trace("app_main.ready");

}
