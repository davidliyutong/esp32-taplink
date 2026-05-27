# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ESP32-NetLink is firmware for ESP32-S3 that creates a USB-NCM-to-WiFi-AP network bridge. A host computer plugs in via USB and sees an NCM (CDC) Ethernet adapter; the ESP32 runs a SoftAP and bridges L2 frames between USB and WiFi, with a built-in DHCP server and web management interface.

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

1. **NVS + config** — `nvs_config.c` loads `netlink_config_t` from NVS flash (or defaults).
2. **Netif creation** — `usb_ncm_netif_create()` and `wifi_ap_create()` each create an `esp_netif_t` with no IP stack (flags=0, no IP info) so they act as raw L2 bridge ports.
3. **Bridge** — `bridge.c` creates an `ESP_NETIF_NETSTACK_DEFAULT_BR` netif that owns the IP address and DHCP server, then attaches the USB and WiFi ports via `esp_netif_br_glue`.
4. **Start** — USB NCM TinyUSB driver, WiFi AP, and HTTP server are started.

### Module responsibilities

| Module | Role |
|---|---|
| `nvs_config` | Persist/load `netlink_config_t` to NVS under namespace `"netlink"` |
| `usb_ncm` | TinyUSB NCM device driver, implements `esp_netif` driver interface (transmit/receive/post_attach), fires ETH_EVENT lifecycle events |
| `wifi_ap` | SoftAP setup, auto-restart on unexpected stop (up to 3 retries), station connect/disconnect tracking |
| `bridge` | L2 bridge netif with DHCP server (pool .2–.11, 60-min lease), configurable gateway/DNS advertisement |
| `web_server` | HTTP server on port 80 with Basic Auth; dashboard (`/`) shows DHCP clients, settings page (`/config`) for WiFi/DHCP/admin config; POST saves to NVS and reboots after 3s |
| `app_main` | Orchestrates init order; LED blink task (GPIO 21) indicates boot/USB state; button (GPIO 0) long-press triggers factory reset via NVS erase |

### Key design details

- USB NCM uses Ethernet-event lifecycle (`ETHERNET_EVENT_START/CONNECTED/DISCONNECTED/STOP`) to integrate with `esp_netif`, even though it's not real Ethernet — this is how bridge glue expects non-WiFi ports.
- The USB NCM MAC is derived from the base efuse MAC with byte[5]+4 and the locally-administered bit set.
- WiFi and USB netifs are created with `flags=0` and `ip_info=NULL` so they have no IP of their own — only the bridge netif has an IP and runs DHCP.
- Config changes via the web UI trigger `config_save()` + delayed `esp_restart()` — there is no hot-reconfigure path.

## CI

GitHub Actions builds on push/PR to `main` using the `espressif/esp-idf:v5.5.3` container. It produces `esp32-netlink.bin` and an ELF tarball as artifacts.

## Target Hardware

- **MCU**: ESP32-S3 (`IDF_TARGET=esp32s3`)
- **LED**: GPIO 21
- **Button**: GPIO 0 (boot button, factory reset on 5s hold)
- **USB**: Native USB-OTG for NCM device
