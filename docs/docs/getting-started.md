---
sidebar_position: 2
---

# Getting Started

## Prerequisites

- An **ESP32-S3** board with native USB-OTG
- A USB cable (data-capable, not charge-only)
- Git installed on your host machine

:::tip
No pre-installed ESP-IDF is needed — the Makefile clones and manages a local copy.
:::

## Clone and Setup

```bash
git clone https://github.com/davidliyutong/esp32-taplink.git
cd esp32-taplink

# Install ESP-IDF v5.5.3 toolchain (one-time, ~10 min)
make setup

# Set target to ESP32-S3 (one-time)
make init
```

The toolchain is installed into `.esp-idf/` and `.espressif/` (both gitignored). Environment is auto-activated via `.envrc` (direnv) or manually with `. .esp-idf/export.sh`.

## Build and Flash

```bash
make build
make flash          # or: make flash-monitor
```

## First Connection

After flashing:

1. **USB side** — The device appears as a USB Ethernet adapter on your host. It gets an IP via DHCP from the `192.168.5.0/24` pool (gateway: `192.168.5.1`).

2. **WiFi side** — Connect to the WiFi AP:
   - **SSID**: `ESP32-TapLink`
   - **Password**: `12345678`
   - Clients get IPs from the `192.168.4.0/24` pool.

3. **Web UI** — Open [http://192.168.5.1](http://192.168.5.1) (from USB side) or [http://192.168.4.1](http://192.168.4.1) (from WiFi side).
   - **Username**: `admin`
   - **Password**: `admin`

:::warning Change Defaults
The default WiFi password and admin password should be changed immediately via the web UI.
:::

## LED Status

The LED on GPIO 21 indicates device state:

| Blink Rate | Meaning |
|---|---|
| Fast (100 ms) | Booting |
| Medium (500 ms) | Idle, USB not connected |
| Slow (1000 ms) | USB connected |

## Factory Reset

Hold the **BOOT button** (GPIO 0) for 5 seconds. The LED will go solid, NVS is erased, and the device reboots with default settings.
