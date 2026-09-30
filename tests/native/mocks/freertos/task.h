#pragma once
#include "FreeRTOS.h"
int xTaskCreate(void (*fn)(void *), const char *, unsigned, void *, unsigned, void *);
void vTaskDelay(TickType_t);
void vTaskDelete(void *);
