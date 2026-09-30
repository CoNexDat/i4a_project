#pragma once
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include <stddef.h>
#define UART_NUM_0 0
esp_err_t uart_wait_tx_done(int, TickType_t);
int uart_read_bytes(int, void *, unsigned, TickType_t);
esp_err_t uart_driver_install(int, int, int, int, void *, int);
esp_err_t uart_driver_delete(int);
