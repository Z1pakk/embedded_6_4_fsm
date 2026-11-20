# 6.4 Electronic Safe

Password: **SHORT, SHORT, LONG** on one button (GPIO4, active LOW).
Wrong press or 3 s without input → reset. Unlocked → locks again on any press or after 5 s.

## Structure

```
button_isr ──► button task ──callback──► on_button() ──events_queue──► fsm_task
└──────── src/button.c ───┘              └─────────── src/main.c ────────────┘
```

- `button.c/.h`: button driver. Debounce + SHORT/LONG detection, reported through a callback. It knows nothing about the safe.
- `main.c`: safe FSM. `on_button()` maps button events to safe events.

## Debounce

1. The ISR fires on the first edge and disables its own interrupt.
2. The button task waits 50 ms.
3. The task clears the latched interrupt bit, re-enables the interrupt, and reads the level.

## Safe FSM

| State \ Event | SHORT | LONG | TIMEOUT |
|---|---|---|---|
| LOCKED | GOT_S | LOCKED | LOCKED (waits forever) |
| GOT_S | GOT_SS | LOCKED | LOCKED (3 s) |
| GOT_SS | LOCKED | **UNLOCKED** | LOCKED (3 s) |
| UNLOCKED | LOCKED | LOCKED | LOCKED (5 s) |

TIMEOUT is `xQueueReceive` timing out, not a separate timer.

## Timings

| | Value |
|---|---|
| Debounce | 50 ms |
| Long press | > 1000 ms |
| Input timeout | 3 s, counted from release |
| Auto-lock | 5 s |

## Notes

- `gpio_get_level()` is in flash (`CONFIG_GPIO_CTRL_FUNC_IN_IRAM` not set), so it's read in the task, not the ISR.
- `button_init` rejects pins ≥ 32, because `gpio_ll_clear_intr_status` only covers GPIO 0..31.

## Tests

| Input | Expected | Result |
|---|---|---|
| S S L | UNLOCKED | |
| L | reset | |
| S L | reset | |
| S S S | reset | |
| S, wait 4 s | timeout reset | |
| 10 fast taps | 10 SHORT | |
| unlocked, wait 5 s | auto-lock | |
