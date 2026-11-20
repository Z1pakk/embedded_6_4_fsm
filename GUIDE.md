# 6.4 Electronic Safe: Step-by-Step Guide (ISR version)

Goal: a single button. Password = **SHORT → SHORT → LONG**.
Wrong press **or** 3 s without input → immediately back to the initial state.

You will build **two state machines**, an **interrupt (ISR)** and **two queues**:

```
GPIO0 edge → ISR (tiny) ──raw_q──► button task (debounce FSM) ──event_q──► safe task (password FSM) → logs / LEDs
             "edge at time t"        "it was SHORT / LONG"                  "S, S, L → UNLOCKED"
```

The key idea of the lesson: **each part only knows its own job.**
- The **ISR** doesn't decide anything. It only records that an edge happened, and when.
- The **button task** turns noisy edges into clean `SHORT` / `LONG` events. It is the "ideal button" (the Producer).
- The **safe task** has no idea what GPIO, bouncing or interrupts are. It only sees `SHORT`, `LONG` and `TIMEOUT` (the Consumer).

Work through the milestones in order. **Each one must run on the board before you start the next.**

---

## Milestone 0: Theory (about 20 min)

### Finite state machines
A finite state machine has:
1. **States**: where you are right now (only one at a time).
2. **Events**: things that happen (an edge, a press, a timeout).
3. **Transitions**: rules of the form "in state X, on event Y, go to state Z".
4. **Actions** (optional): what to do on a transition or on entering a state (log, turn on an LED).

### Interrupts in ESP-IDF / FreeRTOS
- An ISR interrupts *whatever* is running, so it must be **short**.
- Inside an ISR you must not call `ESP_LOG*`, `printf`, `vTaskDelay`, or anything that can block.
- Only `...FromISR` FreeRTOS functions are allowed (`xQueueSendFromISR`, `xTaskNotifyFromISR`, ...).
- `higherPriorityTaskWoken` + `portYIELD_FROM_ISR()`: look up what they do and why, without them, the woken task may run up to one tick (10 ms) late.
- `IRAM_ATTR`: look up why an ISR should live in IRAM, not flash.
- Pattern name to google: **"deferred interrupt processing"**.

✅ **Self-check:**
- Why is `if/else` with flags like `bool got_first, got_second` worse than an explicit `enum state`?
- The ISR *could* classify SHORT/LONG on release. Why do we still move that logic into a task? (Think: logging, debugging, ISR length, debounce quality.)

---

## Facts about *your* project (from `sdkconfig.esp32-s3-devkitc-1`)

| Setting | Value | What it means for you |
|---|---|---|
| `CONFIG_FREERTOS_HZ` | `100` | 1 tick = **10 ms**. Too coarse to measure bounce or debounce precisely. Take timestamps with `esp_timer_get_time()` (µs, safe to call in an ISR). |
| `CONFIG_GPIO_CTRL_FUNC_IN_IRAM` | not set | `gpio_get_level()` lives in flash. **Don't call it in your `IRAM_ATTR` ISR.** Read the level in the task. Mention this in the README. |

Hardware: the **BOOT button is on GPIO0**. It's active LOW and needs a pull-up, so no wiring is needed.
Optional LEDs: GPIO 10/11/12 (your RGB from 1.7). Green = unlocked, red = reset.

---

## Milestone 1: Design on paper (do NOT skip)

### 1a. Safe FSM (Consumer)

Fill in this table:

| Current state \ Event | SHORT | LONG | TIMEOUT (3 s) |
|---|---|---|---|
| Initial (waiting for 1st press) | ? | ? | ? |
| Got 1 SHORT | ? | ? | ? |
| Got 2 SHORT | ? | ? | ? |
| Unlocked | ? | ? | ? |

Decide:
- If the user presses **S S S**, do you reset, or treat the 3rd S as a new "first S"? The task says **reset to the initial state**. Stay consistent.
- Does TIMEOUT make sense in the initial state? (Nothing to reset, so maybe wait forever?)
- Unlocked: does it lock again after N seconds, or on any press? Your choice, but write it down.

### 1b. Button FSM (Producer), driven by ISR events

There is no polling. The button task only wakes up when one of these happens:
- `EDGE`: the ISR put a timestamp into `raw_q`
- `QUIET`: `xQueueReceive(raw_q, ..., timeout)` returned "nothing", meaning there were no edges for `timeout` ms

States:
- `RELEASED`: idle
- `DEBOUNCE_PRESS`: edges are happening, the button is probably going down
- `PRESSED`: stable and down, measuring how long it's held
- `DEBOUNCE_RELEASE`: edges are happening, the button is probably going up

Fill in:

| State | Receive timeout | On `EDGE` | On `QUIET` |
|---|---|---|---|
| RELEASED | ? (finite or forever?) | ? | ? |
| DEBOUNCE_PRESS | ? | ? | ? (read the level: is it really down?) |
| PRESSED | ? | ? | ? |
| DEBOUNCE_RELEASE | ? | ? | ? (read the level: emit SHORT/LONG?) |

Think about:
- Where do you read `gpio_get_level()`? (Only in the task, and only once things are QUIET.)
- Which timestamp marks the **start** of the press, and which marks the **end**? (First edge? Last edge?)
- What if the level you read on QUIET doesn't match what you expected (a very short glitch)?

Choose numbers: debounce time (typical 20–50 ms) and short/long threshold (typical 500–800 ms).

✅ **Self-check:** Every cell in both tables has an answer, and you've drawn both diagrams (circles = states, arrows = events).
**Send both tables to the reviewer (or me) before coding.**

---

## Milestone 2: See the bounce

Goal: get an ISR working and *see* the real bounce of your button.

1. Configure GPIO0: input, pull-up, `intr_type = GPIO_INTR_ANYEDGE`.
2. `gpio_install_isr_service(0)` then `gpio_isr_handler_add(pin, handler, arg)`.
3. In the ISR: get `esp_timer_get_time()`, `xQueueSendFromISR` it into `raw_q`, then `portYIELD_FROM_ISR`.
4. A task receives the timestamps and logs them (logging is allowed in a task).
5. Press the button about 10 times.

Record the following:
- How many edges does one press produce? (Min / max)
- How long, in µs, from the first edge to the last edge of a single press?

Use the numbers:
- **Size of `raw_q`**: if one press gives 30 edges and the queue holds 8, which lost edge actually hurts? Is losing a bounce edge harmful? What about losing the *last* edge? (Hint: in the design above the task reads the *level* on QUIET, so what does that protect you from?)
- **Debounce time**: pick it comfortably above the longest bounce you measured.

✅ **Done when:** you have a short table of bounce measurements for the README.

---

## Milestone 3: Ideal button (Producer)

Replace the logging task with your button FSM from Milestone 1b.

Structure of the task:
1. `for (;;)`: choose the receive timeout from the current state.
2. `xQueueReceive(raw_q, &t, timeout)`. The result is `EDGE` (got a timestamp) or `QUIET` (timed out).
3. `switch (state)` and apply your table.
4. On a confirmed release: `held = release_time - press_time`, classify it as SHORT or LONG, then `xQueueSend(event_q, ...)` with timeout `0`, and log a warning if the queue is full.

Pitfalls:
- Convert ms to ticks for the receive timeout: `pdMS_TO_TICKS(...)`. With 100 Hz, `pdMS_TO_TICKS(25)` = 2 ticks = 20 ms. Check what your debounce becomes.
- Time differences: `now - start` with unsigned types handles overflow, but `now > start + x` doesn't. `esp_timer_get_time()` returns `int64_t` µs, so it won't overflow in your lifetime, but know the rule anyway.
- The ISR only *enqueues*. No logic, no logging, no `gpio_get_level()` (see the sdkconfig table).

✅ **Test before continuing:**
- 10 fast taps should give exactly 10 `SHORT`.
- A 1 s hold should give exactly 1 `LONG`.
- Holding the button and wiggling it slightly should still give exactly 1 event.

---

## Milestone 4: Safe FSM (Consumer)

### 4a. Represent the FSM
Two options:
- **`switch` / `case`**: easy to read for small machines.
- **Transition table**: a 2D array `next_state[state][event]`. Your table from 1a becomes code almost 1:1. Add `const char*` name arrays for states and events so logs are readable. This is the more "professional" option and good to show in review.

### 4b. The timeout trick (the core of the homework)
You don't need a separate timer. You've already used this trick in the button task:

> If `xQueueReceive(event_q, &ev, 3 s)` times out, treat it as a `TIMEOUT` event and feed it into the same FSM.

Now "the user got stuck for 3 s" is just one more column in your table.

Bonus thinking:
- Should the timeout be the same in every state? A small function `timeout_for(state)` handles this (initial: forever, unlocked: auto-lock after 5 s, others: 3 s).
- The 3 s is measured from the last **release** (that's when the event arrives). A long hold takes time too. Is that OK? Document your decision.
- After a reset, should presses already sitting in `event_q` count? (Look up `xQueueReset`.)

### 4c. Actions
- Every transition: log `OLD --EVENT--> NEW`.
- Entering UNLOCKED: green LED on.
- Reset: log the reason (wrong input or timeout) and flash red.

✅ **Self-check:**
- Does only the ISR and the button task touch the button GPIO?
- Does the safe task contain no timing logic except the receive timeout?

---

## Milestone 5: `app_main` + test + README

### `app_main`
1. Create both queues and check them for `NULL`.
2. Install the ISR service and add the handler. Check the `esp_err_t` results (`ESP_ERROR_CHECK`).
3. Create both tasks and check for `pdPASS`.
4. Priorities: which task must react fastest to the ISR? (Hint: the button task, because the QUIET timeout only works if it runs soon after each edge.)
5. Add `static_assert`s (debounce < long threshold, priorities < `configMAX_PRIORITIES`, ...).

Think about the order: what happens if an edge fires *before* `raw_q` exists?

### Test plan (put it in the README)

| # | Input | Expected |
|---|---|---|
| 1 | S S L | UNLOCKED |
| 2 | L | stays at initial, reset logged |
| 3 | S L | reset |
| 4 | S S S | reset (not unlocked) |
| 5 | S, wait 4 s | TIMEOUT → reset |
| 6 | S S, wait 4 s, L | timeout reset, then L alone does nothing |
| 7 | S S L while already unlocked | your chosen behavior |
| 8 | 10 fast taps | exactly 10 SHORT (debounce works) |

Run every row on the real board and paste the serial log.

### Deliverables
- `src/main.c`
- `README.md` with:
  - both FSM diagrams or tables
  - an architecture picture (ISR → raw_q → button task → event_q → safe task)
  - chosen timings and **why** (from your Milestone 2 bounce measurements)
  - the design decisions (3 s counted from release, behavior when unlocked, the `gpio_get_level` / IRAM note)
  - the test log

---

### Optional advanced variant (only after everything works)
The ISR disables its own interrupt (`gpio_intr_disable`) and starts a one-shot `esp_timer`. The timer callback reads the now-stable level, emits the event and re-enables the interrupt. This gives fewer wakeups and no `raw_q`, but it's harder to get right. Compare the two approaches in the README.

### Concepts to google if stuck
`deferred interrupt processing FreeRTOS`, `ESP-IDF gpio_isr_handler_add example`, `portYIELD_FROM_ISR`, `IRAM_ATTR ESP32`, `table-driven state machine C`, `button debounce state machine`.

When you have code, send it to me and I'll review it like the reviewer would, without rewriting it for you.
