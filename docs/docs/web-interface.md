---
sidebar_position: 5
---

# Web Interface

ESP32-TapLink runs an HTTP server on **port 80** with **Basic Auth** protection.

## Authentication

All pages require HTTP Basic Authentication:

- **Username**: `admin`
- **Password**: configurable (default: `admin`)

## Pages

### Dashboard (`/`)

The main page showing:

- **DHCP Lease Table** — lists all active leases with interface (USB/WiFi), IP address, MAC address, and time remaining

### Settings (`/config`)

Configuration form with:

| Setting | Input Type |
|---|---|
| WiFi SSID | Text (max 32 chars) |
| WiFi Password | Password (8–63 chars, or empty for open) |
| WiFi TX Power | Dropdown (2–20 dBm) |
| WiFi Channel | Dropdown (Auto / 1–13) |
| USB Subnet | Text (CIDR notation, e.g., `192.168.5.0/24`) |
| WiFi Subnet | Text (CIDR notation) |
| Advertise Gateway | Checkbox |
| Advertise DNS | Checkbox |
| Admin Password | Password |

Saving triggers `config_save()` and shows a "Saved. Reboot required." notice. A **Reboot** button is available to apply changes.

### Port Forwarding (`/port-forward`)

A table of 4 TCP forwarding rules. Each row:

| Column | Description |
|---|---|
| On | Enable checkbox |
| Listen | Port on the ESP32 to listen on |
| Target IP | Destination IP (must be in USB subnet) |
| Target | Destination port |

### Diagnostics (`/diag`)

Auto-refreshes every 15 seconds. Shows:

- **Network Interfaces** — all lwIP netifs with IP, MAC, and UP/LINK status
- **USB NCM** — connection status, RX/TX packet counts
- **WiFi Stations** — connected WiFi clients with MAC and RSSI
- **DHCP Leases** — active leases per interface
- **ARP Table** — current ARP entries
- **Recent Logs** — last 30 log lines from the ring buffer

## Internal Endpoints

| Method | Path | Description |
|---|---|---|
| `GET` | `/` | Dashboard |
| `GET` | `/style.css` | CSS stylesheet (cached 24h) |
| `GET` | `/config` | Settings form |
| `POST` | `/config` | Save settings |
| `GET` | `/port-forward` | Port forwarding form |
| `POST` | `/port-forward` | Save port forwarding rules |
| `GET` | `/diag` | Diagnostics page |
| `POST` | `/reboot` | Trigger reboot |
| `GET` | `/rebooting` | Reboot waiting page (auto-polls for reconnect) |
