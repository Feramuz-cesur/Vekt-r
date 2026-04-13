# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**Vector V2** is an ESP32-based wheeled robot with expressive animated eyes, WiFi-controlled via a WebSocket web interface. Written in C++ using PlatformIO + Arduino framework.

## Build & Deploy Commands

```bash
platformio run                        # Build
platformio run --target upload        # Build and flash to ESP32
platformio run --target monitor       # Serial monitor (115200 baud)
platformio run --target uploadfs      # Upload web UI from /data/ to SPIFFS
platformio run --target clean         # Clean build artifacts
```

There are no automated tests. The project targets **ESP32 DoIT DevKit v1**.

## Architecture

All firmware logic lives in a single file: `src/main.cpp` (~815 lines).

The firmware runs **three concurrent FreeRTOS tasks**:

| Task | Core | Priority | Responsibility |
|------|------|----------|---------------|
| `Task_Gozler` | 1 | 2 | Eye animation via RoboEyes, ~100fps |
| `Task_Mantik` | 1 | 1 | Main control loop: touch sensor, mood cycling, LED effects, battery monitoring, ~50Hz |
| `Task_Network` | 0 | 1 | WebSocket cleanup + OTA updates, ~20Hz |

### Data Flow

Web UI (`data/index.html`) sends JSON commands over WebSocket → `handleWebSocketMessage()` in `main.cpp` routes them to shared global state → `Task_Mantik` applies motor/servo/LED changes, `Task_Gozler` renders eye animations → battery % is broadcast back to the UI every 5 seconds.

### Key Subsystems

- **Motors:** 4 DC motors on pins 32, 33, 25, 26
- **Servos:** 2 servos on pins 27 (arm) and 14 (head)
- **OLED Display:** Adafruit SH1106G 128x64 (I2C 0x3c) — renders animated eyes
- **Audio:** DFRobotDFPlayerMini on Serial2 (RX=4, TX=5), 7 MP3 tracks
- **LEDs:** 8x WS2812B NeoPixel strip on pin 23 (modes: OFF, STATIC, RAINBOW, PULSE, POLICE)
- **Battery:** ADC on pin 34 (45K+45K voltage divider), mapped via discharge curve
- **Touch:** Pin 13, 3-sec hold displays IP address on OLED

### Eye Animation

Uses the custom `RoboEyes` library in `lib/RoboEyes-main/FluxGarage_RoboEyes.h`. Moods are set via `eyes.setMood()` and expressions via `eyes.setExpression()`. The library runs in `Task_Gozler` independently so eyes animate smoothly regardless of network/logic load.

### Web UI

`data/index.html` is a self-contained ~2000-line HTML/JS file (Tailwind CSS, Font Awesome) served from SPIFFS. It connects via WebSocket and provides: directional pad, speed/servo sliders, 6 mood buttons, 7 dance routines, LED mode/color picker, and volume control.

## Important Notes

- WiFi credentials are hardcoded in `main.cpp` (lines ~54-55) — update before flashing to a new network
- NTP is configured for Turkish timezone (UTC+3)
- The `data/` directory must be uploaded to SPIFFS separately via `uploadfs` whenever the web UI changes
- Dance routines (7 total) are blocking sequences that coordinate motors, servos, and LEDs — they run inside `Task_Mantik`
- `old_files/` contains previous UI iterations and can be ignored
