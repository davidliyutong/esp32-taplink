---
sidebar_position: 3
---

# Architecture

All application code lives in `main/`. The firmware follows a straightforward init-and-run model with no RTOS task hierarchy beyond the main task and a few service tasks.

## Startup Sequence

```
app_main()
  │
  ├─ 1. LED/button task      (led_button_task)
  ├─ 2. NVS init + config    (config_load)
  ├─ 3. Netif + event loop   (esp_netif_init, esp_event_loop_create_default)
  ├─ 4. WiFi init             (esp_wifi_init)
  ├─ 5. Create netifs         (usb_ncm_netif_create, wifi_ap_create)
  ├─ 6. Router start          (router_start)
  ├─ 7. Web server start      (web_server_start)
  ├─ 8. WiFi AP start         (wifi_ap_start)
  ├─ 9. USB NCM start         (usb_ncm_start)
  └─ 10. Port forwarding      (port_forward_start)
```

## Module Overview

```
app_main
  ├─ nvs_config     — load/save taplink_config_t from NVS
  ├─ usb_ncm        — TinyUSB NCM device, esp_netif driver interface
  ├─ wifi_ap        — SoftAP with auto-restart and station tracking
  ├─ router         — dual-subnet DHCP servers, IP forwarding, route injection
  ├─ port_forward   — TCP port-forwarding rules (lwIP raw API)
  └─ web_server     — HTTP/80, dashboard + config pages, Basic Auth
```

### `nvs_config`

Manages the `taplink_config_t` struct — loading from and saving to NVS flash under namespace `"taplink"`. Applies sanitization on load: validates subnets, clamps TX power, prunes invalid port-forward rules.

### `usb_ncm`

Implements the TinyUSB NCM (Network Control Model) device driver. Exposes an `esp_netif` driver interface (transmit/receive/post_attach) and fires `ETHERNET_EVENT_*` lifecycle events for integration with the network stack.

The USB NCM MAC address is derived from the base eFuse MAC with `byte[5]+4` and the locally-administered bit set.

### `wifi_ap`

Manages the WiFi SoftAP. Includes auto-restart logic — if the AP stops unexpectedly, it retries up to 3 times. Tracks station connect/disconnect events.

### `router`

The core networking module. Creates two independent network interfaces (USB and WiFi), each on its own subnet with its own DHCP server. Enables lwIP `CONFIG_LWIP_IP_FORWARD` for L3 routing between them.

Key responsibilities:
- **DHCP server configuration** — pool `.2` to `.11`, 60-minute leases, configurable gateway/DNS advertisement
- **Static route injection** — injects routes via DHCP options (classless RFC 3442, Microsoft option 249, and classful option 33) so clients on each side can reach the other subnet
- **Lease tracking** — maintains an in-memory table of active DHCP leases for the dashboard
- **lwIP hooks** — `taplink_lwip_hooks.h` provides `LWIP_HOOK_DHCPS_POST_APPEND_OPTS` and `LWIP_HOOK_DHCPS_POST_STATE` to inject routes and observe lease state changes

### `port_forward`

Implements up to 4 TCP port-forwarding rules using the lwIP raw socket API. Each rule maps a listen port on the ESP32 to a target IP:port on the USB subnet.

### `web_server`

HTTP server on port 80 with Basic Auth. Serves four pages (Dashboard, Settings, Port Forwarding, Diagnostics) plus handles POST endpoints for config changes and reboot. Includes a ring buffer for recent log capture.

## Data Flow

```
USB Host ──[NCM frames]──▶ usb_ncm ──▶ esp_netif (192.168.5.0/24)
                                              │
                                        lwIP IP_FORWARD
                                              │
WiFi Clients ◀──[WiFi]── wifi_ap ◀── esp_netif (192.168.4.0/24)
```

There is no hot-reconfigure path. Config changes via the web UI call `config_save()` and then show a reboot-required page; press the Reboot button to restart the device and apply the changes.

## Firmware Version

Derived from `git describe --tags` at CMake configure time and compiled in as `FIRMWARE_VERSION`.
