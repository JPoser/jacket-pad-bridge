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

Working end to end: controller events are broadcast over **ESP-NOW**
(channel 1) as 5-byte Bluefruit control-pad packets — the exact bytes
the games' phone path parses — and received on the badge by their
`padlink.py`. Mapping: D-pad → `'5'`-`'8'`, face buttons → `'2'`-`'4'`
(fire in JacVaders), start/select → `'1'` (restart). Events also print
to USB serial for debugging.

Power/heat (v3): CPU pinned to 80MHz, WiFi modem sleep on (the bridge
only transmits), and Bluetooth scanning stops while a controller is
connected — reopens on disconnect. Bonds persist across reboots: pair
a pad once with its pair button, thereafter it reconnects on power-up.

Powerbank note: the savings can drop idle draw near the auto-shutoff
threshold of some powerbanks. If yours powers off after a few minutes,
use one with a trickle/low-current mode.

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
