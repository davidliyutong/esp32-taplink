# Repository Guidelines

## Project Structure & Module Organization

This is an ESP-IDF firmware project for `esp32s3`. Top-level `CMakeLists.txt` declares the `esp32-netlink` project and injects `FIRMWARE_VERSION` from `git describe`. `Makefile` wraps ESP-IDF setup and daily development commands. Runtime code lives in `main/`:

- `app_main.c`: boot sequence, NVS init, LED/button task, subsystem startup.
- `usb_ncm.*`: USB NCM network interface and connection state.
- `wifi_ap.*`: SoftAP setup and station count.
- `bridge.*`: ESP netif bridge wiring between USB and Wi-Fi.
- `web_server.*`: configuration/dashboard HTTP server.
- `nvs_config.*`: persisted configuration defaults, load, and save.

Generated or local-only directories such as `.esp-idf/`, `.espressif/`, `managed_components/`, `build/`, and `sdkconfig` are ignored and should not be committed. Keep reproducible defaults in `sdkconfig.defaults`.

## Build, Test, and Development Commands

- `make setup`: clone ESP-IDF `v5.5.3` locally and install the `esp32s3` toolchain.
- `make init`: run `idf.py set-target esp32s3`.
- `make build`: compile firmware; this is the primary validation command.
- `make flash` / `make monitor` / `make flash-monitor`: program and inspect a connected board.
- `make menuconfig`: edit Kconfig settings interactively.
- `make format`: run `clang-format -i` over `main/**/*.{c,h,cpp,hpp}`.
- `make lint`: run `clang-tidy -p build`; run `make build` first.

CI mirrors the build with the `espressif/esp-idf:v5.5.3` container.

## Coding Style & Naming Conventions

Use C with 4-space indentation and K&R braces, matching existing files. Keep module-private state and helpers `static`, use `s_` for file-static variables, and define a per-file `TAG` for ESP logging. Public APIs belong in the matching header and use module prefixes such as `wifi_ap_start`, `usb_ncm_start`, and `config_load`. Prefer ESP-IDF types and error handling (`esp_err_t`, `ESP_ERROR_CHECK`) over ad hoc status values.

## Testing Guidelines

There is no standalone unit test suite yet. Treat `make build` and, for hardware-sensitive changes, `make flash-monitor` smoke testing as required checks. If adding tests later, follow ESP-IDF component test conventions and keep test fixtures out of firmware runtime paths.

## Commit & Pull Request Guidelines

History uses Conventional Commit style, usually `feat:` or `feat(scope): ...`. Keep messages imperative and scoped when useful, for example `feat(wifi_ap): add channel validation`. Pull requests should describe behavior changes, list validation commands run, link related issues, and include screenshots only for web dashboard UI changes.

## Security & Configuration Tips

Do not commit secrets, local `sdkconfig`, or generated artifacts. Route configurable credentials and network settings through `nvs_config.*` and defaults rather than hardcoded values.
