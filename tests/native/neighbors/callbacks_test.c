#include "../../../components/integration/callbacks/callbacks.c"
#include <setjmp.h>

#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT
#endif
#define CHECK(expr) do { if (!(expr)) return __LINE__; } while (0)

typedef struct { uint8_t items[10][600]; size_t size; unsigned count, read; } queue_t;
static queue_t queues[2];
static unsigned created, seen;
static int events[10];
static jmp_buf drained;
QueueHandle_t xQueueCreate(unsigned length, size_t size) {
    (void)length;
    queue_t *queue = &queues[created++];
    memset(queue, 0, sizeof(*queue));
    queue->size = size;
    return queue;
}
int xQueueSend(QueueHandle_t handle, const void *data, TickType_t wait) {
    (void)wait;
    queue_t *queue = handle;
    if (queue->count >= 10 || queue->size > 600) return 0;
    memcpy(queue->items[queue->count++], data, queue->size);
    return 1;
}
int xQueueReceive(QueueHandle_t handle, void *data, TickType_t wait) {
    (void)wait;
    queue_t *queue = handle;
    if (queue->read == queue->count) longjmp(drained, 1);
    memcpy(data, queue->items[queue->read++], queue->size);
    return 1;
}
void vTaskDelay(TickType_t ticks) { (void)ticks; }
int xTaskCreatePinnedToCore(void (*fn)(void *), const char *name, unsigned stack,
                           void *arg, unsigned priority, void *handle, unsigned core) {
    (void)fn; (void)name; (void)stack; (void)arg; (void)priority; (void)handle; (void)core;
    return pdPASS;
}
static void connected(void *ctx, uint32_t net, uint32_t mask) {
    (void)ctx; (void)net; (void)mask; events[seen++] = 1;
}
static void lost(void *ctx, uint32_t net, uint32_t mask) {
    (void)ctx; (void)net; (void)mask; events[seen++] = 3;
}
static void message(void *ctx, const uint8_t *data, uint16_t len) {
    (void)ctx; (void)len; events[seen++] = data[0];
}

EXPORT int test_peer_callbacks_preserve_session_order(void) {
    created = seen = 0;
    CHECK(node_init_event_queues() == ESP_OK);
    CHECK(node_start_event_tasks() == ESP_OK);
    node_register_wireless_callbacks((wireless_callbacks_t){
        .on_peer_connected = connected, .on_peer_message = message, .on_peer_lost = lost
    }, NULL);
    uint8_t first[] = {2}, second[] = {4}, third[] = {5};
    node_on_peer_message(first, sizeof(first));
    node_on_peer_connected(1, 2, PEER_CLIENT);
    node_on_peer_message(second, sizeof(second));
    node_on_peer_lost(1, 2, PEER_CLIENT);
    node_on_peer_connected(3, 4, PEER_SERVER);
    node_on_peer_message(third, sizeof(third));
    node_on_peer_message(NULL, 1);
    node_on_peer_message(third, 513);
    if (!setjmp(drained)) peer_event_task(NULL);
    const int expected[] = {2, 1, 4, 3, 1, 5};
    CHECK(seen == 6 && !memcmp(events, expected, sizeof(expected)));
    return 0;
}
