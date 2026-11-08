# FortiGill ESP32-C5 Net Tools

A self-contained network toolkit that runs entirely on a single Waveshare
ESP32-C5-Zero board. Scan, probe, and manage your network from a simple
web UI — no app, no cloud, no companion server required.

**Hardware:** Waveshare ESP32-C5-Zero (4MB flash, single-core 240MHz
RISC-V + LP core, dual-band WiFi 6, Bluetooth 5 / BLE)

**Current version:** v1.13.0

---

## Getting Started

1. **Flash the firmware** — see [FLASHING.md](FLASHING.md) for step-by-step
   instructions on Windows, macOS, and Linux.
2. **Power on the board.** On first boot (no saved WiFi credentials yet)
   it broadcasts its own WiFi access point.
3. **Connect** your phone or laptop to that access point — SSID
   `FortiGill`, password `fortigillwins` — then open a browser to the
   board's address (typically `192.168.4.1`).
4. Open the **Wi-Fi Settings** tab, enter your home network's name and
   password, and save. The board reboots and joins your network.
5. From then on, reach the web UI at `http://esp32-c5.local` or the
   board's new IP address on your network.
6. **System Status** is the landing page — a live dashboard so you can
   confirm the board is healthy before doing anything else.

---

## Tools

- **System Status** — the default landing page. A live dashboard showing
  a loop-rate CPU-load proxy, free memory, flash/storage usage, onboard
  chip temperature, current network info, and which background tools
  are actively running.

- **WiFi Scan** — scans for nearby WiFi networks and lists them with
  signal strength and channel/band info.

- **Ping** — sends ICMP pings to an IP address or hostname and reports
  round-trip times, with an optional Continuous mode (ping until you
  press Stop) that shows a live min/avg/max stats line and a scrolling
  chart of reply times as they come in.

- **Traceroute** — traces the hop-by-hop network path to a given IP or
  hostname.

- **DNS / WHOIS** — performs DNS lookups (including reverse DNS) and
  WHOIS queries against a domain or IP address.

- **BLE Scan** — scans for and tracks nearby Bluetooth Low Energy
  devices.

- **Port Scan** — scans a target IP or hostname for common open TCP and
  UDP ports, grabs SSH/HTTP(S) banners where available, and guesses the
  likely operating system from the ICMP TTL and open-port/banner
  signals.

- **Subnet Sweep** — ARP-probes your whole subnet to find which hosts
  are alive, then optionally sweeps a specific TCP/UDP port across every
  host found.

- **TFTP Server** — upload files to the board's flash storage from your
  browser and delete them again; once you press **Start Daemon**, those
  files become downloadable to any TFTP client on the network over UDP
  port 69. The daemon does nothing until started, and total storage is
  capped at about 1MB.

- **Wi-Fi Settings** — configure which WiFi network the board joins, or
  let it fall back to broadcasting its own access point if it can't
  connect.

- **Network Settings** — switch the board's IPv4 configuration between
  DHCP and a static IP.

- **LED** — toggle the onboard status LED on or off. (See the LED
  reference below for what each blink pattern means when it's on.)

---

## Status LED reference

| Pattern | Meaning |
|---|---|
| Green, double-blinks Green fast every 15s | Connected to your network on 2.4GHz |
| Green, double-blinks Blue fast every 15s | Connected to your network on 5GHz |
| Fast green blink | WiFi Scan running |
| Fast blue blink | BLE Scan running |
| Fast orange blink | Port Scan running |
| Slow blue blink | In AP mode, no client connected yet |
| Solid blue | In AP mode, a client is connected |

---

## License / Contributing

_Add your preferred license and contribution notes here._
