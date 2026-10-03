#pragma once
#include "FreeRTOS.h"
void vTaskDelay(TickType_t);
int xTaskCreatePinnedToCore(void (*fn)(void *), const char *name, unsigned stack,
                           void *arg, unsigned priority, void *handle, unsigned core);
