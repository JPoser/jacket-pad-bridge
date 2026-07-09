/*
 * jacket-pad-bridge v1 — Bluepad32 controller receiver.
 *
 * An original dual-mode ESP32 (Classic + BLE) running Bluepad32 as a
 * Bluetooth HID host: it pairs with controllers the Tildagon badge's
 * BLE-only ESP32-S3 can't hear — 8BitDo pads (D mode), DualShock,
 * Switch Pro, and friends.
 *
 * v1 proves the controller link: every connection and button/D-pad
 * change is printed to USB serial. emit() is the seam where v2 will
 * forward events to the badge (UART into a hexpansion slot, or BLE
 * NUS speaking the games' Bluefruit packet protocol).
 *
 * Board: "ESP32 Dev Module" from the esp32-bluepad32 core
 * (FQBN esp32-bluepad32:esp32:esp32).
 */

#include <Bluepad32.h>

static ControllerPtr controllers[BP32_MAX_GAMEPADS];

// Last-seen state per slot, so we only report changes.
static uint8_t lastDpad[BP32_MAX_GAMEPADS];
static uint16_t lastButtons[BP32_MAX_GAMEPADS];
static uint16_t lastMisc[BP32_MAX_GAMEPADS];

// v2 will turn this into a real transport (UART/BLE). One event per
// press/release edge: kind is "dpad", "btn" or "misc"; bit names the
// control; down is the new state.
static void emit(int slot, const char* kind, const char* bit, bool down) {
  Serial.printf("EVT slot=%d %s %s %s\n", slot, kind, bit, down ? "down" : "up");
}

static void diffBits(int slot, const char* kind, uint16_t prev, uint16_t now,
                     const char* const names[], int count) {
  for (int i = 0; i < count; i++) {
    uint16_t mask = 1 << i;
    if ((prev ^ now) & mask) {
      emit(slot, kind, names[i], now & mask);
    }
  }
}

static const char* const DPAD_NAMES[] = {"up", "down", "right", "left"};
static const char* const BUTTON_NAMES[] = {
    "a", "b", "x", "y", "l1", "r1", "l2", "r2",
    "thumb_l", "thumb_r"};
static const char* const MISC_NAMES[] = {"system", "select", "start",
                                         "capture"};

static void onConnected(ControllerPtr ctl) {
  for (int i = 0; i < BP32_MAX_GAMEPADS; i++) {
    if (controllers[i] == nullptr) {
      controllers[i] = ctl;
      lastDpad[i] = 0;
      lastButtons[i] = 0;
      lastMisc[i] = 0;
      ControllerProperties p = ctl->getProperties();
      Serial.printf("CONNECTED slot=%d model='%s' vid=0x%04x pid=0x%04x\n",
                    i, ctl->getModelName().c_str(), p.vendor_id, p.product_id);
      return;
    }
  }
  Serial.println("CONNECTED but no free slot, ignoring");
}

static void onDisconnected(ControllerPtr ctl) {
  for (int i = 0; i < BP32_MAX_GAMEPADS; i++) {
    if (controllers[i] == ctl) {
      controllers[i] = nullptr;
      Serial.printf("DISCONNECTED slot=%d\n", i);
      return;
    }
  }
}

static void processController(int slot, ControllerPtr ctl) {
  if (!ctl->isGamepad()) {
    return;
  }
  uint8_t dpad = ctl->dpad();
  uint16_t buttons = ctl->buttons();
  uint16_t misc = ctl->miscButtons();

  diffBits(slot, "dpad", lastDpad[slot], dpad, DPAD_NAMES, 4);
  diffBits(slot, "btn", lastButtons[slot], buttons, BUTTON_NAMES, 10);
  diffBits(slot, "misc", lastMisc[slot], misc, MISC_NAMES, 4);

  lastDpad[slot] = dpad;
  lastButtons[slot] = buttons;
  lastMisc[slot] = misc;
}

void setup() {
  Serial.begin(115200);
  Serial.printf("jacket-pad-bridge v1, Bluepad32 fw %s\n",
                BP32.firmwareVersion());
  BP32.setup(&onConnected, &onDisconnected);
  // Bring-up convenience: drop stale bonds every boot so a blinking pad
  // always gets accepted. v2 should keep bonds instead.
  BP32.forgetBluetoothKeys();
  BP32.enableVirtualDevice(false);
  Serial.println("READY - put the pad in pairing mode");
}

void loop() {
  if (BP32.update()) {
    for (int i = 0; i < BP32_MAX_GAMEPADS; i++) {
      if (controllers[i] != nullptr && controllers[i]->isConnected() &&
          controllers[i]->hasData()) {
        processController(i, controllers[i]);
      }
    }
  }

  static uint32_t lastBeat = 0;
  if (millis() - lastBeat > 5000) {
    lastBeat = millis();
    Serial.println("...listening (bridge alive)");
  }
  vTaskDelay(1);
}
