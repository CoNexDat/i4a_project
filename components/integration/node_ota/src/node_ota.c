#include "node_ota/node_ota.h"
#include "ota_receiver.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "reset_manager/reset_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

#define ACK_TIMEOUT_MS 15000
#define BOOT_CONFIRM_TIMEOUT_MS 90000
#define BOOT_RECOVERY_RESTART_MS 120000
#define SERIAL_LINE_SIZE (sizeof(NODE_OTA_PREFIX) + NODE_OTA_MAX_PACKET * 2 + 2)

static const char *TAG = "node_ota";
static ring_share_t *ring;
static uint8_t role;
static QueueHandle_t inbox;
static SemaphoreHandle_t exchange_lock;
static ota_receiver_t receiver;
static bool serial_started;
/* Latched only for the first boot of a newly selected OTA image. Cleared by
 * the next boot, when otadata already marks this image valid. */
static bool boot_recovery_pending;

_Static_assert(NODE_OTA_MAX_PACKET + 1 <= 512, "Must fit sibling callback queue");

static bool valid_request(const node_ota_packet_t *p)
{
    return p->sender == NODE_OTA_HOST && p->op >= NODE_OTA_INFO &&
           p->op <= NODE_OTA_REBOOT && p->length <= NODE_OTA_CHUNK &&
           (p->targets & (1U << NODE_OTA_CENTER)) && !(p->targets & ~NODE_OTA_ROLES);
}

static void on_ring_message(void *context, const uint8_t *wire, uint16_t length)
{
    node_ota_packet_t p;
    if (!node_ota_decode(wire, length, &p)) return;
    bool accept = role == NODE_OTA_CENTER
        ? ((p.op & NODE_OTA_REPLY) && p.sender < NODE_OTA_CENTER && p.length >= 5)
        : (valid_request(&p) && (p.targets & (1U << role)));
    /* Never erase/write flash or wait for ACKs in the shared ring callback.
     * Queue overflow is recovered by the host's exact retransmission. */
    if (accept) xQueueSend(inbox, &p, 0);
}

static void receiver_task(void *arg)
{
    node_ota_packet_t request, reply;
    uint8_t wire[NODE_OTA_MAX_PACKET];
    for (;;) {
        if (xQueueReceive(inbox, &request, pdMS_TO_TICKS(1000))) {
            ota_receiver_handle(&receiver, &request, &reply);
            size_t length = node_ota_encode(&reply, wire);
            rs_broadcast(ring, RS_NODE_OTA, wire, length);
        } else {
            ota_receiver_expire(&receiver);
        }
    }
}

esp_err_t node_ota_init(ring_share_t *rs, uint8_t device_role)
{
    if (ring || !rs || device_role > NODE_OTA_CENTER) return ESP_ERR_INVALID_STATE;
    role = device_role;
    inbox = xQueueCreate(10, sizeof(node_ota_packet_t));
    exchange_lock = xSemaphoreCreateMutex();
    if (!inbox || !exchange_lock) return ESP_ERR_NO_MEM;
    ring = rs;
    ota_receiver_init(&receiver, role);
    receiver.boot_recovery_pending = boot_recovery_pending;
    rs_register_component(ring, RS_NODE_OTA,
        (ring_callback_t){.callback = on_ring_message});
    if (role != NODE_OTA_CENTER &&
        xTaskCreate(receiver_task, "ota_receiver", 6144, NULL, 2, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}

esp_err_t node_ota_exchange(const node_ota_packet_t *request,
                            node_ota_reply_fn emit, void *context)
{
    if (!ring || role != NODE_OTA_CENTER) return ESP_ERR_INVALID_STATE;
    if (!request || !emit || !valid_request(request)) return ESP_ERR_INVALID_ARG;
    if (!xSemaphoreTake(exchange_lock, 0)) return ESP_ERR_INVALID_STATE;

    node_ota_packet_t reply, directed = *request;
    uint8_t wire[NODE_OTA_MAX_PACKET];
    while (xQueueReceive(inbox, &reply, 0)) { }
    esp_err_t result = ESP_OK;
    int64_t deadline = esp_timer_get_time() + ACK_TIMEOUT_MS * 1000LL;
    /* Only one board may modify flash at a time. Flash operations suspend
     * normal tasks/interrupts, including SPI receive/rearm and forwarding.
     * Concurrent erases/writes can therefore swallow another board's ACK.
     * Keep the center bit for compatibility with existing v1 receivers; the
     * center does not consume its own ring broadcast. Every operation uses
     * the same per-peer mask so session checks and cached retries still work. */
    for (uint8_t peer = 0; peer < NODE_OTA_CENTER; ++peer) {
        if (!(request->targets & (1U << peer))) continue;
        directed.targets = (1U << NODE_OTA_CENTER) | (1U << peer);
        size_t length = node_ota_encode(&directed, wire);
        rs_broadcast(ring, RS_NODE_OTA, wire, length);
        bool received = false;
        while (esp_timer_get_time() < deadline) {
            if (!xQueueReceive(inbox, &reply, pdMS_TO_TICKS(100))) continue;
            if (reply.op != (request->op | NODE_OTA_REPLY) ||
                reply.session != request->session || reply.sequence != request->sequence ||
                reply.targets != directed.targets || reply.sender != peer) continue;
            esp_err_t err = (esp_err_t)node_ota_get_u32(reply.data);
            if (err != ESP_OK) result = err;
            /* The host tracks the whole node; only the SPI hop is directed. */
            reply.targets = request->targets;
            emit(&reply, context);
            received = true;
            break;
        }
        if (!received) {
            ESP_LOGW(TAG, "OTA ACK timeout: op=%u seq=%lu offset=%lu role=%u",
                     request->op, (unsigned long)request->sequence,
                     (unsigned long)request->offset, peer);
            result = ESP_ERR_TIMEOUT;
            goto done;
        }
    }
    /* Receive peripheral ACKs before the center itself touches flash. */
    ota_receiver_handle(&receiver, request, &reply);
    emit(&reply, context);
    esp_err_t err = (esp_err_t)node_ota_get_u32(reply.data);
    if (err != ESP_OK) result = err;
done:
    xSemaphoreGive(exchange_lock);
    return result;
}

static void serial_reply(const node_ota_packet_t *reply, void *context)
{
    uint8_t wire[NODE_OTA_MAX_PACKET];
    size_t length = node_ota_encode(reply, wire);
    char line[SERIAL_LINE_SIZE];
    static const char hex[] = "0123456789abcdef";
    size_t pos = 0;
    line[pos++] = '\n'; /* resync even after a partial printf from another task */
    memcpy(line + pos, NODE_OTA_PREFIX, strlen(NODE_OTA_PREFIX));
    pos += strlen(NODE_OTA_PREFIX);
    for (size_t i = 0; i < length; ++i) {
        line[pos++] = hex[wire[i] >> 4];
        line[pos++] = hex[wire[i] & 15];
    }
    line[pos++] = '\n';
    flockfile(stdout);
    fwrite(line, 1, pos, stdout);
    fflush(stdout);
    funlockfile(stdout);
}

static int unhex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void serial_line(char *line, size_t length)
{
    size_t prefix = strlen(NODE_OTA_PREFIX);
    if (length < prefix || memcmp(line, NODE_OTA_PREFIX, prefix)) return;
    size_t bytes = (length - prefix) / 2;
    if ((length - prefix) % 2 || bytes > NODE_OTA_MAX_PACKET) return;
    uint8_t wire[NODE_OTA_MAX_PACKET];
    for (size_t i = 0; i < bytes; ++i) {
        int hi = unhex(line[prefix + 2 * i]), lo = unhex(line[prefix + 2 * i + 1]);
        if (hi < 0 || lo < 0) return;
        wire[i] = (hi << 4) | lo;
    }
    node_ota_packet_t request;
    if (!node_ota_decode(wire, bytes, &request)) return;
    esp_err_t err = node_ota_exchange(&request, serial_reply, NULL);
    if (err == ESP_OK && request.op == NODE_OTA_REBOOT) {
        /* Peripherals remain up to carry ACKs around the ring. The center's
         * existing startup reset broadcast restarts them after it comes up. */
        uart_wait_tx_done(UART_NUM_0, pdMS_TO_TICKS(1000));
        vTaskDelay(pdMS_TO_TICKS(500));
        rm_restart_local();
    }
}

static void serial_task(void *arg)
{
    char line[SERIAL_LINE_SIZE];
    size_t length = 0;
    bool overflow = false;
    int64_t last_byte = 0;
    for (;;) {
        uint8_t byte;
        if (uart_read_bytes(UART_NUM_0, &byte, 1, pdMS_TO_TICKS(100)) != 1) {
            if (esp_timer_get_time() - last_byte > 2000000) {
                length = 0;
                overflow = false;
            }
            if (xSemaphoreTake(exchange_lock, 0)) {
                ota_receiver_expire(&receiver);
                xSemaphoreGive(exchange_lock);
            }
            continue;
        }
        last_byte = esp_timer_get_time();
        if (byte == '\n') {
            if (!overflow) serial_line(line, length);
            length = 0;
            overflow = false;
        } else if (byte != '\r') {
            if (length < sizeof(line)) line[length++] = byte;
            else overflow = true;
        }
    }
}

esp_err_t node_ota_start_serial(void)
{
    if (role != NODE_OTA_CENTER) return ESP_OK;
    if (!ring || serial_started) return ESP_ERR_INVALID_STATE;
    /* USB-UART on DevKitC connects to UART0. Leave console TX configured by IDF. */
    esp_err_t err = uart_driver_install(UART_NUM_0, 4096, 0, 0, NULL, 0);
    if (err != ESP_OK) return err;
    if (xTaskCreate(serial_task, "ota_serial", 8192, NULL, 2, NULL) != pdPASS) {
        uart_driver_delete(UART_NUM_0);
        return ESP_ERR_NO_MEM;
    }
    serial_started = true;
    ESP_LOGI(TAG, "USB firmware update ready on UART0");
    return ESP_OK;
}

static bool boot_pending(void)
{
    esp_ota_img_states_t state;
    return esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
           state == ESP_OTA_IMG_PENDING_VERIFY;
}

void node_ota_boot_trace(const char *stage)
{
    /* Share stdout's lock with serial_reply, so diagnostics cannot split an ACK.
     * Before node_ota_init the GPIO role is not known yet (host/255 sentinel). */
    flockfile(stdout);
    printf("\nI4ABOOT t_ms=%lld role=%u stage=%s\n",
           (long long)(esp_timer_get_time() / 1000),
           (unsigned)(ring ? role : NODE_OTA_HOST), stage);
    fflush(stdout);
    funlockfile(stdout);
}

static void boot_watchdog(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(BOOT_CONFIRM_TIMEOUT_MS));
    if (boot_pending()) {
        node_ota_boot_trace("watchdog.pending_restart");
        rm_restart_local(); /* bootloader rolls back unconfirmed image */
        return;
    }
    /* Confirmation writes otadata while SPI is active. A stalled ring cannot
     * deliver RM reset broadcasts. Each newly updated board therefore restarts
     * locally once, after the normal confirmation/rollback window. On that
     * clean boot the center's existing RM startup synchronizes the node again.
     * Do not restart immediately on confirmation: other boards may still be
     * pending verification and a center reset would roll them back. */
    vTaskDelay(pdMS_TO_TICKS(BOOT_RECOVERY_RESTART_MS - BOOT_CONFIRM_TIMEOUT_MS));
    node_ota_boot_trace("recovery.local_restart");
    rm_restart_local();
    vTaskDelete(NULL);
}

void node_ota_watch_boot(void)
{
    bool pending = boot_pending();
    boot_recovery_pending = pending;
    node_ota_boot_trace(pending ? "boot.pending_verify" : "boot.not_pending");
    if (pending &&
        xTaskCreate(boot_watchdog, "ota_boot_check", 2048, NULL, 2, NULL) != pdPASS) {
        node_ota_boot_trace("watchdog.create_failed_restart");
        rm_restart_local();
    }
}

esp_err_t node_ota_confirm_boot(void)
{
    node_ota_boot_trace("confirm.check");
    if (!boot_pending()) {
        node_ota_boot_trace("confirm.not_pending");
        return ESP_OK;
    }
    node_ota_boot_trace("confirm.write_begin");
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    node_ota_boot_trace(err == ESP_OK ? "confirm.write_ok" : "confirm.write_failed");
    return err;
}
