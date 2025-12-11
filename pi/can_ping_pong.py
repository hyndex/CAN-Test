#!/usr/bin/env python3
"""
Bidirectional CAN ping-pong test for Raspberry Pi + MCP2515 (8 MHz, 125 kbps).

Behavior:
1) Responds to ESP-initiated PING (0x123) with PONG (0x124) and prints MATCHED.
2) Sends its own PING (0x223) every second, expects PONG (0x224) from ESP,
   and prints MATCHED when payload echoes exactly.

Requirements:
- SocketCAN interface up (e.g., `can0` via mcp2515 overlay, 125000 bit/s).
- python-can installed (`sudo apt install -y python3-can`).
"""

import signal
import sys
import time
from typing import Optional

import can

ESP_PING_ID = 0x123  # ESP -> Pi
ESP_PONG_ID = 0x124  # Pi -> ESP
PI_PING_ID = 0x223   # Pi -> ESP
PI_PONG_ID = 0x224   # ESP -> Pi

PING_PERIOD_SEC = 1.0


def make_pattern(counter: int) -> bytes:
    counter &= 0xFF
    return bytes([
        counter,
        counter ^ 0xFF,
        0x55,
        0xAA,
        0xC3,
        0x3C,
        0x5A,
        0xA5,
    ])


def pattern_matches(data: bytes) -> bool:
    data = bytes(data)
    if len(data) != 8:
        return False
    c = data[0]
    return (
        data[1] == (c ^ 0xFF) and
        data[2] == 0x55 and
        data[3] == 0xAA and
        data[4] == 0xC3 and
        data[5] == 0x3C and
        data[6] == 0x5A and
        data[7] == 0xA5
    )


class PingPongRunner:
    def __init__(self, channel: str = "can0"):
        self.channel = channel
        try:
            self.bus = can.Bus(interface="socketcan", channel=channel)
        except Exception as exc:  # noqa: BLE001 - we want to show any init failure
            print(f"Failed to open CAN interface {channel}: {exc}")
            sys.exit(1)

        self.last_pi_ping_data: Optional[bytes] = None
        self.pi_counter: int = 0
        self.next_pi_ping_at: float = time.monotonic()
        self.running = True

    def _send(self, msg: can.Message, label: str) -> None:
        try:
            self.bus.send(msg)
            print(label)
        except can.CanError as exc:
            print(f"ERROR sending {label}: {exc}")

    def _handle_rx(self, msg: can.Message) -> None:
        print(
            f"RX: ID=0x{msg.arbitration_id:X}, DLC={msg.dlc}, "
            f"DATA={bytes(msg.data).hex(' ')}"
        )

        if msg.is_extended_id or msg.dlc != 8:
            return  # ignore unsupported frames for this test

        # Case A: ESP-initiated PING that Pi must echo
        if msg.arbitration_id == ESP_PING_ID:
            if pattern_matches(msg.data):
                print("MATCHED (ESP->Pi PING)")
            else:
                print("MISMATCH pattern from ESP")

            pong = can.Message(
                arbitration_id=ESP_PONG_ID,
                is_extended_id=False,
                data=msg.data
            )
            self._send(pong, "TX PONG (Pi->ESP) in response to ESP PING")

        # Case B: PONG from ESP for Pi-initiated PING
        elif msg.arbitration_id == PI_PONG_ID:
            if self.last_pi_ping_data is not None and bytes(msg.data) == self.last_pi_ping_data:
                print("MATCHED (Pi-initiated)")
            else:
                print("MISMATCH (Pi-initiated)")

    def _send_pi_ping_if_due(self, now: float) -> None:
        if now < self.next_pi_ping_at:
            return

        data = make_pattern(self.pi_counter)
        self.last_pi_ping_data = data

        ping_msg = can.Message(
            arbitration_id=PI_PING_ID,
            is_extended_id=False,
            data=data
        )
        self._send(ping_msg, f"TX PING (Pi->ESP), counter={self.pi_counter}")

        self.pi_counter = (self.pi_counter + 1) & 0xFF
        self.next_pi_ping_at = now + PING_PERIOD_SEC

    def run(self) -> None:
        print(f"Starting ping-pong on {self.channel} (125kbps expected)...")

        while self.running:
            now = time.monotonic()
            self._send_pi_ping_if_due(now)

            try:
                msg = self.bus.recv(timeout=0.1)
            except can.CanError as exc:
                print(f"Receive error: {exc}")
                continue

            if msg is not None:
                self._handle_rx(msg)

    def stop(self) -> None:
        self.running = False
        self.bus.shutdown()
        print("Stopped.")


def main() -> None:
    runner = PingPongRunner(channel="can0")

    def _signal_handler(sig, frame):  # noqa: ANN001, D401 - signal signature
        del sig, frame
        runner.stop()
        sys.exit(0)

    signal.signal(signal.SIGINT, _signal_handler)
    signal.signal(signal.SIGTERM, _signal_handler)

    runner.run()


if __name__ == "__main__":
    main()
