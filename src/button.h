#pragma once

// Button driver: debounced SHORT / LONG press detection on one GPIO.
//
// Single instance. The pin is active LOW (internal pull-up is enabled by the driver).
// The driver knows nothing about who uses the presses: it only calls the callback.

#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

typedef enum {
    BUTTON_EVENT_SHORT_PRESS,
    BUTTON_EVENT_LONG_PRESS
} button_event_t;

// Called from the button TASK (not from the ISR), once per completed press.
// So it's allowed to log and to use normal FreeRTOS calls (xQueueSend, not ...FromISR),
// but it should be short and must not block for long: while it runs, the button is not read.
typedef void (*button_callback_t)(button_event_t event, TickType_t held_ticks, void *user_ctx);

typedef struct {
    gpio_num_t pin;                 // must be < 32
    TickType_t debounce_ticks;
    TickType_t long_press_ticks;    // must be > debounce_ticks
    UBaseType_t task_priority;
    uint32_t task_stack_size;
    button_callback_t on_event;     // must not be NULL
    void *user_ctx;                 // passed to on_event as is
} button_config_t;

// Configures the GPIO, the ISR, the internal queue and the button task.
// Returns ESP_ERR_INVALID_ARG for a bad config, ESP_ERR_INVALID_STATE if called twice,
// ESP_ERR_NO_MEM if the task can't be created, or the error of the GPIO driver.
esp_err_t button_init(const button_config_t *config);

const char *button_event_name(button_event_t event);

// How many edges the ISR could not report because the internal queue was full
uint32_t button_isr_error_count(void);
