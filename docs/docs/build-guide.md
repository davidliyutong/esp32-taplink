---
sidebar_position: 7
---

# Build Guide

## Toolchain

ESP32-TapLink uses a **local** ESP-IDF v5.5.3 installation managed by the Makefile. The toolchain lives entirely under `.esp-idf/` and `.espressif/` (both gitignored).

```bash
make setup    # Clone ESP-IDF + install toolchain (~10 min)
make init     # Set target to esp32s3 (run once after setup)
```

## Build Commands

| Command | Description |
|---|---|
| `make build` | Compile firmware |
| `make flash` | Flash to connected device |
| `make monitor` | Serial monitor (`Ctrl-]` to quit) |
| `make flash-monitor` | Flash then monitor |
| `make menuconfig` | Interactive Kconfig editor |
| `make clean` | Incremental clean |
| `make fullclean` | Remove entire build directory |
| `make format` | `clang-format` all `main/` sources |
| `make lint` | `clang-tidy` (needs prior build for `compile_commands.json`) |

## Environment

Environment is auto-activated via `.envrc` (direnv) or manually:

```bash
. .esp-idf/export.sh
```

## Key Kconfig Options

These are set in `sdkconfig.defaults`:

| Option | Value | Purpose |
|---|---|---|
| `CONFIG_TINYUSB_NET_MODE_NCM` | `y` | Enable USB NCM device mode |
| `CONFIG_LWIP_IP_FORWARD` | `y` | Enable L3 IP forwarding |
| `CONFIG_LWIP_IP4_FRAG` | `y` | Allow IPv4 fragmentation |
| `CONFIG_ESP_WIFI_SOFTAP_SUPPORT` | `y` | Enable WiFi SoftAP |
| `CONFIG_PM_ENABLE` | `y` | Enable power management |
| `CONFIG_FREERTOS_HZ` | `1000` | 1 ms tick resolution |
| `CONFIG_LWIP_MAX_SOCKETS` | `24` | Socket pool for port forwarding |

## Firmware Version

Derived from `git describe --tags` at CMake configure time:

```cmake
execute_process(
    COMMAND git describe --tags --always --dirty
    OUTPUT_VARIABLE GIT_VERSION
)
add_compile_definitions(FIRMWARE_VERSION="${GIT_VERSION}")
```

Tag your releases (e.g., `v1.0.0`) and the firmware will report the correct version.

## CI

GitHub Actions builds on push/PR to `main` using the `espressif/esp-idf:v5.5.3` container. Artifacts:

- `esp32-taplink.bin` — flashable binary
- `esp32-taplink.tar.gz` — binary + ELF (for debugging)

## Updating ESP-IDF

```bash
# Edit IDF_VERSION in Makefile if needed, then:
make update
```
