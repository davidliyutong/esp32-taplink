---
sidebar_position: 6
---

# Port Forwarding

ESP32-TapLink supports up to **4 TCP port-forwarding rules**. Each rule maps a listen port on the ESP32 to a target host on the USB subnet.

## Use Case

A typical scenario: you have a device on the USB side (e.g., a Raspberry Pi at `192.168.5.2`) running an SSH server. WiFi clients can't reach it directly because the subnets are separate. With a port-forward rule, WiFi clients can SSH to the ESP32's WiFi IP on a forwarded port.

```
WiFi Client ──▶ 192.168.4.1:2222 ──[ESP32 forward]──▶ 192.168.5.2:22
```

## Configuration

Rules are configured via the **Port Forwarding** page in the web UI, or stored directly in NVS.

| Field | Constraints |
|---|---|
| **Listen Port** | 1–65535, cannot be `80` |
| **Target IP** | Must be in the USB subnet, cannot be network/gateway/broadcast address |
| **Target Port** | 1–65535 |

## Constraints

- Maximum **4 rules** total
- No two enabled rules may share the same **listen port**
- Port `80` is reserved for the web UI
- Only **TCP** is supported (no UDP forwarding)
- Target IP must be within the USB subnet range

## How It Works

Port forwarding uses the **lwIP raw socket API** (`SOCK_STREAM`). For each enabled rule, a listener socket is created on the ESP32. When a connection arrives, the ESP32 opens a new connection to the target and relays data bidirectionally.

:::note
Changes to port-forward rules require a **reboot** to take effect — there is no hot-reconfigure path.
:::
