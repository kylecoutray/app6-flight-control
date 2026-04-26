# Application 6 – Lockheed Martin Flight-Control Trainer

## AI Use Note

I used ChatGPT 5.4 as a support tool, not as a code dump tool. It was used to compare task structures, reason through synchronization choices, think through the ISR-to-task handoff, decide how to track latency and deadline misses. I selected the Orlando aerospace scenario, verified the final code paths, kept the timing map aligned to the assignment, and made all final decisions regarding the task set, ISR behavior, wiring, and system claims.

I also used it to clean up the readme. See here: 
https://chatgpt.com/share/69ed087b-2254-83ea-9d97-8c65197ae2aa

## AI Use Disclosure

* **Tool used:** ChatGPT 5.4
* **How it was used:**

  * Brainstorming task structure
  * Comparing synchronization options
  * Reasoning about a minimal ISR with a binary semaphore to a task
  * Discussing latest-sample queue behavior
  * Reviewing a lightweight deadline-miss mechanism
  * Thinking through sensor-to-control timestamp propagation
  * Refining the periodic proof log
  * Improving README wording and formatting
* **What I retained ownership of:**

  * Final architecture
  * Wiring
  * Company scenario
  * Deadlines
  * Timing proof interpretation
  * All final code decisions
* **Chat/session URL:** [https://chatgpt.com/share/69ed08a7-62e0-83ea-acb2-02a3c8602e54](https://chatgpt.com/share/69ed08a7-62e0-83ea-acb2-02a3c8602e54)
* **Scope reflected:** Task structure, synchronization choices, deadline logging, latency tracking, proof logging, and heartbeat purpose

## Author

* **Name:** Kyle Coutray
* **Company Context:** Lockheed Martin, Orlando
* **Product Theme:** Pilot training and flight-control support node
* **Platform:** ESP32 + Arduino + native FreeRTOS + Wokwi
* **Concurrency Diagram:** https://docs.google.com/presentation/d/1yul4Idd5T0e2X10qZYz7wJAIhIxZDDSGfwXHTLG0H_o/edit?usp=sharing

## Company Synopsis

Lockheed Martin has a major presence in Orlando, where it works on aerospace, defense, simulation, and training systems. That makes it a strong fit for a pilot training flight-control proof of concept. Real-time behavior matters in this kind of system because pilot input, control updates, and safety monitoring all need to respond predictably and on time. If those tasks slip, the simulator can show delayed aircraft response, incorrect control behavior, or unsafe states, so timing matters just as much as correctness.

## Project Overview

This proof of concept models a timing-sensitive subsystem within a Lockheed Martin pilot-training rig in Orlando.

* The **potentiometer** acts as the pilot yoke-position sensor
* The **pushbutton** acts as an emergency override interrupt
* The **green LED** represents scheduler heartbeat
* The **red LED** indicates a hard fault
* The **blue LED** shows override mode status

The firmware uses native FreeRTOS tasks on the ESP32 to separate hard real-time control work from soft visualization and telemetry work. External communication is handled through UART serial output, where the system prints compact proof lines containing timestamps, control latency, render cost, and deadline-miss counters.

**Design goal:** Demonstrate deterministic control and safety behavior even under variable rendering load.

## Hardware Mapping

* Potentiometer → GPIO34 (pilot yoke-position sensor)
* Pushbutton → GPIO18 (emergency override ISR input)
* Green LED → GPIO5 (heartbeat / liveness indicator)
* Red LED → GPIO4 (hard-fault indicator)
* Blue LED → GPIO19 (override-mode status)

## Runtime Design

* **sensorInputTask**
  Hard task, 20 ms period, 20 ms deadline
  Samples the yoke sensor and overwrites the latest command queue item

* **flightControlTask**
  Hard task, 20 ms period, 20 ms deadline
  Converts the latest sensor sample into a control command and measures latency

* **safetyTask**
  Hard task, 25 ms period, 25 ms deadline
  Monitors unsafe ranges and latches the red fault LED

* **renderTask**
  Soft task, 80 ms period, 80 ms deadline
  Simulates AR rendering with variable execution time

* **telemetryTask**
  Soft task, 100 ms period, 100 ms deadline
  Maintains timing evidence and emits proof logs once per second

* **heartbeatTask**
  Soft task, 500 ms period, 500 ms deadline
  Drives visible system liveness

* **overrideButtonIsr**
  Interrupt handler on GPIO18 for emergency override input

* **overrideManagerTask**
  ISR service task that toggles STABLE / OVERRIDE mode via semaphore

## Task Table

| Task / ISR | Period / Trigger | Hard / Soft | Consequence if Missed |
|---|---|---|---|
| sensorInputTask | 20 ms | Hard | Pilot input becomes stale before reaching the control path |
| flightControlTask | 20 ms | Hard | Control output is delayed and trainer response becomes inaccurate |
| safetyTask | 25 ms | Hard | Unsafe input range may not be caught in time and fault response is delayed |
| renderTask | 80 ms | Soft | Visualization or AR overlay lags, but control path still runs |
| telemetryTask | 100 ms | Soft | Proof logging is delayed or skipped, but control and safety still run |
| heartbeatTask | 500 ms | Soft | Visible liveness indicator becomes less reliable |
| overrideManagerTask | Event-driven | Soft | Override mode change is delayed after the interrupt |
| overrideButtonIsr | Event-driven interrupt | ISR support path | Override request is not forwarded quickly to the task layer |

## Synchronization and Communication

* **Queue (controlQueue)**
  One-slot queue from sensorInputTask to flightControlTask
  Uses `xQueueOverwrite()` to ensure only the newest sample is processed

* **Binary Semaphore (overrideSem)**
  Given by ISR, taken by overrideManagerTask
  Separates interrupt context from control logic

* **Mutex (stateMutex)**
  Protects shared state including mode, latency metrics, and deadline counters

* **Critical Section (isrMux)**
  Protects ISR counter access across interrupt and task contexts

* **External Communication**
  UART serial output provides compact proof logs with timestamps and counters

## Determinism Evidence

The system outputs compact proof lines instead of verbose debug logs. Each proof line includes:

* Sensor timestamp
* Control completion time
* Input latency (`inputLat`)
* Worst latency observed
* Deadline miss counters

Expected behavior:

* `inputLat` remains below the 20 ms deadline
* `hardMiss = 0` during correct operation
* Green LED confirms system liveness
* Red LED activates only for faults or missed deadlines

This combination provides both visual and logged evidence of deterministic execution.

## Engineering Analysis

### 1. Scheduler Fit

Hard real-time tasks have the highest priority:

* sensorInputTask → priority 4
* flightControlTask, safetyTask → priority 3
* renderTask → priority 1

This ensures control tasks are never preempted by visualization workloads.

The one-slot queue prevents backlog accumulation and ensures the control loop always operates on the latest data.

Example proof line:

```
[Proof t=1000 ms] ... inputLat=4 ms worstInputLat=6 ms hardMiss=0 ...
```

### 2. Race-Proofing

* Shared state is protected with `stateMutex`
* ISR-to-task communication uses a binary semaphore
* ISR remains minimal and deterministic
* `isrMux` protects shared counters between ISR and telemetry

State updates are handled in task context through helper functions, avoiding unsafe ISR logic.

### 3. Worst-Case Spike

Stress scenario:

* Rapid potentiometer changes
* Repeated override button presses
* Maximum render workload

Even under this load:

* Hard tasks maintain priority
* Queue prevents backlog
* Telemetry reveals latency margin

Example:

* Worst latency: 6 ms
* Remaining margin: ~14 ms before deadline violation

### 4. Design Trade-Off

Excluded features:

* Wi-Fi
* Web dashboards
* Complex streaming

Reason:

* Prioritized deterministic scheduling over interface complexity
* Reduced concurrency overhead
* Maintained predictable execution

Control model simplified to a latest-sample queue, which is more appropriate for real-time control loops.

## Submission Notes

* Determinism proof: UART logs + LED indicators
* renderTask fulfills the variable-time task requirement

---
