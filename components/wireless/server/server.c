#include "esp_netif.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "callbacks.h"
#include "task_config.h"
#include "server.h"
#include "peer_session.h"
#include "node.h"

#define PORT 3999

static const char *LOGGING_TAG = "tcp_server";
static volatile bool server_is_up;
static TaskHandle_t volatile server_task;
static peer_session_t session;
static bool session_initialized;

static bool get_network(uint32_t *net, uint32_t *mask) {
    esp_netif_ip_info_t info;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (!netif || esp_netif_get_ip_info(netif, &info) != ESP_OK) return false;
    *net = ntohl(info.ip.addr & info.netmask.addr);
    *mask = ntohl(info.netmask.addr);
    return true;
}

static void socket_read_loop(int sock, uint32_t net, uint32_t mask) {
    bool admitted = peer_session_open(&session, sock, false, NULL);
    if (admitted && server_is_up) {
        node_on_peer_connected(net, mask, PEER_SERVER);
        uint8_t buffer[PEER_SESSION_MAX_MESSAGE];
        while (server_is_up) {
            int len = peer_session_receive(&session, buffer, sizeof(buffer));
            if (len <= 0) break;
            node_on_peer_message(buffer, len);
        }
        peer_session_close(&session);
        node_on_peer_lost(net, mask, PEER_SERVER);
    } else {
        peer_session_close(&session);
        if (server_is_up) {
            ESP_LOGW(LOGGING_TAG, "Peer admission rejected; releasing AP association");
            node_reject_wireless_peer();
        }
    }
}

static void tcp_server_task(void *arg) {
    (void)arg;
    uint32_t net, mask;
    int listener = -1;
    if (!get_network(&net, &mask)) goto stopped;
    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listener < 0) goto stopped;
    struct sockaddr_in address = {
        .sin_family = AF_INET, .sin_port = htons(PORT), .sin_addr.s_addr = htonl(INADDR_ANY)
    };
    int reuse = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) || listen(listener, 1)) goto stopped;
    while (server_is_up) {
        fd_set readers;
        FD_ZERO(&readers);
        FD_SET(listener, &readers);
        struct timeval timeout = { .tv_sec = 0, .tv_usec = 200000 };
        if (lwip_select(listener + 1, &readers, NULL, NULL, &timeout) <= 0) continue;
        int sock = accept(listener, NULL, NULL);
        if (sock < 0) continue;
        socket_read_loop(sock, net, mask);
        shutdown(sock, SHUT_RDWR);
        close(sock);
    }
stopped:
    if (listener >= 0) close(listener);
    server_is_up = false;
    server_task = NULL;
    vTaskDelete(NULL);
}

void server_wait_stopped(void) {
    if (server_task == xTaskGetCurrentTaskHandle()) return;
    while (server_task) vTaskDelay(pdMS_TO_TICKS(10));
}

void server_create(void) {
    if (server_is_up) return;
    server_wait_stopped();
    if (!session_initialized) {
        ESP_ERROR_CHECK(peer_session_init(&session) ? ESP_OK : ESP_ERR_NO_MEM);
        session_initialized = true;
    }
    peer_session_start(&session);
    server_is_up = true;
    ESP_ERROR_CHECK(xTaskCreatePinnedToCore(tcp_server_task, "tcp_server", TASK_SERVER_STACK,
        NULL, TASK_SERVER_PRIORITY, (TaskHandle_t *)&server_task, TASK_SERVER_CORE) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}

void server_close(void) {
    server_is_up = false;
    if (session_initialized) peer_session_shutdown(&session);
}

bool server_send_message(const uint8_t *msg, uint16_t len) {
    return session_initialized && peer_session_send(&session, msg, len);
}
