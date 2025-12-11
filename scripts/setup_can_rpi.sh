#!/usr/bin/env bash
set -euo pipefail

# Configure Raspberry Pi overlays for MCP2515 on SPI and optionally bring up CAN.
# Defaults match this project: 8 MHz crystal, can0, 125 kbps, interrupt GPIO25.
#
# Usage:
#   sudo ./scripts/setup_can_rpi.sh [--config /boot/config.txt] [--channel can0] \
#       [--bitrate 125000] [--oscillator 8000000] [--interrupt 25] \
#       [--restart-ms 100] [--txqueuelen 1024] [--triple-sampling]
#
# Notes:
# - Must be run as root (sudo).
# - Supports can0/can1 overlays (mcp2515-can0 / mcp2515-can1).

CONFIG_PATH=""
CHANNEL="can0"
BITRATE=125000
OSCILLATOR=8000000
INT_GPIO=25
RESTART_MS=100
TX_QUEUELEN=1024
TRIPLE_SAMPLING=0

usage() {
  echo "Usage: sudo $0 [--config PATH] [--channel can0|can1] [--bitrate N] [--oscillator N] [--interrupt GPIO] [--restart-ms N] [--txqueuelen N] [--triple-sampling]" >&2
  exit 1
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --config) CONFIG_PATH="$2"; shift 2 ;;
    --channel) CHANNEL="$2"; shift 2 ;;
    --bitrate) BITRATE="$2"; shift 2 ;;
    --oscillator) OSCILLATOR="$2"; shift 2 ;;
    --interrupt) INT_GPIO="$2"; shift 2 ;;
    --restart-ms) RESTART_MS="$2"; shift 2 ;;
    --txqueuelen) TX_QUEUELEN="$2"; shift 2 ;;
    --triple-sampling) TRIPLE_SAMPLING=1; shift 1 ;;
    -h|--help) usage ;;
    *) echo "Unknown arg: $1" >&2; usage ;;
  esac
done

if [[ "$(id -u)" -ne 0 ]]; then
  echo "Run this script as root (sudo)." >&2
  exit 1
fi

if [[ -z "$CONFIG_PATH" ]]; then
  for candidate in /boot/firmware/config.txt /boot/config.txt; do
    if [[ -f "$candidate" ]]; then
      CONFIG_PATH="$candidate"
      break
    fi
  done
fi

if [[ -z "$CONFIG_PATH" ]]; then
  echo "Could not find /boot/config.txt (or /boot/firmware/config.txt). Pass --config <path>." >&2
  exit 1
fi

case "$CHANNEL" in
  can0) OVERLAY="mcp2515-can0" ;;
  can1) OVERLAY="mcp2515-can1" ;;
  *) echo "Unsupported channel: $CHANNEL (expected can0 or can1)" >&2; exit 1 ;;
esac

backup_path="${CONFIG_PATH}.bak.$(date +%Y%m%d-%H%M%S)"
cp "$CONFIG_PATH" "$backup_path"
echo "Backed up $CONFIG_PATH to $backup_path"

# Remove any existing MCP2515 overlay lines to avoid conflicting configuration.
tmp_cfg="$(mktemp)"
while IFS= read -r line || [[ -n "$line" ]]; do
  if [[ "$line" =~ ^dtoverlay=mcp2515 ]]; then
    echo "Removed existing overlay line: $line"
    continue
  fi
  echo "$line" >> "$tmp_cfg"
done < "$CONFIG_PATH"
mv "$tmp_cfg" "$CONFIG_PATH"

ensure_line() {
  local line="$1"
  if ! grep -Fxq "$line" "$CONFIG_PATH"; then
    echo "$line" >> "$CONFIG_PATH"
    echo "Added: $line"
  else
    echo "Already present: $line"
  fi
}

ensure_line "dtparam=spi=on"
ensure_line "dtoverlay=${OVERLAY},oscillator=${OSCILLATOR},interrupt=${INT_GPIO}"
ensure_line "dtoverlay=spi-bcm2835"
ensure_line "dtoverlay=spi-dma"

echo
echo "Overlay config updated in $CONFIG_PATH."

# Bring up interface immediately if present.
if ip link show "$CHANNEL" >/dev/null 2>&1; then
  echo "Configuring $CHANNEL at ${BITRATE} bps..."
  ip link set "$CHANNEL" down || true

  TYPE_ARGS=(type can bitrate "$BITRATE" restart-ms "$RESTART_MS")
  if [[ "$TRIPLE_SAMPLING" -eq 1 ]]; then
    TYPE_ARGS+=(triple-sampling on)
  fi
  ip link set "$CHANNEL" "${TYPE_ARGS[@]}"

  ip link set "$CHANNEL" txqueuelen "$TX_QUEUELEN"
  ip link set "$CHANNEL" up
  ip -s -d link show "$CHANNEL"
else
  echo "Interface $CHANNEL not present yet. Reboot to load the overlays, then run:"
  echo "  sudo ip link set $CHANNEL down"
  echo "  sudo ip link set $CHANNEL type can bitrate $BITRATE restart-ms $RESTART_MS${TRIPLE_SAMPLING:+ triple-sampling on}"
  echo "  sudo ip link set $CHANNEL txqueuelen $TX_QUEUELEN"
  echo "  sudo ip link set $CHANNEL up"
fi

echo
echo "Reboot recommended: sudo reboot"
