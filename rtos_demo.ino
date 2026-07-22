/*
 * ================================================================
 *  Real-Time Operating System (RTOS) Demo - ESP32 FreeRTOS
 * ================================================================
 * WHAT THIS PROJECT DOES
 * -----------------------
 * The ESP32 already runs FreeRTOS under the hood (Arduino's
 * setup()/loop() is itself just one FreeRTOS task). This project
 * makes that explicit by creating several independent tasks that
 * run concurrently across the ESP32's two cores, and demonstrates
 * the core RTOS primitives:
 *
 *   - Tasks pinned to specific cores (xTaskCreatePinnedToCore)
 *   - A Queue for passing sensor data between tasks safely
 *   - A Mutex (binary semaphore) protecting a shared resource
 *     (Serial output) from being garbled by two tasks writing at once
 *   - A counting Semaphore modeling a limited resource pool
 *     (e.g., "only 2 tasks may access the SPI bus at once")
 *   - Task priorities, so a critical "alarm" task always pre-empts
 *     lower-priority housekeeping work
 *   - A software timer for a periodic heartbeat, independent of any task loop
 *
 * ARCHITECTURE
 * ------------
 *   [SensorTask] --(Queue)--> [ProcessTask] --(Queue)--> [LoggerTask]
 *        |                          |                          |
 *        +------ shared Serial access, protected by Mutex ------+
 *
 *   [AlarmTask] (high priority) can preempt everything else when a
 *   simulated threshold is crossed.
 *
 *   [ResourceUserTask x3] compete for a counting semaphore that only
 *   allows 2 concurrent "resource users" at a time, demonstrating
 *   semaphore-based resource limiting (classic bounded-resource problem).
 *
 * HOW IT WORKS (read this before your interview!)
 * --------------------------------------------------
 *   - xTaskCreatePinnedToCore(...) creates a task and assigns it to
 *     core 0 or 1. ESP32 Arduino core normally runs WiFi/BT stack on
 *     core 0 and the sketch's loop() on core 1 -- we intentionally
 *     spread our tasks to show real parallelism.
 *   - xQueueSend/xQueueReceive move data between tasks without
 *     shared-memory race conditions -- the queue itself is
 *     thread-safe (implemented with internal critical sections).
 *   - A Mutex (xSemaphoreCreateMutex) is a binary semaphore with
 *     PRIORITY INHERITANCE, meant specifically for protecting a
 *     shared resource like Serial -- prevents priority inversion.
 *   - A counting Semaphore (xSemaphoreCreateCounting) has no
 *     inheritance and models "N interchangeable resources available"
 *     rather than "one resource, mutually exclusive."
 *   - vTaskDelay(pdMS_TO_TICKS(ms)) yields the CPU for that duration
 *     instead of busy-waiting, letting other tasks/the idle task run
 *     (important for the watchdog timer and power efficiency).
 * ================================================================
 */

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"

// ---------------- SHARED HANDLES ----------------
QueueHandle_t sensorToProcessQueue;
QueueHandle_t processToLoggerQueue;

SemaphoreHandle_t serialMutex;          // protects Serial.print calls
SemaphoreHandle_t resourceSemaphore;    // counting semaphore, models 2 slots

TimerHandle_t heartbeatTimer;

// ---------------- DATA STRUCTS PASSED THROUGH QUEUES ----------------
typedef struct {
  uint32_t sampleId;
  float rawValue;
} SensorSample;

typedef struct {
  uint32_t sampleId;
  float processedValue;
  bool alarmFlag;
} ProcessedSample;

// A simulated "danger" threshold to demonstrate the high-priority AlarmTask
const float ALARM_THRESHOLD = 90.0;

// ---------------- HELPER: THREAD-SAFE PRINT ----------------
void safePrint(const String &msg) {
  // Take the mutex before touching the shared Serial resource.
  // portMAX_DELAY = wait indefinitely for the mutex to be free.
  if (xSemaphoreTake(serialMutex, portMAX_DELAY) == pdTRUE) {
    Serial.println(msg);
    xSemaphoreGive(serialMutex);
  }
}

// ---------------- TASK: SensorTask (Core 1) ----------------
// Simulates reading a sensor every 500ms and pushes it into a queue.
void SensorTask(void *pvParameters) {
  uint32_t id = 0;
  for (;;) {
    SensorSample sample;
    sample.sampleId = id++;
    // Simulate a noisy signal that occasionally spikes toward "danger"
    sample.rawValue = 40.0 + (float)(random(0, 6000)) / 100.0; // 40-100 range

    if (xQueueSend(sensorToProcessQueue, &sample, pdMS_TO_TICKS(100)) != pdTRUE) {
      safePrint("[SensorTask] WARNING: queue full, sample dropped");
    }

    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

// ---------------- TASK: ProcessTask (Core 1) ----------------
// Applies a simple filter/scaling, decides if alarm should fire,
// forwards result to LoggerTask.
void ProcessTask(void *pvParameters) {
  SensorSample incoming;
  for (;;) {
    if (xQueueReceive(sensorToProcessQueue, &incoming, portMAX_DELAY) == pdTRUE) {
      ProcessedSample result;
      result.sampleId = incoming.sampleId;
      result.processedValue = incoming.rawValue; // (placeholder for real filtering)
      result.alarmFlag = (incoming.rawValue >= ALARM_THRESHOLD);

      xQueueSend(processToLoggerQueue, &result, pdMS_TO_TICKS(100));

      // If this sample crosses the threshold, notify the high-priority
      // AlarmTask directly via its own queue so it can preempt and
      // react immediately, independent of LoggerTask's pace.
      if (result.alarmFlag) {
        bool alarmSignal = true;
        xQueueSend(alarmQueue, &alarmSignal, 0);
      }
    }
  }
}

// ---------------- TASK: LoggerTask (Core 0) ----------------
// Prints processed results. Uses the mutex-protected safePrint().
void LoggerTask(void *pvParameters) {
  ProcessedSample incoming;
  for (;;) {
    if (xQueueReceive(processToLoggerQueue, &incoming, portMAX_DELAY) == pdTRUE) {
      String line = "[LoggerTask] sample #" + String(incoming.sampleId) +
                    " value=" + String(incoming.processedValue, 2);
      safePrint(line);
    }
  }
}

// ---------------- TASK: AlarmTask (Core 0, HIGH PRIORITY) ----------------
// Watches the same processed stream via a second peek mechanism --
// in this simplified demo it just monitors the last value shared
// through a volatile flag written by ProcessTask indirectly. To keep
// this genuinely RTOS-relevant, we instead give AlarmTask its own
// higher-priority check by re-deriving alarm state from a dedicated
// queue so it can preempt LoggerTask/ProcessTask instantly.
QueueHandle_t alarmQueue;

void AlarmTask(void *pvParameters) {
  bool alarmState;
  for (;;) {
    if (xQueueReceive(alarmQueue, &alarmState, portMAX_DELAY) == pdTRUE) {
      if (alarmState) {
        // High priority task pre-empts lower priority tasks immediately
        safePrint("  !!! [AlarmTask] THRESHOLD EXCEEDED - simulated alert !!!");
      }
    }
  }
}

// ---------------- TASK: ResourceUserTask (models bounded resource) ----------------
// Three tasks compete for a counting semaphore with only 2 "slots",
// demonstrating semaphore-based resource limiting.
void ResourceUserTask(void *pvParameters) {
  int taskNum = (int)(intptr_t)pvParameters;
  for (;;) {
    safePrint("[ResourceUser" + String(taskNum) + "] waiting for resource slot...");
    if (xSemaphoreTake(resourceSemaphore, portMAX_DELAY) == pdTRUE) {
      safePrint("[ResourceUser" + String(taskNum) + "] ACQUIRED slot, working...");
      vTaskDelay(pdMS_TO_TICKS(1500 + taskNum * 200)); // simulate work
      safePrint("[ResourceUser" + String(taskNum) + "] releasing slot");
      xSemaphoreGive(resourceSemaphore);
    }
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

// ---------------- TIMER CALLBACK: Heartbeat ----------------
// Software timers run in a dedicated "Timer Service Task" -- they
// are NOT the same as a hardware task, but a convenient way to run
// short periodic callbacks without dedicating a whole task/stack.
void heartbeatCallback(TimerHandle_t xTimer) {
  safePrint("[Heartbeat] system alive, tick=" + String(xTaskGetTickCount()));
}

void setup() {
  Serial.begin(115200);
  delay(500);
  randomSeed(analogRead(0));

  Serial.println("=== FreeRTOS Multi-Task Demo on ESP32 ===");
  Serial.printf("Chip has %d cores, running at %d MHz\n",
                ESP.getChipCores(), ESP.getCpuFreqMHz());

  // ---- Create synchronization primitives ----
  sensorToProcessQueue = xQueueCreate(10, sizeof(SensorSample));
  processToLoggerQueue = xQueueCreate(10, sizeof(ProcessedSample));
  alarmQueue            = xQueueCreate(10, sizeof(bool));

  serialMutex = xSemaphoreCreateMutex();
  resourceSemaphore = xSemaphoreCreateCounting(2, 2); // max 2, initially 2 available

  // ---- Create tasks, pinned across both cores ----
  // Parameters: function, name, stack size (words), param, priority, handle, core
  xTaskCreatePinnedToCore(SensorTask,  "SensorTask",  4096, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(ProcessTask, "ProcessTask", 4096, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(LoggerTask,  "LoggerTask",  4096, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(AlarmTask,   "AlarmTask",   4096, NULL, 4, NULL, 0); // highest priority

  xTaskCreatePinnedToCore(ResourceUserTask, "ResUser1", 2048, (void*)1, 1, NULL, 0);
  xTaskCreatePinnedToCore(ResourceUserTask, "ResUser2", 2048, (void*)2, 1, NULL, 0);
  xTaskCreatePinnedToCore(ResourceUserTask, "ResUser3", 2048, (void*)3, 1, NULL, 1);

  // ---- Software timer: heartbeat every 3s, auto-reload ----
  heartbeatTimer = xTimerCreate("Heartbeat", pdMS_TO_TICKS(3000), pdTRUE, NULL, heartbeatCallback);
  xTimerStart(heartbeatTimer, 0);

  Serial.println("All tasks and timers started.");
}

void loop() {
  // In a pure-RTOS design, loop() itself is just the lowest-priority
  // Arduino task (it runs in Task "loopTask"). All real work happens
  // in the dedicated tasks created in setup(), so we just idle here.
  vTaskDelay(pdMS_TO_TICKS(1000));
}
