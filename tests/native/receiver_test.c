/* Host tests for the actual C receiver. Flash and crypto providers are mocked;
 * these tests exercise sequencing, failure recovery and idempotence. */
#include "ota_receiver.h"
#include "esp_app_desc.h"
#include <string.h>

#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT
#endif
#define CHECK(expr) do { if (!(expr)) return __LINE__; } while (0)

void *memcpy(void *d, const void *s, size_t n) {
    unsigned char *dst = d; const unsigned char *src = s;
    for (size_t i = 0; i < n; ++i) dst[i] = src[i];
    return d;
}
void *memset(void *d, int c, size_t n) {
    unsigned char *dst = d;
    for (size_t i = 0; i < n; ++i) dst[i] = c;
    return d;
}
int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = a, *y = b;
    for (size_t i = 0; i < n; ++i) if (x[i] != y[i]) return x[i] - y[i];
    return 0;
}

static esp_partition_t running = {0x20000, 4096}, next = {0x210000, 4096};
static const esp_partition_t *boot;
static esp_app_desc_t description;
static int writes, begins, ends, aborts;
static size_t begin_size;
static esp_err_t write_error, end_error, select_error;
static int64_t now;
static esp_ota_img_states_t image_state = ESP_OTA_IMG_VALID;
static esp_err_t confirm_error;
static unsigned confirmations;
static ota_receiver_t receiver;
static node_ota_packet_t request, reply;

const esp_partition_t *esp_ota_get_running_partition(void) { return &running; }
const esp_partition_t *esp_ota_get_boot_partition(void) { return boot; }
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *p) { return &next; }
const esp_app_desc_t *esp_app_get_description(void) { return &description; }
int64_t esp_timer_get_time(void) { return now; }
esp_err_t esp_ota_begin(const esp_partition_t *p, size_t n, esp_ota_handle_t *h) { ++begins; begin_size = n; *h = 1; return ESP_OK; }
esp_err_t esp_ota_write(esp_ota_handle_t h, const void *d, size_t n) { ++writes; return write_error; }
esp_err_t esp_ota_end(esp_ota_handle_t h) { ++ends; return end_error; }
esp_err_t esp_ota_abort(esp_ota_handle_t h) { ++aborts; return ESP_OK; }
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *p) { boot = p; return select_error; }
esp_err_t esp_ota_get_state_partition(const esp_partition_t *p, esp_ota_img_states_t *s) { *s = image_state; return ESP_OK; }
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void) {
    ++confirmations;
    if (confirm_error == ESP_OK) image_state = ESP_OTA_IMG_VALID;
    return confirm_error;
}
void mock_boot(esp_ota_img_states_t state, esp_err_t error) {
    image_state = state;
    confirm_error = error;
    confirmations = 0;
    now = 0;
}
unsigned mock_confirmations(void) { return confirmations; }
void mock_advance_time(int64_t delta) { now += delta; }
void mbedtls_sha256_init(mbedtls_sha256_context *c) { }
void mbedtls_sha256_free(mbedtls_sha256_context *c) { }
int mbedtls_sha256_starts(mbedtls_sha256_context *c, int is224) { return 0; }
int mbedtls_sha256_update(mbedtls_sha256_context *c, const unsigned char *d, size_t n) { return 0; }
int mbedtls_sha256_finish(mbedtls_sha256_context *c, unsigned char d[32]) { memset(d, 0x42, 32); return 0; }

static void reset(void) {
    boot = &running;
    writes = begins = ends = aborts = 0;
    write_error = end_error = select_error = ESP_OK;
    now = 1;
    ota_receiver_init(&receiver, 0);
    memset(&request, 0, sizeof(request));
    request.sender = NODE_OTA_HOST;
    request.targets = 0x11;
    request.session = 123;
}

static esp_err_t call(uint8_t op, uint32_t offset, uint16_t length) {
    request.op = op;
    request.offset = offset;
    request.length = length;
    ++request.sequence;
    ota_receiver_handle(&receiver, &request, &reply);
    return (esp_err_t)node_ota_get_u32(reply.data);
}

static esp_err_t begin(void) {
    node_ota_put_u32(request.data, 4);
    memset(request.data + 4, 0x42, 32);
    return call(NODE_OTA_BEGIN, 0, 36);
}

EXPORT int test_lifecycle(void) {
    reset();
    CHECK(begin() == ESP_OK);
    CHECK(begin_size == OTA_WITH_SEQUENTIAL_WRITES);
    ota_receiver_handle(&receiver, &request, &reply);
    CHECK(begins == 1);
    CHECK(call(NODE_OTA_DATA, 0, 4) == ESP_OK);
    ota_receiver_handle(&receiver, &request, &reply);
    CHECK(writes == 1 && receiver.received == 4);
    CHECK(call(NODE_OTA_SELECT, 4, 0) == ESP_ERR_INVALID_STATE);
    CHECK(boot == &running);
    CHECK(call(NODE_OTA_END, 4, 0) == ESP_OK && ends == 1);
    ota_receiver_handle(&receiver, &request, &reply);
    CHECK(ends == 1 && boot == &running);
    CHECK(call(NODE_OTA_SELECT, 4, 0) == ESP_OK && boot == &next);
    ota_receiver_handle(&receiver, &request, &reply);
    CHECK(node_ota_get_u32(reply.data) == ESP_OK);
    CHECK(call(NODE_OTA_REBOOT, 4, 0) == ESP_OK);
    CHECK(call(NODE_OTA_ABORT, 0, 0) == ESP_OK && boot == &running);
    CHECK(receiver.phase == NODE_OTA_IDLE);
    return 0;
}

EXPORT int test_invalid_order_and_hash(void) {
    reset();
    CHECK(call(NODE_OTA_DATA, 0, 4) != ESP_OK);
    CHECK(begin() == ESP_OK);
    CHECK(call(NODE_OTA_DATA, 1, 4) != ESP_OK && writes == 0);
    CHECK(call(NODE_OTA_DATA, 0, 5) != ESP_OK && writes == 0);
    CHECK(call(NODE_OTA_END, 4, 0) != ESP_OK);
    request.session++;
    CHECK(call(NODE_OTA_DATA, 0, 4) != ESP_OK);
    request.session--;
    CHECK(call(NODE_OTA_DATA, 0, 4) == ESP_OK);
    request.data[0] ^= 1;
    ota_receiver_handle(&receiver, &request, &reply);
    CHECK(node_ota_get_u32(reply.data) != ESP_OK && writes == 1);
    receiver.expected_hash[0] ^= 1;
    CHECK(call(NODE_OTA_END, 4, 0) == ESP_ERR_INVALID_CRC);
    CHECK(boot == &running && aborts == 1 && receiver.phase == NODE_OTA_IDLE);
    return 0;
}

EXPORT int test_expiry_and_flash_failures(void) {
    reset();
    CHECK(begin() == ESP_OK);
    write_error = ESP_FAIL;
    CHECK(call(NODE_OTA_DATA, 0, 4) == ESP_FAIL);
    CHECK(receiver.phase == NODE_OTA_IDLE && aborts == 1);
    reset();
    CHECK(begin() == ESP_OK);
    now += NODE_OTA_IDLE_TIMEOUT_US + 1;
    ota_receiver_expire(&receiver);
    CHECK(receiver.phase == NODE_OTA_IDLE && aborts == 1);
    reset();
    CHECK(begin() == ESP_OK);
    CHECK(call(NODE_OTA_DATA, 0, 4) == ESP_OK);
    end_error = ESP_FAIL;
    CHECK(call(NODE_OTA_END, 4, 0) == ESP_FAIL);
    CHECK(aborts == 0 && boot == &running && receiver.phase == NODE_OTA_IDLE);
    reset();
    CHECK(begin() == ESP_OK);
    CHECK(call(NODE_OTA_DATA, 0, 4) == ESP_OK);
    CHECK(call(NODE_OTA_END, 4, 0) == ESP_OK);
    CHECK(call(NODE_OTA_SELECT, 4, 0) == ESP_OK);
    now += NODE_OTA_IDLE_TIMEOUT_US + 1;
    CHECK(call(NODE_OTA_INFO, 0, 0) == ESP_OK);
    CHECK(receiver.phase == NODE_OTA_IDLE && boot == &running);
    CHECK(call(NODE_OTA_REBOOT, 4, 0) != ESP_OK);
    return 0;
}

EXPORT int test_partial_select_failure(void) {
    reset();
    CHECK(begin() == ESP_OK);
    CHECK(call(NODE_OTA_DATA, 0, 4) == ESP_OK);
    CHECK(call(NODE_OTA_END, 4, 0) == ESP_OK);
    select_error = ESP_FAIL; /* simulate otadata write before API failure */
    CHECK(call(NODE_OTA_SELECT, 4, 0) == ESP_FAIL);
    select_error = ESP_OK;
    CHECK(call(NODE_OTA_ABORT, 0, 0) == ESP_OK && boot == &running);
    return 0;
}

EXPORT int test_info_does_not_change_transaction(void) {
    reset();
    node_ota_put_u32(request.data, next.size + 1);
    CHECK(call(NODE_OTA_BEGIN, 0, 36) == ESP_ERR_INVALID_SIZE && begins == 0);
    CHECK(begin() == ESP_OK);
    node_ota_packet_t original = request;
    now += NODE_OTA_IDLE_TIMEOUT_US / 2;
    CHECK(call(NODE_OTA_INFO, 0, 0) == ESP_OK && reply.length == 51);
    CHECK(receiver.last_activity == 1);
    ota_receiver_handle(&receiver, &original, &reply);
    CHECK(begins == 1 && node_ota_get_u32(reply.data) == ESP_OK);
    request.targets = 0x13;
    CHECK(call(NODE_OTA_DATA, 0, 4) == ESP_ERR_INVALID_STATE && writes == 0);
    request.targets = 0x11;
    CHECK(call(NODE_OTA_ABORT, 0, 0) == ESP_OK);
    CHECK(receiver.session == 0 && !receiver.handle_open);
    CHECK(begin() == ESP_OK && begins == 2);
    return 0;
}

EXPORT int roundtrip(const uint8_t *input, size_t length, uint8_t *output) {
    node_ota_packet_t packet;
    if (!node_ota_decode(input, length, &packet)) return 0;
    return (int)node_ota_encode(&packet, output);
}
