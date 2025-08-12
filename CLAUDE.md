# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is an ESP32-C3 soil moisture monitoring system that reads soil sensor data via ADC, transmits data over WiFi/MQTT, and supports remote pump control. The project is built using ESP-IDF framework and targets the ESP32-C3 Super Mini development board.

**Key Features:**
- Real-time soil moisture monitoring using ADC
- WiFi connectivity for data transmission
- MQTT protocol for IoT communication
- Remote pump control via MQTT commands
- Modular command handling system
- FreeRTOS-based multitasking architecture

## Development Commands

**Build and Flash:**
```bash
# Build the project
idf.py build

# Configure project settings
idf.py menuconfig

# Flash to device
idf.py -p PORT flash

# Monitor serial output
idf.py -p PORT monitor

# Flash and monitor in one command
idf.py -p PORT flash monitor

# Full clean build
idf.py fullclean
idf.py build
```

**Testing:**
```bash
# Run pytest tests
pytest pytest_hello_world.py
```

## Hardware Configuration

**Target Platform:** ESP32-C3 Super Mini
**Key Pin Definitions:**
- Soil Sensor ADC: GPIO0 (ADC_CHANNEL_0)
- Pump Control: GPIO6
- Built-in LED: GPIO8 (inverted logic)

**Calibration Values:**
- AIR_VALUE: 3000 (dry sensor reading)
- WATER_VALUE: 1400 (wet sensor reading)

## Architecture

### Main Components

**main.c:**
- Main application entry point (`app_main()`)
- WiFi initialization and event handling
- MQTT client setup and message handling
- ADC initialization with calibration
- Sensor data reading and JSON formatting
- FreeRTOS task creation and management

**command_handler.c/h:**
- Modular MQTT command processing system
- Command queue and event group management
- Automated watering function (1.5-second pump operation)
- Status reporting and statistics tracking
- Watering count statistics
- OTA update command handling
- Supported commands: 澆水/WATER, GET_STATUS, GET_READING, OTA_UPDATE, OTA_STATUS, OTA_CANCEL

**ota_update.c/h:**
- Over-The-Air (OTA) firmware update functionality
- HTTP/HTTPS firmware download via WiFi
- Automatic version validation and integrity checking
- Progress tracking and MQTT status reporting  
- Automatic device reboot after successful update
- Supports rollback on failed updates

### Communication Protocol

**MQTT Topics:**
- `soilsensorcapture/esp/data` - Sensor data publishing
- `soilsensorcapture/esp/command` - Remote command input
- `soilsensorcapture/esp/status` - System status reports
- `soilsensorcapture/esp/response` - Command responses
- `soilsensorcapture/esp/ota_status` - OTA update progress and status

**Data Format:** JSON with timestamp, voltage, moisture percentage, raw ADC values, GPIO status, and watering statistics

**Watering Command System:**
- Command: "澆水" or "WATER" 
- Function: Automatically turns on pump for 1.5 seconds then turns off
- Response: Immediate start confirmation and completion notification with total watering count
- Statistics: Tracks total watering operations performed

**OTA Update System:**
- Command: "OTA_UPDATE" with firmware URL in command data
- Function: Downloads and installs new firmware over WiFi
- Security: Validates firmware version and integrity before installation
- Progress: Real-time progress reporting via MQTT
- Auto-reboot: Automatically restarts device after successful update
- Additional commands: "OTA_STATUS" (check progress), "OTA_CANCEL" (abort update)

### Build System

Uses ESP-IDF's CMake-based build system with:
- Root `CMakeLists.txt` with project configuration
- Component-level `main/CMakeLists.txt` specifying dependencies
- Key dependencies: driver, esp_wifi, mqtt, nvs_flash, esp_event, json, esp_adc, app_update, esp_http_client, bootloader_support

## Configuration

**WiFi Settings:** Update `WIFI_SSID` and `WIFI_PASS` in main.c
**MQTT Broker:** Currently configured for HiveMQ public broker
**Project Name:** "soilsensorcapture" (defined in root CMakeLists.txt)

## Development Notes

- Code extensively commented in Chinese for educational purposes
- Uses ESP-IDF v5.0+ ADC APIs (adc_oneshot)
- Implements proper error checking with ESP_ERROR_CHECK macros
- FreeRTOS tasks with appropriate stack sizes and priorities
- Memory management with proper cleanup (cJSON_Delete, free)
- Includes comprehensive logging with ESP_LOG macros
- Watering system uses FreeRTOS vTaskDelay for precise 1.5-second pump timing
- Command system supports both Chinese ("澆水") and English ("WATER") commands
- OTA updates require proper partition table configuration for dual-boot support
- HTTP client with configurable timeouts and progress callbacks for reliable firmware downloads