# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ESP32-TapLink is firmware for ESP32-S3 that creates a USB-NCM-to-WiFi-AP network router. A host computer plugs in via USB and sees an NCM (CDC) Ethernet adapter; the ESP32 runs a SoftAP and routes IP traffic between USB and WiFi via lwIP IP forwarding, with dual-subnet DHCP servers and a web management interface.

## Build Commands

The project uses a local ESP-IDF v5.5.3 install managed by the Makefile. The toolchain lives entirely under `.esp-idf/` and `.espressif/` (both gitignored).

```bash
make setup          # first-time: clone ESP-IDF + install toolchain
make init           # set target to esp32s3 (run once after setup)
make build          # compile firmware
make flash          # flash to connected device
make monitor        # serial monitor (Ctrl-] to quit)
make flash-monitor  # flash then monitor
make menuconfig     # interactive Kconfig editor
make clean          # incremental clean
make fullclean      # remove entire build directory
make format         # clang-format all main/ sources
make lint           # clang-tidy (requires a prior build for compile_commands.json)
```

Environment is auto-activated via `.envrc` (direnv) or manually with `. .esp-idf/export.sh`.

Firmware version is derived from `git describe --tags` at CMake configure time and compiled in as `FIRMWARE_VERSION`.

## Architecture

All application code is in `main/`. The startup sequence in `app_main.c` is:

1. **NVS + config** — `nvs_config.c` loads `taplink_config_t` from NVS flash (or defaults).
2. **Netif creation** — `usb_ncm_netif_create()` and `wifi_ap_create()` each create an `esp_netif_t` on separate subnets.
3. **Router** — `router.c` configures dual DHCP servers (one per subnet), enables lwIP IP forwarding, and injects static routes via DHCP options.
4. **Start** — USB NCM TinyUSB driver, WiFi AP, HTTP server, and port-forwarding are started.

### Module responsibilities

| Module | Role |
|---|---|
| `nvs_config` | Persist/load `taplink_config_t` to NVS under namespace `"taplink"` |
| `usb_ncm` | TinyUSB NCM device driver, implements `esp_netif` driver interface (transmit/receive/post_attach), fires ETH_EVENT lifecycle events |
| `wifi_ap` | SoftAP setup, auto-restart on unexpected stop (up to 3 retries), station connect/disconnect tracking |
| `router` | Dual-subnet DHCP servers (pool .2–.11, 60-min lease), lwIP IP forwarding, DHCP static route injection, lease tracking |
| `port_forward` | TCP port-forwarding rules (up to 4, lwIP raw API) |
| `web_server` | HTTP server on port 80 with Basic Auth; dashboard (`/`) shows DHCP clients, settings page (`/config`) for WiFi/DHCP/admin config; POST saves to NVS and reboots after 3s |
| `app_main` | Orchestrates init order; LED blink task (GPIO 21) indicates boot/USB state; button (GPIO 0) long-press triggers factory reset via NVS erase |

### Key design details

- USB NCM uses Ethernet-event lifecycle (`ETHERNET_EVENT_START/CONNECTED/DISCONNECTED/STOP`) to integrate with `esp_netif`, even though it's not real Ethernet — this is how bridge glue expects non-WiFi ports.
- The USB NCM MAC is derived from the base efuse MAC with byte[5]+4 and the locally-administered bit set.
- WiFi and USB netifs each have their own IP and DHCP server on separate subnets; traffic between them is routed at L3 via `CONFIG_LWIP_IP_FORWARD`.
- Config changes via the web UI trigger `config_save()` + delayed `esp_restart()` — there is no hot-reconfigure path.

## CI

GitHub Actions builds on push/PR to `main` using the `espressif/esp-idf:v5.5.3` container. It produces `esp32-taplink.bin` and an ELF tarball as artifacts.

## Target Hardware

- **MCU**: ESP32-S3 (`IDF_TARGET=esp32s3`)
- **LED**: GPIO 21
- **Button**: GPIO 0 (boot button, factory reset on 5s hold)
- **USB**: Native USB-OTG for NCM device
