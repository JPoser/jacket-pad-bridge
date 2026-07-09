# jacket-pad-bridge

A Bluetooth Classic → badge bridge, so ordinary Bluetooth gamepads can
drive the jacket games ([jac-man](../jac-man),
[jacket-space-invaders](../jacket-space-invaders)).

The Tildagon badge's ESP32-S3 is BLE-only; most controllers — 8BitDo
pads, DualShock, Switch Pro — speak Bluetooth *Classic*. This bridge is
an original dual-mode ESP32 (WROOM/WROVER dev board) running
[Bluepad32](https://bluepad32.readthedocs.io/) as a Bluetooth HID host:
it pairs with the controller and forwards button events to the badge.

Proven with an 8BitDo Micro in D-input mode (mode switch to **D**,
hold the pair button to bond).

## Status

- **v1 (this)**: controller → bridge link working; events printed to
  USB serial as `EVT slot=0 btn a down` lines. `emit()` in
  `bridge/bridge.ino` is the seam for the badge transport.
- **v2 (planned)**: forward events to the badge. Preferred: ESP-NOW
  (WiFi, coexists with Classic BT, no pairing, and the badge side can
  reuse the games' existing Bluefruit packet parser). Fallbacks: UART
  into a spare hexpansion slot, or a BTstack BLE central writing to the
  games' Nordic UART service.

## Build & flash

Uses the prebuilt Bluepad32 Arduino core (no ESP-IDF build needed):

```sh
brew install arduino-cli
arduino-cli config init --additional-urls \
  "https://raw.githubusercontent.com/ricardoquesada/esp32-arduino-lib-builder/master/bluepad32_files/package_esp32_bluepad32_index.json"
arduino-cli core update-index
arduino-cli core install esp32-bluepad32:esp32

arduino-cli compile --fqbn esp32-bluepad32:esp32:esp32 bridge
# Some boards need the slower upload speed:
arduino-cli upload -p /dev/cu.usbserial-XXX \
  --fqbn "esp32-bluepad32:esp32:esp32:UploadSpeed=115200" bridge
```

Watch it (`arduino-cli monitor` needs a real TTY; pyserial doesn't):

```sh
uv run --with pyserial python3 -c "
import serial
p = serial.Serial('/dev/cu.usbserial-XXX', 115200, timeout=1)
while True:
    print(p.readline().decode('utf-8', 'replace'), end='')"
```

Note: v1 calls `BP32.forgetBluetoothKeys()` on every boot (bring-up
convenience — a blinking pad is always accepted). v2 should keep bonds.
