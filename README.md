# ESP32-S3 ↔ MCP2515 ↔ CAN ↔ MCP2515 ↔ Raspberry Pi 4B (Bidirectional Ping-Pong)

Fully wired, production-ready CAN integrity test using Robu-style MCP2515 + TJA1050 (8 MHz crystal, 125 kbps). Both nodes send and echo known payloads and print `MATCHED` only when the payload is verified byte-for-byte.

## Safety (3.3 V vs 5 V)

Robu MCP2515 boards power both MCP2515 and TJA1050 from 5 V and **do not level-shift SPI**. Raspberry Pi and ESP32-S3 GPIO are 3.3 V only. Protect the MCUs by:

- Modifying the board so MCP2515 runs at 3.3 V and TJA1050 at 5 V, or
- Using a 3.3 V CAN transceiver board / HAT, or
- Adding proper bidirectional level shifters on all SPI lines.

Do not skip this in hardware.

## Wiring

### CAN bus

- CANH ↔ CANH, CANL ↔ CANL between the two MCP2515 boards (twisted pair recommended).
- Enable the 120 Ω termination jumper on both boards (two-node bus).
- Common ground between ESP supply and Pi supply.

### ESP32-S3 DevKitC-1 ↔ MCP2515 (8 MHz)

| MCP2515 pin | ESP32-S3 GPIO |
| ----------- | ------------- |
| CS          | 41            |
| INT         | 40            |
| SCK         | 48            |
| SO (MISO)   | 21            |
| SI (MOSI)   | 47            |
| VCC         | 5 V (see safety note) |
| GND         | GND           |

### Raspberry Pi 4B ↔ MCP2515 (8 MHz)

| MCP2515 pin | Pi header pin | Pi GPIO  |
| ----------- | ------------- | -------- |
| VCC         | 2 or 4        | 5 V      |
| GND         | 6 (e.g.)      | GND      |
| SCK         | 23            | GPIO 11  |
| SI (MOSI)   | 19            | GPIO 10  |
| SO (MISO)   | 21            | GPIO 9   |
| CS          | 24            | GPIO 8 (spi0.0) |
| INT         | 22            | GPIO 25  |

## Firmware (ESP32-S3)

- Code: `src/main.cpp`
- Config: `platformio.ini` (Arduino, autowp MCP2515 library)
- CAN: 125 kbps, MCP2515 clock 8 MHz.
- Behavior:
  - ESP initiates PING (ID `0x123`) every second; expects PONG (ID `0x124`) with identical payload.
  - Responds to Pi-initiated PING (ID `0x223`) with PONG (ID `0x224`).
  - Prints `MATCHED` only when payload bytes match exactly.
  - Tracks send errors; auto-reinitializes MCP2515 after repeated failures.

### Build & flash

```bash
pio run -t upload -e esp32-s3-devkitc-1
pio device monitor -e esp32-s3-devkitc-1
```

## Raspberry Pi setup

1. Edit `/boot/config.txt` and append (8 MHz crystal):
   ```
   dtparam=spi=on
   dtoverlay=mcp2515-can0,oscillator=8000000,interrupt=25
   dtoverlay=spi-bcm2835
   ```
2. Reboot.
3. Install tools:
   ```bash
   sudo apt update
   sudo apt install -y can-utils python3-can
   ```
4. Bring up CAN:
   ```bash
   sudo ip link set can0 up type can bitrate 125000
   ip -details -statistics link show can0
   ```
5. Run the bidirectional responder/initiator:
   ```bash
   python3 pi/can_ping_pong.py
   ```

### Helper script (optional)

You can automate the Pi overlay setup and CAN bring-up with:

```bash
sudo ./scripts/setup_can_rpi.sh
```

Defaults: `can0`, 125000 bit/s, oscillator 8000000, interrupt GPIO25. It backs up `/boot/config.txt` (or `/boot/firmware/config.txt`), ensures the overlays are present, and if `can0` already exists it configures and brings it up immediately. Reboot after first run to load overlays.

## Expected runtime output

- ESP32 serial:
  - `TX PING (ESP->Pi)...` every second.
  - `MATCHED (ESP-initiated)` when Pi echoes correctly.
  - `MATCHED (Pi->ESP PING)` when ESP validates Pi PING before echoing.
- Raspberry Pi console:
  - `TX PING (Pi->ESP), counter=...` every second.
  - `MATCHED (Pi-initiated)` when ESP echoes correctly.
  - `MATCHED (ESP->Pi PING)` when ESP-initiated payload verifies.

## Troubleshooting checklist

- Bitrate/clock: Both ends must use 125 kbps and MCP2515 `oscillator=8000000`.
- Driver load: `dmesg | grep -i mcp2515` should show successful init; `can0` must be `state UP`.
- Wiring: Verify SCK/MOSI/MISO/CS/INT and CANH/CANL (not swapped).
- Termination: Exactly two 120 Ω terminators (both jumpers on for a two-node bus).
- Level shifting: Ensure Pi/ESP never see 5 V on SPI. Use proper hardware mitigation.
- Noise/power: Stable 5 V supply for TJA1050; clean ground reference between nodes.
