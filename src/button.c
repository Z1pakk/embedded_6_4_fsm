#include "button.h"

#include <stdatomic.h>
#include <stdbool.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "hal/gpio_ll.h"

#define BUTTON_QUEUE_LENGTH UINT8_C(2)   // ISR disables itself after 1 edge -> at most 1 item waits

static const char *TAG = "button";

typedef enum {
    BUTTON_PRESSED,
    BUTTON_RELEASED
} button_state_t;

// input of the button FSM: the settled level of the pin, read after the debounce time
typedef enum {
    BUTTON_INPUT_DOWN,   // pin is LOW  -> button is held
    BUTTON_INPUT_UP      // pin is HIGH -> button is released
} button_input_t;

typedef struct {
    button_input_t input;
    TickType_t timestamp_ticks;   // time of the first edge (= when the change began)
} button_input_msg_t;

// Everything the button FSM remembers between steps.
// press_start_ticks only means something while state == BUTTON_PRESSED.
typedef struct {
    button_state_t state;
    TickType_t press_start_ticks;
} button_fsm_t;

// result of one button FSM step: where to go next + what to report
typedef struct {
    button_fsm_t next;
    bool has_event;          // false = nothing to report
    button_event_t event;    // valid when has_event
    TickType_t held_ticks;   // press duration, valid when has_event
} button_fsm_result_t;

typedef struct {
    TickType_t timestamp_ticks;
} button_edge_msg_t;

static button_config_t s_config;
static bool s_initialized = false;

static QueueHandle_t s_queue;
static uint8_t s_queue_storage[BUTTON_QUEUE_LENGTH * sizeof(button_edge_msg_t)];
static StaticQueue_t s_queue_ctrl;

static _Atomic uint32_t s_isr_errors = 0;

const char *button_event_name(button_event_t event) {
    switch (event) {
        case BUTTON_EVENT_SHORT_PRESS: return "SHORT PRESS";
        case BUTTON_EVENT_LONG_PRESS:  return "LONG PRESS";
    }
    return "?";
}

uint32_t button_isr_error_count(void) {
    return atomic_load(&s_isr_errors);
}

// Fires on the FIRST edge of a press/release, then switches itself off so the bounce
// edges don't fire at all. The button task switches it back on after the debounce time.
//
// Note: gpio_intr_disable() is not in IRAM (CONFIG_GPIO_CTRL_FUNC_IN_IRAM is not set).
// That's fine here: the ISR service is installed with flags = 0 (no ESP_INTR_FLAG_IRAM),
// so this ISR never runs while the flash cache is disabled.
static void IRAM_ATTR button_isr(void *arg) {
    (void)arg;

    BaseType_t high_priority_task_woken = pdFALSE;

    gpio_intr_disable(s_config.pin);  // ignore the rest of the bounce burst

    button_edge_msg_t msg = { .timestamp_ticks = xTaskGetTickCountFromISR() };

    if (xQueueSendFromISR(s_queue, &msg, &high_priority_task_woken) != pdPASS) {
        atomic_fetch_add(&s_isr_errors, 1);  // the queue is full: ISR cannot wait
    }

    portYIELD_FROM_ISR(high_priority_task_woken);
}

// Turn the settled pin level into an input event for the button FSM
static button_input_msg_t read_button_input(TickType_t edge_ticks) {
    button_input_msg_t msg = { .input = BUTTON_INPUT_UP, .timestamp_ticks = edge_ticks };

    if (gpio_get_level(s_config.pin) == 0) {   // active LOW (pull-up): LOW = held
        msg.input = BUTTON_INPUT_DOWN;
    }
    return msg;
}

/*
 * Button FSM (state x input):
 *
 *  State \ Input   DOWN                  UP
 *  RELEASED        -> PRESSED            -> RELEASED (glitch, ignore)
 *  PRESSED         -> PRESSED (glitch)   -> RELEASED + SHORT/LONG
 *
 * Only decides — does not send anything anywhere and touches no globals.
 */
static button_fsm_result_t handle_button_input(button_fsm_t fsm, button_input_msg_t msg) {
    button_fsm_result_t result = { .next = fsm, .has_event = false, .held_ticks = 0 };

    switch (fsm.state) {
        case BUTTON_RELEASED:
            switch (msg.input) {
                case BUTTON_INPUT_DOWN:             // really went down
                    result.next.state = BUTTON_PRESSED;
                    result.next.press_start_ticks = msg.timestamp_ticks;
                    break;
                case BUTTON_INPUT_UP:               // still up -> it was just a glitch
                    break;
            }
            break;

        case BUTTON_PRESSED:
            switch (msg.input) {
                case BUTTON_INPUT_UP:               // really went up
                    result.next.state = BUTTON_RELEASED;
                    result.has_event = true;
                    result.held_ticks = msg.timestamp_ticks - fsm.press_start_ticks;
                    result.event = BUTTON_EVENT_SHORT_PRESS;
                    if (result.held_ticks > s_config.long_press_ticks) {
                        result.event = BUTTON_EVENT_LONG_PRESS;
                    }
                    break;
                case BUTTON_INPUT_DOWN:             // still down -> it was just a glitch
                    break;
            }
            break;
    }
    return result;
}

static void button_task(void *arg) {
    (void)arg;
    button_fsm_t fsm = { .state = BUTTON_RELEASED, .press_start_ticks = 0 };

    ESP_LOGI(TAG, "Button task started");
    for (;;) {
        // 1. wait for the first edge (the ISR has already disabled its interrupt)
        button_edge_msg_t msg;
        if (xQueueReceive(s_queue, &msg, portMAX_DELAY) != pdPASS) {
            continue;
        }

        // 2. let the contact stop bouncing (the interrupt is off, so bounce edges are not seen)
        vTaskDelay(s_config.debounce_ticks);

        // 3. CLEAR -> ENABLE -> READ.
        //    Clear: drop a status bit that may have been latched by bounce edges while the
        //    interrupt was off, so enabling doesn't fire immediately for a stale edge.
        //    (gpio_ll_clear_intr_status covers GPIO 0..31 only -> button_init checks pin < 32)
        //    Enable, then read: a change right after this point either shows up in the read
        //    below or fires a new interrupt — it can never be lost.
        gpio_ll_clear_intr_status(&GPIO, 1UL << s_config.pin);
        ESP_ERROR_CHECK(gpio_intr_enable(s_config.pin));
        const button_input_msg_t input = read_button_input(msg.timestamp_ticks);

        // 4. feed the input to the FSM. If the bounce was longer than the debounce
        //    time, the interrupt may fire again: that's harmless, the level won't have changed.
        const button_fsm_result_t result = handle_button_input(fsm, input);
        fsm = result.next;

        if (result.has_event) {
            s_config.on_event(result.event, result.held_ticks, s_config.user_ctx);
        }
    }
}

esp_err_t button_init(const button_config_t *config) {
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (config == NULL || config->on_event == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->pin < 0 || config->pin >= 32) {   // gpio_ll_clear_intr_status covers 0..31 only
        return ESP_ERR_INVALID_ARG;
    }
    if (config->long_press_ticks <= config->debounce_ticks) {
        return ESP_ERR_INVALID_ARG;
    }

    s_config = *config;   // keep our own copy: the caller's struct may be on its stack

    s_queue = xQueueCreateStatic(BUTTON_QUEUE_LENGTH, sizeof(button_edge_msg_t), s_queue_storage, &s_queue_ctrl);
    if (s_queue == NULL) {
        return ESP_FAIL;
    }

    // task first, ISR last: an edge must never arrive before someone can receive it
    if (xTaskCreate(button_task, "Button task", s_config.task_stack_size, NULL, s_config.task_priority, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    const gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << s_config.pin,
        .mode = GPIO_MODE_INPUT,
        // Trust No Pin: enable internal pull-up explicitly, don't rely on the board
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,  // need both edges: press and release
    };
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        return err;
    }

    // ESP_ERR_INVALID_STATE = the service is already installed (e.g. by another driver): fine
    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = gpio_isr_handler_add(s_config.pin, button_isr, NULL);
    if (err != ESP_OK) {
        return err;
    }

    s_initialized = true;
    return ESP_OK;
}
