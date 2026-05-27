---
sidebar_position: 1
slug: /
---

# Introduction

ESP32-TapLink is firmware for ESP32-S3 that turns the chip into a **USB NCM-to-WiFi AP network router**.

A host computer plugs in via USB and sees a standard CDC-NCM Ethernet adapter. The ESP32 simultaneously runs a WiFi SoftAP. Both sides get their own IP subnet with independent DHCP servers, and lwIP routes IP traffic between them.

```
┌──────────┐     USB NCM     ┌───────────┐     WiFi AP     ┌────────────┐
│   Host   │────────────────▶│  ESP32-S3 │◀────────────────│  Clients   │
│  (DHCP)  │  192.168.5.0/24 │  (router) │ 192.168.4.0/24  │   (DHCP)   │
└──────────┘                 └───────────┘                 └────────────┘
```

## Key Features

| Feature | Description |
|---|---|
| **USB NCM** | Driverless on macOS/Linux, standard CDC-NCM on Windows |
| **WiFi SoftAP** | Configurable SSID, password, channel, TX power |
| **Dual-subnet DHCP** | Independent pools for USB and WiFi |
| **IP Forwarding** | lwIP L3 routing with DHCP static route injection |
| **Port Forwarding** | Up to 4 TCP rules |
| **Web UI** | Dashboard, settings, diagnostics — protected by Basic Auth |
| **NVS Persistence** | All config survives reboot |
| **Factory Reset** | Hold GPIO 0 for 5 seconds |

## Hardware Requirements

| Item | Detail |
|---|---|
| MCU | ESP32-S3 (native USB-OTG required) |
| LED | GPIO 21 |
| Button | GPIO 0 (boot button) |

:::caution Not a Layer-2 Bridge
ESP32-TapLink does **not** perform transparent L2 frame bridging. See [Limitations](./limitations) for details.
:::
