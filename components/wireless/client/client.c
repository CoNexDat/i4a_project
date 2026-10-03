#include "esp_netif.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "callbacks.h"
#include "task_config.h"
#include "client.h"
#include "peer_session.h"
#include "channel_manager/channel_manager.h"
#include "node.h"
#include <fcntl.h>
#include <errno.h>

#define SERVER_PORT 3999
#define RETRY_DELAY_MS 5000

static const char *LOGGING_TAG = "tcp_client";
static volatile bool sta_is_up;
static TaskHandle_t volatile client_task;
static peer_session_t session;
static bool session_initialized;

static bool get_network(char *gateway, size_t capacity, uint32_t *net, uint32_t *mask) {
    esp_netif_ip_info_t info;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif || esp_netif_get_ip_info(netif, &info) != ESP_OK) return false;
    inet_ntoa_r(info.gw, gateway, capacity);
    *net = ntohl(info.ip.addr & info.netmask.addr);
    *mask = ntohl(info.netmask.addr);
    return true;
}

static bool connect_gateway(int sock, const struct sockaddr_in *address) {
    int flags = lwip_fcntl(sock, F_GETFL, 0);
    if (flags < 0 || lwip_fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) return false;
    bool connected = connect(sock, (const struct sockaddr *)address, sizeof(*address)) == 0;
    if (!connected && errno != EINPROGRESS) return false;
    TickType_t began = xTaskGetTickCount();
    while (!connected && sta_is_up && xTaskGetTickCount() - began < pdMS_TO_TICKS(5000)) {
        fd_set writers;
        FD_ZERO(&writers);
        FD_SET(sock, &writers);
        struct timeval timeout = { .tv_sec = 0, .tv_usec = 200000 };
        int ready = lwip_select(sock + 1, NULL, &writers, NULL, &timeout);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) break;
        if (!ready) continue;
        int error = 0;
        socklen_t size = sizeof(error);
        connected = getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &size) == 0 && error == 0;
        break;
    }
    return connected && sta_is_up && lwip_fcntl(sock, F_SETFL, flags) == 0;
}

static bool socket_read_loop(int sock, uint32_t net, uint32_t mask) {
    bool connected = false;
    bool admitted = peer_session_open(&session, sock, true, node_get_link_name());
    if (admitted && sta_is_up) {
        /* Channel provisioning only follows admission. */
        cm_provide_to_siblings(node_get_device_channel(), node_get_link_name());
        uint8_t buffer[PEER_SESSION_MAX_MESSAGE];
        while (sta_is_up) {
            int len = peer_session_receive(&session, buffer, sizeof(buffer));
            if (len <= 0) break;
            /* The first AP handshake provisions the STA before it replies. */
            node_on_peer_message(buffer, len);
            if (!connected) {
                node_on_peer_connected(net, mask, PEER_CLIENT);
                connected = true;
            }
        }
    }
    peer_session_close(&session);
    if (connected) node_on_peer_lost(net, mask, PEER_CLIENT);
    return admitted;
}

static void tcp_client_task(void *arg) {
    (void)arg;
    while (sta_is_up) {
        char gateway[INET_ADDRSTRLEN];
        uint32_t net, mask;
        if (!get_network(gateway, sizeof(gateway), &net, &mask)) {
            vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
            continue;
        }
        int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (sock < 0) {
            vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
            continue;
        }
        struct sockaddr_in address = { .sin_family = AF_INET, .sin_port = htons(SERVER_PORT) };
        inet_pton(AF_INET, gateway, &address.sin_addr);
        bool admitted = true;
        if (sta_is_up && connect_gateway(sock, &address))
            admitted = socket_read_loop(sock, net, mask);
        shutdown(sock, SHUT_RDWR);
        close(sock);
        if (!admitted && sta_is_up) {
            ESP_LOGW(LOGGING_TAG, "Peer admission rejected; resuming Wi-Fi discovery");
            sta_is_up = false;
            node_reject_wireless_peer();
        }
        for (unsigned i = 0; i < RETRY_DELAY_MS / 100 && sta_is_up; ++i)
            vTaskDelay(pdMS_TO_TICKS(100));
    }
    client_task = NULL;
    vTaskDelete(NULL);
}

void client_wait_stopped(void) {
    if (client_task == xTaskGetCurrentTaskHandle()) return;
    while (client_task) vTaskDelay(pdMS_TO_TICKS(10));
}

void client_open(void) {
    if (sta_is_up) return;
    client_wait_stopped();
    if (!session_initialized) {
        ESP_ERROR_CHECK(peer_session_init(&session) ? ESP_OK : ESP_ERR_NO_MEM);
        session_initialized = true;
    }
    peer_session_start(&session);
    sta_is_up = true;
    ESP_ERROR_CHECK(xTaskCreatePinnedToCore(tcp_client_task, "tcp_client", TASK_CLIENT_STACK,
        NULL, TASK_CLIENT_PRIORITY, (TaskHandle_t *)&client_task, TASK_CLIENT_CORE) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}

void client_close(void) {
    sta_is_up = false;
    if (session_initialized) peer_session_shutdown(&session);
}

bool client_send_message(const uint8_t *msg, uint16_t len) {
    return session_initialized && peer_session_send(&session, msg, len);
}
