# Public Knowledge

Last updated: 2026-04-27

This file is the required shared handoff document for any AI working in this repository.

## Mandatory AI Workflow

If you are an AI agent operating in this repo, treat the following as required repo policy:

1. Before running any shell command or making any edit, read this file first.
2. The first repo-local command of a new session should be one of:
   - `sed -n '1,220p' public_knowledge.md`
   - `cat public_knowledge.md`
3. Before making code changes, verify whether the current task conflicts with anything recorded here.
4. After every meaningful change to code, configuration, hardware integration, or project status, update this file in the same turn before stopping.
5. Do not overwrite prior progress casually. Preserve history and update the status sections.

Note:
- This document records a required workflow convention for future AI agents.
- It improves continuity, but it is not a hard technical enforcement mechanism by itself.

## Primary Project Goal

Build and maintain a practical ESP32-S3 based MimiClaw firmware that can:

- connect to Wi-Fi reliably
- interact with users through Feishu, Telegram, and WebSocket
- let the AI call narrow hardware tools safely
- control onboard hardware such as the ESP32-S3 board WS2812 RGB LED
- read external sensors such as the SGP30 air-quality sensor over I2C
- avoid unsafe or hallucinated hardware actions when the requested capability does not actually exist

## Current Hardware / Runtime Baseline

- Board: ESP32-S3
- Onboard WS2812 data pin: GPIO48
- SGP30 currently wired and validated on:
  - SDA: GPIO17
  - SCL: GPIO18
- Servo motor: GPIO5 (hard-wired, no pin override), LEDC PWM 50Hz, 13-bit resolution, 500-2500us pulse range
- WebSocket gateway port: `18789`
- Serial port used for flashing and runtime checks: `/dev/ttyUSB0`
- Verified Wi-Fi at runtime on 2026-04-27:
  - SSID: `Redmi K70`
  - Device reported `WiFi connected: yes`
  - Device IP observed: `10.29.203.55`

## Build / Flash Baseline

Important for future AI agents:

- The host ESP-IDF repo at `/home/cube/WorkSpace/ESP/esp-idf` is currently on `v6.1-dev`.
- The project was ported to build with IDF 6.1-dev on 2026-04-27:
  - Removed `json` from CMakeLists REQUIRES (removed in IDF 6.x)
  - Added upstream `cJSON v1.7.15` as `main/cJSON_upstream.c` / `main/cJSON_upstream.h`
  - Changed `cJSON_upstream.c` include to use `cJSON_upstream.h` (project has a custom `cJSON.h` with different signatures)
  - Removed `WIFI_REASON_ASSOC_EXPIRE` case in `wifi_manager.c` (removed from IDF 6.x enum)
  - Build confirmed working: `idf.py build` succeeds, firmware linked
- Flash confirmed working on `/dev/ttyUSB0` with hash verification.
- Do NOT attempt to use `json` component from IDF 5.x — it does not exist in IDF 6.x.

## Current Implemented Capabilities

- Wi-Fi connection and runtime status inspection
- Feishu channel active and sending replies successfully
- Telegram channel code exists, but runtime logs showed no token configured during validation
- WebSocket inbound/outbound chat channel
- WS2812 tools:
  - `set_status_light`
  - `ws2812_set`
- SGP30 tools:
  - `read_air_quality`
  - `sgp30_read_air_quality`
- Servo motor tool:
  - `servo_write` — angle (0-180°) or pulse width (us), GPIO5 only (hard-wired, no pin param)
- Conversation logging: All user-LLM exchanges logged with `=== CONV ===` tag visible on serial monitor
- Serial CLI commands including `config_show`, `wifi_status`, and `tool_exec`
- Automatic SGP30 periodic reading visible in serial logs
- 17 registered agent tools total (including web_search, cron, gpio, files, etc.)

## Safety Improvement Recently Added

Two important behavior-boundary changes were added but are not yet committed:

1. Prompt-side rule update in `main/agent/context_builder.c`
   - unsupported hardware / sensor / actuator requests must be refused
   - do not substitute a nearby tool
   - if unsure, ask for clarification or say the capability is not supported

2. Execution-side tool guard in `main/agent/agent_loop.c`
   - blocks tool execution when the requested user intent does not clearly match the called tool
   - currently covers:
     - board light / WS2812
     - air-quality / SGP30
     - GPIO read / write
     - cron operations

Meaning:

- The system is now materially safer against “wrong but nearby” hardware actions.
- This is an actual execution boundary, not only a prompt suggestion.

## Verified Runtime Behavior On 2026-04-27

The following was verified on real hardware after compile/flash:

- firmware flashed successfully to ESP32-S3
- device booted and serial CLI responded
- `config_show` reported build-time Wi-Fi and model configuration
- `wifi_status` reported connected status and IP address
- Feishu message flow was active
- SGP30 auto-read loop produced stable live data in serial logs
- direct serial CLI servo test succeeded with `tool_exec servo_write {"angle":90}`
- servo runtime log confirmed:
  - `Servo PWM updated on GPIO 5: pulse=1500us duty=614/8191`
  - `tool_exec status: ESP_OK`
  - `OK: servo on GPIO5 set to 90 degrees (pulse=1500us)`
- direct serial CLI sweep test also succeeded:
  - `tool_exec servo_write {"angle":30}` -> `pulse=833us duty=341/8191`
  - `tool_exec servo_write {"angle":150}` -> `pulse=2166us duty=887/8191`
  - `tool_exec servo_write {"angle":90}` -> `pulse=1500us duty=614/8191`

Observed SGP30 sample range during runtime check:

- eCO2 roughly `400` to `413 ppm`
- TVOC roughly `0` to `14 ppb`

## Important Limitation Still Remaining

The new guard logic has been compiled and flashed, but the final end-to-end negative test is still pending:

- Not yet fully verified on-device with a real unsupported user request such as “open the buzzer”.

Reason:

- the existing serial CLI can directly execute tools with `tool_exec`
- but `tool_exec` bypasses the agent decision path and therefore does not test the guard
- current serial CLI does not provide a direct “inject user message into agent loop” command
- WebSocket gateway exists, but host-side connectivity to the board WebSocket endpoint was not yet established during this validation pass

## Recommended Next Step

Best next engineering step:

- add a minimal serial CLI command such as `inject_msg <channel> <chat_id> <text>`
- make it push a real inbound message onto the message bus
- then test both:
  - supported request, for example air-quality query
  - unsupported request, for example buzzer control

Expected outcome after that test:

- supported request should use the correct tool
- unsupported request should refuse cleanly
- no nearby hardware tool should be executed as a substitute

## Current Repo State To Be Aware Of

As of 2026-04-27:

- uncommitted source changes exist in:
  - `main/agent/context_builder.c`
  - `main/agent/agent_loop.c`
  - `main/tools/tool_registry.c`
  - `main/tools/tool_servo.c`
  - `main/tools/tool_servo.h`
  - `main/mimi_config.h`
  - `main/mimi_secrets.h`
  - `main/mimi_secrets.h.example`
  - `main/CMakeLists.txt`
  - `main/cJSON_upstream.c` (new)
  - `main/cJSON_upstream.h` (new)
  - `main/wifi/wifi_manager.c`
  - `public_knowledge.md`
- untracked build directories exist:
  - `build_idf55/`
  - `build_v61/`

Guidance:

- do not accidentally commit build output directories
- do not revert the above source changes unless explicitly asked

## Project Status Summary

- WS2812 control: implemented
- Feishu-triggered hardware control path: implemented
- SGP30 driver and AI-callable read path: implemented
- SGP30 live serial monitoring: implemented and verified
- Servo motor control (GPIO5, LEDC PWM): implemented and registered
- Servo LLM call path: corrected in prompt/schema/guard on 2026-04-27, pending board-side runtime retest
- Servo direct tool path on GPIO5: verified on hardware at 90 degrees
- Wi-Fi reliability for current configured network: working in current validation
- “If unsupported, say unsupported” prompt rule: implemented
- execution-side tool guard: implemented and flashed
- real negative end-to-end unsupported-request test: pending
- GPIO pin allowlist documented in system prompt (prevents LLM hallucination)
- Build ported to ESP-IDF 6.1-dev (cJSON upstream, fixed wifi_manager)
- guard changes committed to git: pending

## Update Log

### 2026-04-27

- Document created as the required shared progress file for future AI sessions.
- Recorded current project goals, validated hardware state, build/flash baseline, and repo workflow requirements.
- Recorded that guard-related source changes are present, flashed, and partially validated, but not yet committed.

### 2026-04-27 (second session)

- Ported project to build with ESP-IDF 6.1-dev: removed `json` REQUIRES, bundled upstream cJSON v1.7.15 as `cJSON_upstream.c/h`, fixed `wifi_manager.c` for removed enum.
- Added servo motor tool `servo_write` on GPIO5 with LEDC PWM (50Hz, 500-2500us).
- Registered servo in tool_registry (17 total tools).
- Fixed LLM hallucination about GPIO5 availability: added explicit allowed pin list (1-18, 21, 38, 46) to system prompt in context_builder.c.
- Build and flash verified successfully on `/dev/ttyUSB0`.
- Confirmed WiFi connection to `Redmi K70`, IP `10.29.203.55`.
- All 17 tools registered and agent loop running.

### 2026-04-27 (third session)

- Fixed `servo_write` tool not being callable by LLM: root cause was `oneOf` in JSON Schema — unsupported by OpenAI/DeepSeek function calling. Changed to standard `required: ["angle"]` in `tool_registry.c`.
- Added Chinese servo keywords to system prompt in `context_builder.c`: 舵机, 旋转, 转动, 角度, 顺时针, 逆时针
- Created `烧录指南.md` — comprehensive flash/burn guide in Chinese covering ESP-IDF setup, two USB ports, `--no-compress` workaround, partition layout, SPIFFS layout, and common issues.
- Built and flashed successfully.

### 2026-04-27 (fourth session)

- Investigated a new issue where the LLM reported servo success but the servo did not visibly move.
- Found prompt/schema consistency gaps:
  - `servo_write` was registered, but missing from the explicit Available Tools list in `context_builder.c`
  - `servo_write` schema still forced `angle`, which made `pulse_us` effectively unavailable to the LLM
  - the new execution-side `tool_guard` had no explicit servo matcher
- Applied fixes:
  - added `servo_write` to the system prompt tool list
  - added servo-specific guard keywords in `agent_loop.c`
  - changed `servo_write` schema to `required: []` so either `angle` or `pulse_us` can be used
  - updated servo implementation logs to include GPIO and PWM duty details
  - aligned `tool_servo.h` comments with the current fixed-pin firmware design
- Local build revalidated successfully with `idf.py build`.
- Direct serial runtime retest was attempted but initially blocked because `/dev/ttyUSB0` was busy from a stale external `screen` holder.
- The stale `screen` holder (`PID 365239`) was cleared, then firmware was rebuilt and flashed successfully to `/dev/ttyUSB0`.
- Flash completed with verified hashes and hard reset.

### 2026-04-27 (fifth session)

- Confirmed again that the servo GPIO in firmware is `GPIO5` via `MIMI_SERVO_DEFAULT_GPIO`.
- Bypassed the LLM and invoked the same registered tool function through serial CLI:
  - `tool_exec servo_write {"angle":90}`
- Real device logs confirmed the tool executed and PWM was updated:
  - `Servo PWM updated on GPIO 5: pulse=1500us duty=614/8191`
  - `tool_exec status: ESP_OK`
  - `OK: servo on GPIO5 set to 90 degrees (pulse=1500us)`

### 2026-04-27 (sixth session)

- Ran a wider direct serial servo sweep to separate “same-angle no visible movement” from “no hardware drive”.
- Commands executed:
  - `tool_exec servo_write {"angle":30}`
  - `tool_exec servo_write {"angle":150}`
  - `tool_exec servo_write {"angle":90}`
- Firmware logs confirmed three distinct PWM outputs on `GPIO5`:
  - `833us` (`duty=341/8191`)
  - `2166us` (`duty=887/8191`)
  - `1500us` (`duty=614/8191`)
