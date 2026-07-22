# Real-Time Operating System (RTOS) Demo — ESP32 FreeRTOS

**Author:** Pratham Pathak

**Intern ID:** CITS2620

**Internship:** CodTech IT Solutions Pvt. Ltd.

**Project duration:** 		11 July – 20 July 2026

A multi-task demo that makes the ESP32's underlying FreeRTOS explicit,
covering the primitives you're expected to know for embedded interviews:
tasks, queues, mutexes, counting semaphores, priorities, dual-core
pinning, and software timers.

## Architecture
```
[SensorTask] --Queue--> [ProcessTask] --Queue--> [LoggerTask]
                              |
                          (on threshold)
                              v
                         [AlarmTask]  (highest priority, preempts everything)

[ResourceUser1/2/3] compete for a 2-slot counting semaphore
[Heartbeat] software timer prints every 3s, independent of any task
```

## Tasks created
| Task           | Core | Priority | Role |
|----------------|------|----------|------|
| SensorTask     | 1    | 2        | Simulates a sensor reading every 500ms |
| ProcessTask    | 1    | 2        | Filters/evaluates samples, flags alarms |
| LoggerTask     | 0    | 1        | Prints processed results |
| AlarmTask      | 0    | 4 (highest) | Preempts to print an alert instantly |
| ResourceUser1-3| 0/1  | 1        | Compete for a 2-slot counting semaphore |

## Synchronization primitives used
- **Queue** (`sensorToProcessQueue`, `processToLoggerQueue`, `alarmQueue`):
  thread-safe data hand-off between tasks, no manual locking needed.
- **Mutex** (`serialMutex`): protects shared `Serial` output from
  interleaved/garbled prints across tasks (uses priority inheritance).
- **Counting semaphore** (`resourceSemaphore`, count=2): models a
  bounded resource pool — only 2 of the 3 `ResourceUserTask`s can hold
  the resource at once.
- **Software timer** (`heartbeatTimer`): periodic callback that doesn't
  need a dedicated task/stack.

## Setup
1. Open `rtos_demo.ino` in Arduino IDE, select your ESP32 board.
2. Upload, then open Serial Monitor at 115200 baud.
3. Watch interleaved task output — note how `[AlarmTask]` lines appear
   immediately when a simulated sensor value crosses 90, even mid-stream
   from other tasks, because of its higher priority.

## Talking points for interviews
- Difference between a Mutex (mutual exclusion, priority inheritance,
  "ownership") and a counting Semaphore (signaling / resource counting,
  no ownership concept).
- Why `vTaskDelay()` is used instead of `delay()` — it yields to the
  scheduler instead of busy-waiting, letting other tasks/idle task run.
- What "priority inversion" is and why the mutex (not a raw semaphore)
  is used to protect Serial.
- Why tasks are pinned to specific cores with `xTaskCreatePinnedToCore`
  and what runs on core 0 by default on ESP32 (WiFi/BT stack).
- Stack size considerations — why each task gets its own stack, and
  what happens if it's undersized.

## Possible extensions
- Add a watchdog timer that resets the system if SensorTask stalls.
- Replace simulated sensor values with a real I2C/ADC sensor.
- Feed AlarmTask into the Embedded Web Server project's `/api/status`
  endpoint (queue -> global flag -> JSON field) to connect the two
  projects into one system — a strong addition to a portfolio repo.
