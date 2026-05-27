---
sidebar_position: 8
---

# Limitations

## Not a Layer-2 Bridge

:::danger Important
ESP32-TapLink does **not** perform transparent Layer-2 frame bridging.
:::

USB and WiFi interfaces live on **separate IP subnets**, and traffic between them is routed at **Layer 3** via `CONFIG_LWIP_IP_FORWARD`. This architecture has specific consequences:

### What doesn't work across sides

| Protocol | Why |
|---|---|
| **mDNS / Bonjour** | Multicast `.local` queries don't cross subnet boundaries |
| **SSDP / UPnP** | Multicast discovery is subnet-local |
| **DLNA** | Relies on SSDP discovery |
| **Wake-on-LAN** | Requires L2 broadcast on the target segment |
| **ARP** | Not forwarded between subnets (each side has its own ARP table) |
| **IoT pairing** | Many IoT protocols require the phone and device to be on the same L2 segment |

### Why not true L2 bridging?

The ESP32 WiFi driver does **not support 802.11 4-address (WDS) frames**. In a SoftAP, only the AP's own MAC appears in the wireless frame headers. For true L2 bridging, the AP would need to transmit frames with arbitrary source MACs (belonging to USB-side hosts) — this requires WDS/4-address mode, which the hardware doesn't expose.

An earlier version of this firmware used ESP-IDF's `esp_netif_br_glue` L2 bridge, but it had the same fundamental limitation: WiFi clients could see the bridge, but USB-side hosts' MACs couldn't traverse the WiFi link.

### What works fine

- **IP connectivity** between USB and WiFi sides (ping, TCP, UDP)
- **SSH / HTTP / any TCP service** across the boundary
- **Port forwarding** from WiFi to USB-side services
- **Static route injection** ensures both sides know how to reach each other

For most use cases — giving a USB-only host network access through WiFi, or letting a laptop manage WiFi devices — L3 routing is sufficient.

## Other Limitations

### No hot-reconfigure

All configuration changes require a **reboot** to take effect. There is no runtime reload path.

### DHCP pool size

The DHCP pool is limited to **10 addresses** per subnet (`.2` through `.11`). The lease tracking table supports **16 entries** total across both interfaces.

### Port forwarding

- **TCP only** — no UDP forwarding
- Maximum **4 rules**
- Target must be in the **USB subnet** only

### WiFi

- **SoftAP only** — the ESP32 does not connect to an upstream WiFi network (no STA mode)
- No WPA3-SAE support (WPA2-PSK only)
- Maximum **10 simultaneous stations** (ESP-IDF default for SoftAP)

### Web UI

- HTTP only (no HTTPS/TLS) — credentials are sent in cleartext
- Basic Auth — no session management
- No REST/JSON API — HTML forms only

### No DNS/NAT

The ESP32 does not run a DNS forwarder or NAT. Enabling "Advertise DNS" in DHCP points clients to the ESP32's IP, but nothing answers DNS queries there.
