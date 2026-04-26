/* --------------------------------------------------------------
   Application: 06 - Real-Time Proof of Concept
   Class: Real Time Systems - Spring 2026
   Author: Kyle Coutray
   Email: kyle.coutray@ucf.edu
   Company: Lockheed Martin (Orlando)
   Website: kylecoutray.com
   AI Use: I used ChatGPT 5.4 as an engineering assistant, not as a code dump tool.
   I used it to compare task structures, reason through synchronization choices,
   think through the ISR -> task pattern, decide how to track latency and deadline
   misses, and tighten parts of the README analysis. I still chose the final
   architecture, wiring, timing values, and proof format, and I reviewed the
   final code paths myself. Chat reference:
   https://chatgpt.com/share/69ed08a7-62e0-83ea-acb2-02a3c8602e54

  Theme: Pilot Training Flight-Control Proof of Concept
  This project simulates a timing-sensitive flight-control support node for an
  Orlando aerospace training rig. The potentiometer is the pilot yoke-position
  sensor, the pushbutton is an emergency override interrupt, the green LED is
  the scheduler heartbeat, the red LED is the hard-fault indicator, and the
  blue LED shows when override mode is active.
---------------------------------------------------------------*/

/*
AI-Generated AI ASSISTANCE SUMMARY:

ChatGPT was used for high-level system design and reasoning, including:
- Task structure and separation (sensor, control, safety, telemetry, etc.)
- Synchronization choices (ISR → task signaling, queues, semaphores)
- Deterministic data flow (latest-sample model)
- Deadline monitoring and fault handling
- Latency measurement and telemetry design
- Interpreting the heartbeat as a scheduler health indicator

All code and final implementation decisions were written and validated manually.

https://chatgpt.com/share/69ed08a7-62e0-83ea-acb2-02a3c8602e54
*/

#include <stdarg.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#define LED_HEARTBEAT 5
#define LED_FAULT 4
#define LED_OVERRIDE 19
#define BUTTON_PIN 18
#define YOKE_SENSOR_PIN 34

// Task timing map for the Lockheed Martin trainer prototype
#define INPUT_PERIOD_MS 20
#define INPUT_DEADLINE_MS 20

#define CONTROL_PERIOD_MS 20
#define CONTROL_DEADLINE_MS 20

#define SAFETY_PERIOD_MS 25
#define SAFETY_DEADLINE_MS 25

#define RENDER_PERIOD_MS 80
#define RENDER_DEADLINE_MS 80

#define TELEMETRY_PERIOD_MS 100
#define TELEMETRY_DEADLINE_MS 100

#define HEARTBEAT_PERIOD_MS 500
#define HEARTBEAT_DEADLINE_MS 500

#define PROOF_LOG_PERIOD_MS 1000

#define CONTROL_QUEUE_LENGTH 1
#define OVERRIDE_DEBOUNCE_MS 120
#define OVERRIDE_THRESHOLD 3400
#define FAULT_THRESHOLD 3600
#define SAFE_EXIT_THRESHOLD 3300

enum FlightMode {
  MODE_STABLE = 0,
  MODE_OVERRIDE = 1
};

struct ControlSample {
  int rawValue;
  int commandedSurface;
  bool overrideRequested;
  uint32_t sensorTimeMs;
};

struct FlightState {
  FlightMode mode;
  int latestRawValue;
  int latestSurfaceCommand;
  uint32_t sampleCount;
  uint32_t queueDrops;
  uint32_t overrideCount;
  uint32_t hardDeadlineMisses;
  uint32_t softDeadlineMisses;
  uint32_t lastInputLatencyMs;
  uint32_t worstInputLatencyMs;
  uint32_t lastRenderCostMs;
  bool hardFaultLatched;
};

SemaphoreHandle_t overrideSem;
SemaphoreHandle_t stateMutex;
SemaphoreHandle_t printMutex;
QueueHandle_t controlQueue;

portMUX_TYPE isrMux = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t isrCount = 0;

FlightState state = {
  MODE_STABLE,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  false
};

void logSync(const char *fmt, ...) {
  char buffer[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);

  xSemaphoreTake(printMutex, portMAX_DELAY);
  Serial.println(buffer);
  xSemaphoreGive(printMutex);
}

FlightMode getMode() {
  FlightMode mode;
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  mode = state.mode;
  xSemaphoreGive(stateMutex);
  return mode;
}

void updateMode(FlightMode mode) {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  state.mode = mode;
  xSemaphoreGive(stateMutex);
}

FlightState snapshotState() {
  FlightState copy;
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  copy = state;
  xSemaphoreGive(stateMutex);
  return copy;
}

void noteQueueDrop() {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  state.queueDrops++;
  xSemaphoreGive(stateMutex);
}

void noteDeadlineResult(bool hardTask, uint32_t runtimeMs, uint32_t deadlineMs, const char *taskName) {
  if (runtimeMs <= deadlineMs) {
    return;
  }

  xSemaphoreTake(stateMutex, portMAX_DELAY);
  if (hardTask) {
    state.hardDeadlineMisses++;
    state.hardFaultLatched = true;
  } else {
    state.softDeadlineMisses++;
  }
  xSemaphoreGive(stateMutex);

  logSync("[%s] Deadline miss runtime=%lu ms deadline=%lu ms",
    taskName,
    (unsigned long) runtimeMs,
    (unsigned long) deadlineMs);
}

void IRAM_ATTR overrideButtonIsr() {
  BaseType_t higherPriorityTaskWoken = pdFALSE;

  // keep ISR short, just count it and wake the task
  portENTER_CRITICAL_ISR(&isrMux);
  isrCount++;
  portEXIT_CRITICAL_ISR(&isrMux);

  xSemaphoreGiveFromISR(overrideSem, &higherPriorityTaskWoken);
  if (higherPriorityTaskWoken == pdTRUE) {
    portYIELD_FROM_ISR();
  }
}

// Hard task: period 20 ms, deadline 20 ms. Samples the yoke sensor predictably.
void sensorInputTask(void *parameter) {
  TickType_t lastWake = xTaskGetTickCount();

  while (1) {
    uint32_t startMs = millis();
    ControlSample sample;

    sample.rawValue = analogRead(YOKE_SENSOR_PIN);
    sample.commandedSurface = map(sample.rawValue, 0, 4095, -30, 30);
    sample.overrideRequested = getMode() == MODE_OVERRIDE;
    // timestamp here so control task can compute end-to-end latency
    sample.sensorTimeMs = millis();

    // AI-use note: queue latest-vs-buffered tradeoff was discussed in the linked chat.
    // I kept only the newest sample because stale control data is worse for this setup.
    if (xQueueOverwrite(controlQueue, &sample) != pdPASS) {
      noteQueueDrop();
    }

    xSemaphoreTake(stateMutex, portMAX_DELAY);
    state.latestRawValue = sample.rawValue;
    state.sampleCount++;
    xSemaphoreGive(stateMutex);

    uint32_t runtimeMs = millis() - startMs;
    noteDeadlineResult(true, runtimeMs, INPUT_DEADLINE_MS, "Input");
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(INPUT_PERIOD_MS));
  }
}

// Hard task: period 20 ms, deadline 20 ms. Converts sensor data into control output.
void flightControlTask(void *parameter) {
  ControlSample sample = {0, 0, false, 0};

  while (1) {
    uint32_t startMs = millis();

    if (xQueueReceive(controlQueue, &sample, pdMS_TO_TICKS(CONTROL_PERIOD_MS)) == pdPASS) {
      int command = sample.commandedSurface;
      if (sample.overrideRequested) {
        command = constrain(command / 2, -15, 15);
      }

      uint32_t endMs = millis();
      // this is the actual sensor -> control latency I wanted to prove
      uint32_t latency = endMs - sample.sensorTimeMs;

      xSemaphoreTake(stateMutex, portMAX_DELAY);
      state.latestSurfaceCommand = command;
      state.lastInputLatencyMs = latency;
      if (latency > state.worstInputLatencyMs) {
        state.worstInputLatencyMs = latency;
      }
      xSemaphoreGive(stateMutex);

      if (latency > CONTROL_DEADLINE_MS) {
        noteDeadlineResult(true, latency, CONTROL_DEADLINE_MS, "FlightControl");
      }
    }

    uint32_t runtimeMs = millis() - startMs;
    noteDeadlineResult(true, runtimeMs, CONTROL_DEADLINE_MS, "FlightControl");
  }
}

// Hard task: period 25 ms, deadline 25 ms. Trips the fault LED if control demand becomes unsafe.
void safetyTask(void *parameter) {
  TickType_t lastWake = xTaskGetTickCount();

  while (1) {
    uint32_t startMs = millis();
    FlightState snap = snapshotState();

    bool hardFault = snap.latestRawValue >= FAULT_THRESHOLD;
    if (!hardFault && snap.mode == MODE_OVERRIDE && snap.latestRawValue >= OVERRIDE_THRESHOLD) {
      hardFault = true;
    }

    xSemaphoreTake(stateMutex, portMAX_DELAY);
    if (hardFault) {
      state.hardFaultLatched = true;
    } else if (snap.latestRawValue <= SAFE_EXIT_THRESHOLD) {
      state.hardFaultLatched = false;
    }
    bool faultOutput = state.hardFaultLatched;
    xSemaphoreGive(stateMutex);

    digitalWrite(LED_FAULT, faultOutput ? HIGH : LOW);

    uint32_t runtimeMs = millis() - startMs;
    noteDeadlineResult(true, runtimeMs, SAFETY_DEADLINE_MS, "Safety");
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(SAFETY_PERIOD_MS));
  }
}

// Soft task: period 80 ms, deadline 80 ms. Variable load simulates AR symbology rendering.
void renderTask(void *parameter) {
  TickType_t lastWake = xTaskGetTickCount();

  while (1) {
    uint32_t startMs = millis();
    FlightState snap = snapshotState();

    int extraRenderMs = map(snap.latestRawValue, 0, 4095, 6, 24);
    if (snap.mode == MODE_OVERRIDE) {
      extraRenderMs += 14;
    }

    digitalWrite(LED_OVERRIDE, snap.mode == MODE_OVERRIDE ? HIGH : LOW);
    vTaskDelay(pdMS_TO_TICKS(extraRenderMs));

    uint32_t runtimeMs = millis() - startMs;

    xSemaphoreTake(stateMutex, portMAX_DELAY);
    state.lastRenderCostMs = runtimeMs;
    xSemaphoreGive(stateMutex);

    noteDeadlineResult(false, runtimeMs, RENDER_DEADLINE_MS, "Render");
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(RENDER_PERIOD_MS));
  }
}

// Soft task: period 100 ms, deadline 100 ms. Pushes timing evidence outward over UART.
void telemetryTask(void *parameter) {
  TickType_t lastWake = xTaskGetTickCount();
  TickType_t lastProofLog = 0;

  while (1) {
    uint32_t startMs = millis();
    FlightState snap = snapshotState();
    uint32_t localIsrCount;

    portENTER_CRITICAL(&isrMux);
    localIsrCount = isrCount;
    portEXIT_CRITICAL(&isrMux);

    // proof log is slow on purpose so the serial output stays readable
    if ((xTaskGetTickCount() - lastProofLog) >= pdMS_TO_TICKS(PROOF_LOG_PERIOD_MS)) {
      logSync(
        "[Proof t=%lu ms] mode=%s inputLat=%lu ms worstInputLat=%lu ms renderCost=%lu ms hardMiss=%lu softMiss=%lu qDrops=%lu isrCount=%lu",
        (unsigned long) millis(),
        snap.mode == MODE_OVERRIDE ? "OVERRIDE" : "STABLE",
        (unsigned long) snap.lastInputLatencyMs,
        (unsigned long) snap.worstInputLatencyMs,
        (unsigned long) snap.lastRenderCostMs,
        (unsigned long) snap.hardDeadlineMisses,
        (unsigned long) snap.softDeadlineMisses,
        (unsigned long) snap.queueDrops,
        (unsigned long) localIsrCount);
      lastProofLog = xTaskGetTickCount();
    }

    uint32_t runtimeMs = millis() - startMs;
    noteDeadlineResult(false, runtimeMs, TELEMETRY_DEADLINE_MS, "Telemetry");
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(TELEMETRY_PERIOD_MS));
  }
}

// Soft task: period 500 ms, deadline 500 ms. Visible liveness indicator for determinism proof.
void heartbeatTask(void *parameter) {
  TickType_t lastWake = xTaskGetTickCount();
  bool ledState = false;

  while (1) {
    uint32_t startMs = millis();
    ledState = !ledState;
    // not just blinking for fun, this shows scheduler is still alive visually
    digitalWrite(LED_HEARTBEAT, ledState ? HIGH : LOW);

    uint32_t runtimeMs = millis() - startMs;
    noteDeadlineResult(false, runtimeMs, HEARTBEAT_DEADLINE_MS, "Heartbeat");
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(HEARTBEAT_PERIOD_MS));
  }
}

// ISR service task: responds to the pilot's emergency override request from the button interrupt.
void overrideManagerTask(void *parameter) {
  TickType_t lastAcceptedTick = 0;

  while (1) {
    if (xSemaphoreTake(overrideSem, portMAX_DELAY) == pdTRUE) {
      TickType_t now = xTaskGetTickCount();
      // quick debounce in task context instead of doing it in the ISR
      if ((now - lastAcceptedTick) < pdMS_TO_TICKS(OVERRIDE_DEBOUNCE_MS)) {
        continue;
      }
      lastAcceptedTick = now;

      FlightMode nextMode = getMode() == MODE_STABLE ? MODE_OVERRIDE : MODE_STABLE;
      updateMode(nextMode);

      xSemaphoreTake(stateMutex, portMAX_DELAY);
      state.overrideCount++;
      xSemaphoreGive(stateMutex);

      logSync("[ISR->Task t=%lu ms] Override button switched mode to %s",
        (unsigned long) millis(),
        nextMode == MODE_OVERRIDE ? "OVERRIDE" : "STABLE");
    }
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(LED_HEARTBEAT, OUTPUT);
  pinMode(LED_FAULT, OUTPUT);
  pinMode(LED_OVERRIDE, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  analogReadResolution(12);
  digitalWrite(LED_HEARTBEAT, LOW);
  digitalWrite(LED_FAULT, LOW);
  digitalWrite(LED_OVERRIDE, LOW);

  overrideSem = xSemaphoreCreateBinary();
  stateMutex = xSemaphoreCreateMutex();
  printMutex = xSemaphoreCreateMutex();
  controlQueue = xQueueCreate(CONTROL_QUEUE_LENGTH, sizeof(ControlSample));

  logSync("[Boot] Lockheed Martin trainer controller starting");
  logSync("[Boot] Hard tasks: Input 20 ms, FlightControl 20 ms, Safety 25 ms");
  logSync("[Boot] Soft tasks: Render 80 ms, Telemetry 100 ms, Heartbeat 500 ms");
  logSync("[Boot] Proof line every 1000 ms; deadline misses print immediately");
  logSync("[Boot] External link: UART serial monitor");

  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), overrideButtonIsr, FALLING);

  xTaskCreate(sensorInputTask, "INPUT", 3072, NULL, 4, NULL);
  xTaskCreate(flightControlTask, "CTRL", 3072, NULL, 3, NULL);
  xTaskCreate(safetyTask, "SAFETY", 3072, NULL, 3, NULL);
  xTaskCreate(renderTask, "RENDER", 3072, NULL, 1, NULL);
  xTaskCreate(telemetryTask, "TELEM", 4096, NULL, 1, NULL);
  xTaskCreate(heartbeatTask, "HEART", 3072, NULL, 1, NULL);
  xTaskCreate(overrideManagerTask, "OVERRIDE", 3072, NULL, 2, NULL);
}

void loop() {
  delay(10);
}
