# FortiGill ESP32-C5 Net Tools

A self-contained network toolkit that runs entirely on a single Waveshare
ESP32-C5-Zero board. Scan, probe, and manage your network from a simple
web UI — no app, no cloud, no companion server required.

**Hardware:** Waveshare ESP32-C5-Zero (4MB flash, single-core 240MHz
RISC-V + LP core, dual-band WiFi 6, Bluetooth 5 / BLE)

**Current version:** v2.12.1

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

- **BLE Scan** — scans for nearby Bluetooth Low Energy devices and
  guesses what each one is from its advertised name, services, and
  manufacturer ID (phone, earbuds, fitness tracker, generic BLE board,
  etc.). Any device broadcasting a Remote ID signal (see below) is
  automatically labeled as a drone rather than guessed from its name.
  Apple devices get extra detail where the signal supports it: AirPods
  (and similar audio accessories) are called out specifically via their
  Proximity Pairing broadcast, and Find My accessories (AirTags and
  other trackers) are flagged via their Offline Finding broadcast — note
  that signal is shared across AirTags, AirPods, and third-party
  trackers, so it can't be narrowed to "AirTag" specifically from this
  byte alone. Ordinary iPhones/iPads/Macs/Watches still show as a
  generic "Apple device," since no documented signal distinguishes
  between them during passive advertising.

- **Radar** — tracks a single Bluetooth device's RSSI over time by MAC
  address, so you can walk around and use the changing signal strength
  to help physically locate it.

- **Drone Remote ID Scanner** — detects nearby drones broadcasting
  Remote ID under the FAA's rule (ASTM F3411). Two scan modes:
  - **Bluetooth**, with a toggle for standard range or **BLE Long
    Range** (Coded PHY) to pick up weaker or farther-away broadcasts.
  - **WiFi** (experimental, best-effort Beacon/NAN decoding) for the
    smaller number of modules that broadcast Remote ID over WiFi
    instead of BLE — most consumer drones use BLE, so try that mode
    first.

  Each detected drone shows its MAC, RSSI, FAA registration number,
  serial number, and operator ID as they're received. Click **More
  Info** on any drone to see everything else it broadcasts, once it
  sends that message type: aircraft type/category/class, operational
  status, GPS location, heading, altitude (geodetic, pressure, and
  height above takeoff or ground level), horizontal and vertical
  speed, the accuracy figures for all of the above, and the operator's
  own location — with distance between the drone and its operator
  computed automatically once both positions are known.

- **Port Scan** — scans a target IP or hostname for common open TCP and
  UDP ports, grabs SSH/HTTP(S) banners where available, and guesses the
  likely operating system from the ICMP TTL and open-port/banner
  signals.

- **Subnet Sweep** — ARP-probes your whole subnet to find which hosts
  are alive, then optionally sweeps a specific TCP/UDP port across every
  host found.

- **Packet Capture** — captures raw 802.11 (WiFi) traffic to a
  standard-format `.pcap` file you can open in Wireshark, channel-hopping
  across 1/6/11 while it runs. No UI tab yet — control it by hitting
  these URLs directly from a browser or `curl`:
  - `GET /pcapstart` — begin a capture (optionally `?mgmtonly=1` to
    capture only management frames instead of all 802.11 traffic)
  - `GET /pcapstop` — stop the current capture
  - `GET /pcapstatus` — JSON status: whether it's active, management-
    only mode, packet count, and bytes written
  - `GET /pcapdownload` — download the captured file
  
  The status LED cycles fast blue/red while a capture is running (see
  the LED reference below).

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

## Mesh networking (in development)

A mesh networking system — multiple boards reporting into a central
portal, with remote start/stop/scheduling of scans, a device registry,
and more — is under active development and will be released separately
once it's further along. The firmware in this repo already includes the
mesh-client groundwork, but there's no public portal server to point it
at yet, so mesh-dependent features (like scheduled recurring scans) aren't
documented here until that piece ships.

---

## Status LED reference

| Pattern | Meaning |
|---|---|
| Green, double-blinks Green fast every 15s | Connected to your network on 2.4GHz |
| Green, double-blinks Blue fast every 15s | Connected to your network on 5GHz |
| Fast green blink | WiFi Scan running |
| Fast blue blink | BLE Scan or Radar running |
| Fast purple blink | Drone Remote ID scan running |
| Fast orange blink | Port Scan running |
| Fast blue/red cycle (100ms each) | Packet Capture running |
| Slow blue blink | In AP mode, no client connected yet |
| Solid blue | In AP mode, a client is connected |

---

## License / Contributing

_Add your preferred license and contribution notes here._
