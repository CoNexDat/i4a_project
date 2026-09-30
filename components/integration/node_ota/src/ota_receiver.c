#include "ota_receiver.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include <string.h>

static esp_err_t discard(ota_receiver_t *r)
{
    /* A failed SELECT can have written otadata before returning an error.
     * Always restore the running partition if boot selection changed. */
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    if (boot && running && boot->address != running->address) {
        esp_err_t err = esp_ota_set_boot_partition(running);
        if (err != ESP_OK) return err;
    }
    if (r->handle_open) {
        esp_ota_abort(r->handle);
        r->handle_open = false;
    }
    mbedtls_sha256_free(&r->hash);
    mbedtls_sha256_init(&r->hash);
    r->phase = NODE_OTA_IDLE;
    r->session = 0;
    r->targets = 0;
    r->size = 0;
    r->received = 0;
    r->cached = false;
    return ESP_OK;
}

void ota_receiver_init(ota_receiver_t *r, uint8_t role)
{
    memset(r, 0, sizeof(*r));
    r->role = role;
    mbedtls_sha256_init(&r->hash);
}

void ota_receiver_expire(ota_receiver_t *r)
{
    if (r->phase != NODE_OTA_IDLE &&
        esp_timer_get_time() - r->last_activity > NODE_OTA_IDLE_TIMEOUT_US)
        discard(r);
}

static esp_err_t apply(ota_receiver_t *r, const node_ota_packet_t *p)
{
    if (p->op == NODE_OTA_INFO)
        return p->length == 0 ? ESP_OK : ESP_ERR_INVALID_SIZE;
    if (!p->session) return ESP_ERR_INVALID_ARG;

    if (p->op == NODE_OTA_BEGIN) {
        if (r->boot_recovery_pending) return ESP_ERR_INVALID_STATE;
        if (p->length != 36 || p->offset != 0) return ESP_ERR_INVALID_SIZE;
        if (r->phase != NODE_OTA_IDLE) return ESP_ERR_INVALID_STATE;
        r->partition = esp_ota_get_next_update_partition(NULL);
        r->size = node_ota_get_u32(p->data);
        if (!r->partition) return ESP_ERR_NOT_FOUND;
        if (!r->size || r->size > r->partition->size) return ESP_ERR_INVALID_SIZE;
        /* Erase sectors as DATA arrives, instead of blocking the SPI ring
         * while erasing the entire image during BEGIN. Size/offset bounds
         * remain enforced by this receiver. */
        esp_err_t err = esp_ota_begin(r->partition, OTA_WITH_SEQUENTIAL_WRITES, &r->handle);
        if (err != ESP_OK) return err;
        r->handle_open = true;
        r->session = p->session;
        r->targets = p->targets;
        r->received = 0;
        memcpy(r->expected_hash, p->data + 4, 32);
        if (mbedtls_sha256_starts(&r->hash, 0) != 0) {
            discard(r);
            return ESP_FAIL;
        }
        r->phase = NODE_OTA_RECEIVING;
        return ESP_OK;
    }

    if (p->op == NODE_OTA_ABORT && r->phase == NODE_OTA_IDLE)
        return p->length == 0 ? ESP_OK : ESP_ERR_INVALID_SIZE;
    if (p->session != r->session || p->targets != r->targets)
        return ESP_ERR_INVALID_STATE;
    if (p->op == NODE_OTA_ABORT)
        return p->length == 0 ? discard(r) : ESP_ERR_INVALID_SIZE;

    if (p->op == NODE_OTA_DATA) {
        if (r->phase != NODE_OTA_RECEIVING) return ESP_ERR_INVALID_STATE;
        if (!p->length || p->offset != r->received || p->length > r->size - r->received)
            return ESP_ERR_INVALID_SIZE;
        esp_err_t err = esp_ota_write(r->handle, p->data, p->length);
        if (err == ESP_OK && mbedtls_sha256_update(&r->hash, p->data, p->length) != 0)
            err = ESP_FAIL;
        if (err != ESP_OK) {
            discard(r);
            return err;
        }
        r->received += p->length;
        return ESP_OK;
    }

    if (p->length != 0 || p->offset != r->size) return ESP_ERR_INVALID_SIZE;
    if (p->op == NODE_OTA_END) {
        if (r->phase != NODE_OTA_RECEIVING || r->received != r->size)
            return ESP_ERR_INVALID_STATE;
        uint8_t digest[32];
        if (mbedtls_sha256_finish(&r->hash, digest) != 0 ||
            memcmp(digest, r->expected_hash, 32) != 0) {
            discard(r);
            return ESP_ERR_INVALID_CRC;
        }
        esp_err_t err = esp_ota_end(r->handle);
        r->handle_open = false; /* esp_ota_end frees the handle even on failure. */
        if (err != ESP_OK) {
            discard(r);
            return err;
        }
        r->phase = NODE_OTA_READY;
        return ESP_OK;
    }
    if (p->op == NODE_OTA_SELECT) {
        if (r->phase != NODE_OTA_READY) return ESP_ERR_INVALID_STATE;
        esp_err_t err = esp_ota_set_boot_partition(r->partition);
        if (err == ESP_OK) r->phase = NODE_OTA_SELECTED;
        return err;
    }
    if (p->op == NODE_OTA_REBOOT)
        return r->phase == NODE_OTA_SELECTED ? ESP_OK : ESP_ERR_INVALID_STATE;
    return ESP_ERR_NOT_SUPPORTED;
}

void ota_receiver_handle(ota_receiver_t *r, const node_ota_packet_t *p,
                         node_ota_packet_t *reply)
{
    ota_receiver_expire(r);
    /* Only exact retransmissions are idempotent, including payload bytes. */
    if (r->cached && p->op != NODE_OTA_INFO &&
        p->op == r->previous.op && p->session == r->previous.session &&
        p->sequence == r->previous.sequence && p->targets == r->previous.targets &&
        p->offset == r->previous.offset && p->length == r->previous.length &&
        memcmp(p->data, r->previous.data, p->length) == 0) {
        r->last_activity = esp_timer_get_time();
        *reply = r->response;
        return;
    }
    esp_err_t err = apply(r, p);
    if (err == ESP_OK && p->op != NODE_OTA_INFO) r->last_activity = esp_timer_get_time();
    memset(reply, 0, sizeof(*reply));
    reply->op = p->op | NODE_OTA_REPLY;
    reply->targets = p->targets;
    reply->sender = r->role;
    reply->session = p->session;
    reply->sequence = p->sequence;
    reply->offset = r->received;
    reply->length = 5;
    node_ota_put_u32(reply->data, (uint32_t)err);
    reply->data[4] = r->phase;
    if (p->op == NODE_OTA_INFO) {
        const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
        const esp_partition_t *running = esp_ota_get_running_partition();
        node_ota_put_u32(reply->data + 5, next ? next->size : 0);
        node_ota_put_u32(reply->data + 9, running ? running->address : 0);
        memcpy(reply->data + 13, esp_app_get_description()->app_elf_sha256, 32);
        /* Expose session for diagnosing interrupted uploads; INFO never renews it. */
        node_ota_put_u32(reply->data + 45, r->session);
        esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
        if (running) esp_ota_get_state_partition(running, &state);
        reply->data[49] = state == ESP_OTA_IMG_PENDING_VERIFY;
        reply->data[50] = r->boot_recovery_pending;
        reply->length = 51;
    } else if (err == ESP_OK) {
        r->cached = true;
        r->previous = *p;
        r->response = *reply;
    }
}
