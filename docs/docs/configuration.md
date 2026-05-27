---
sidebar_position: 4
---

# Configuration Reference

All configuration is stored in NVS flash under the namespace `"taplink"` and represented by the `taplink_config_t` struct.

## Config Fields

| Field | Type | Default | Description |
|---|---|---|---|
| `wifi_ssid` | `char[33]` | `ESP32-TapLink` | WiFi AP SSID |
| `wifi_password` | `char[65]` | `12345678` | WiFi AP password (8–63 chars, or empty for open) |
| `admin_password` | `char[65]` | `admin` | Web UI admin password |
| `usb_subnet` | `uint32_t` | `192.168.5.0` | USB-side network address |
| `usb_prefix_len` | `uint8_t` | `24` | USB-side subnet prefix length (8–29) |
| `wifi_subnet` | `uint32_t` | `192.168.4.0` | WiFi-side network address |
| `wifi_prefix_len` | `uint8_t` | `24` | WiFi-side subnet prefix length (8–29) |
| `dhcp_gw_enabled` | `uint8_t` | `0` | Advertise gateway in DHCP offers |
| `dhcp_dns_enabled` | `uint8_t` | `0` | Advertise DNS in DHCP offers |
| `wifi_tx_power` | `int8_t` | `44` | WiFi TX power (in 0.25 dBm units) |
| `wifi_channel` | `uint8_t` | `0` | WiFi channel (0 = auto, 1–13) |
| `port_forwards[4]` | `port_forward_rule_t` | *(empty)* | TCP port forwarding rules |

## WiFi TX Power Levels

The `wifi_tx_power` field uses ESP-IDF's quarter-dBm scale:

| Value | Actual Power |
|---|---|
| `80` | 20 dBm (max) |
| `68` | 17 dBm |
| `60` | 15 dBm |
| `44` | 11 dBm (default) |
| `34` | 8.5 dBm |
| `20` | 5 dBm |
| `8` | 2 dBm (min) |

## DHCP Server

Each subnet runs its own DHCP server:

- **Gateway IP**: `.1` of the subnet (e.g., `192.168.5.1`)
- **Pool range**: `.2` to `.11`
- **Lease time**: 60 minutes
- **Max tracked leases**: 16

### DHCP Options

When USB and WiFi subnets don't overlap (which they shouldn't), the DHCP server injects static routes so clients on each side can reach the other subnet:

- **Option 121** — Classless Static Routes (RFC 3442)
- **Option 249** — Microsoft Classless Static Routes
- **Option 33** — Static Routes (classful, only when prefix matches classful boundary)

### Gateway and DNS Advertisement

| Setting | Effect |
|---|---|
| `dhcp_gw_enabled = 1` | DHCP offers include gateway option pointing to the ESP32 |
| `dhcp_dns_enabled = 1` | DHCP offers include DNS option pointing to the ESP32 |

:::note
The ESP32 does **not** run a DNS resolver or forwarder. Enabling DNS advertisement is only useful if you have upstream DNS configured elsewhere in your network.
:::

## Port Forward Rule

```c
typedef struct {
    uint8_t  enabled;
    uint16_t listen_port;
    uint32_t target_ip;
    uint16_t target_port;
} port_forward_rule_t;
```

| Constraint | Detail |
|---|---|
| Max rules | 4 |
| Listen port | 1–65535, cannot be `80` (reserved for web UI) |
| Target IP | Must be in the USB subnet, cannot be network/gateway/broadcast |
| Duplicates | No two enabled rules may share the same listen port |

## NVS Keys

The config is stored under individual NVS keys:

| NVS Key | Config Field |
|---|---|
| `wifi_ssid` | `wifi_ssid` |
| `wifi_pass` | `wifi_password` |
| `admin_pass` | `admin_password` |
| `usb_ip` | `usb_subnet` |
| `usb_pfx` | `usb_prefix_len` |
| `wifi_ip` | `wifi_subnet` |
| `wifi_pfx` | `wifi_prefix_len` |
| `dhcp_gw` | `dhcp_gw_enabled` |
| `dhcp_dns` | `dhcp_dns_enabled` |
| `wifi_txp` | `wifi_tx_power` |
| `wifi_ch` | `wifi_channel` |
| `pf{N}_enabled` | `port_forwards[N].enabled` |
| `pf{N}_lport` | `port_forwards[N].listen_port` |
| `pf{N}_tip` | `port_forwards[N].target_ip` |
| `pf{N}_tport` | `port_forwards[N].target_port` |

## Sanitization

On load, `config_sanitize()` validates all fields and falls back to defaults for invalid values:

- Empty SSID → restored to default
- Password < 8 chars (non-empty) → restored to default
- Invalid subnet (prefix < 8 or > 29) → restored to default
- Overlapping USB/WiFi subnets → both restored to default
- TX power out of range (< 8 or > 80) → restored to default
- WiFi channel > 13 → restored to default
- Invalid port-forward rules → dropped silently
