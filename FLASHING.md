# Flashing FortiGill ESP32-C5 Net Tools

Target hardware: **Waveshare ESP32-C5-Zero**

These instructions assume you're comfortable using a terminal/command
line — they're high-level rather than click-by-click.

## What you'll need

- A Waveshare ESP32-C5-Zero board and a USB-C cable
- [Arduino CLI](https://arduino.github.io/arduino-cli/latest/installation/)
  with the ESP32 board package installed (3.3.5 or newer recommended)
- This repo's `esp32_c5_tools.ino` and `partitions.csv`, in the same folder

## 1. Install Arduino CLI and the ESP32 core

### Windows
1. Install Arduino CLI (download from the link above, or `winget install ArduinoCLI`) and make sure it's on your PATH.
2. ```
   arduino-cli core update-index
   arduino-cli core install esp32:esp32
   ```
3. Plug in the board over USB-C. Check **Device Manager** for the new COM port (e.g. `COM5`).

### macOS
1. ```
   brew install arduino-cli
   ```
2. ```
   arduino-cli core update-index
   arduino-cli core install esp32:esp32
   ```
3. Plug in the board. Find the port:
   ```
   ls /dev/cu.*
   ```
   (usually something like `/dev/cu.usbmodem1101`)

### Linux
1. ```
   curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | sh
   ```
   (or your distro's package, if it has one)
2. ```
   arduino-cli core update-index
   arduino-cli core install esp32:esp32
   ```
3. Plug in the board. Find the port:
   ```
   ls /dev/ttyUSB* /dev/ttyACM*
   ```
   You may need permission to access it:
   ```
   sudo usermod -aG dialout $USER
   ```
   (then log out and back in)

## 2. Get the sketch

Clone or download this repo so that `esp32_c5_tools.ino` and
`partitions.csv` end up in the same folder, e.g. `esp32_c5_tools/`.

**`partitions.csv` matters** — it gives the board a ~2.94MB app
partition and a 1MB SPIFFS partition (used by the TFTP Server tool).
Without it sitting next to the `.ino` file, the build will use a
different, incompatible layout.

## 3. Compile and upload

From inside that folder, the two commands are identical across all
three operating systems — only the `--port` value changes:

```
arduino-cli compile --fqbn "esp32:esp32:esp32c5:CDCOnBoot=cdc,PartitionScheme=custom" .
arduino-cli upload --fqbn "esp32:esp32:esp32c5:CDCOnBoot=cdc,PartitionScheme=custom" -p <YOUR_PORT> .
```

Replace `<YOUR_PORT>` with what you found in step 1, for example:
- Windows: `COM5`
- macOS: `/dev/cu.usbmodem1101`
- Linux: `/dev/ttyUSB0`

## 4. First boot

See **Getting Started** in the main [README](README.md) for connecting
to the board's access point and joining it to your WiFi network.

---

## Backing up the whole flash chip

This creates one `.bin` file containing everything on the chip —
bootloader, partition table, firmware, and storage — so someone else
can write it straight to their own Waveshare ESP32-C5-Zero and get an
identical, ready-to-go board. No Arduino IDE or compiling required on
their end.

### Important: start from a "clean" board

If you've already set your own home WiFi password on this board, that
password is sitting in flash right now and would be captured by the
backup. Before making a backup meant for sharing publicly:

1. Fully erase the chip:
   ```
   esptool --chip esp32c5 --port <YOUR_PORT> erase-flash
   ```
2. Compile and upload the sketch fresh (step 3 above) — this gives you
   a clean image with no saved WiFi credentials or stored TFTP files.
3. **Now** make the backup (next section) — before reconnecting it to
   your own WiFi.
4. Afterward, go back through Wi-Fi Settings on your own board to
   reconnect it for your own daily use.

### Create the backup

```
esptool --chip esp32c5 --port <YOUR_PORT> read-flash 0x0 0x400000 fortigill_esp32c5_backup.bin
```

This reads the full 4MB flash chip into `fortigill_esp32c5_backup.bin`.
That's the file to share (e.g. as a GitHub Release attachment).

### Restore it onto another board

Anyone with their own Waveshare ESP32-C5-Zero only needs `esptool` and
your `.bin` file — no Arduino IDE needed:

```
esptool --chip esp32c5 --port <THEIR_PORT> erase-flash
esptool --chip esp32c5 --port <THEIR_PORT> write-flash 0x0 fortigill_esp32c5_backup.bin
```

Their board will boot up identical to yours, broadcasting its own
setup access point, ready for them to join their own WiFi.

### If `esptool` isn't on your PATH

Install it directly:
```
pip install esptool
```
or use the copy already bundled with the Arduino ESP32 core:
- **macOS:** `~/Library/Arduino15/packages/esp32/tools/esptool_py/<version>/esptool`
  (find the exact path with `find ~/Library/Arduino15/packages/esp32/tools/esptool_py -name "esptool*"`)
- **Windows:** `%LOCALAPPDATA%\Arduino15\packages\esp32\tools\esptool_py\<version>\esptool.exe`
- **Linux:** `~/.arduino15/packages/esp32/tools/esptool_py/<version>/esptool`
