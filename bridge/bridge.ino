/*
 * jacket-pad-bridge v2 — Bluepad32 controller receiver + ESP-NOW link.
 *
 * An original dual-mode ESP32 (Classic + BLE) running Bluepad32 as a
 * Bluetooth HID host: it pairs with controllers the Tildagon badge's
 * BLE-only ESP32-S3 can't hear — 8BitDo pads (D mode), DualShock,
 * Switch Pro, and friends.
 *
 * Every button/D-pad edge is broadcast over ESP-NOW as a 5-byte
 * Bluefruit control-pad packet ('!' 'B' <button '1'-'8'> <'1'/'0'>
 * <crc>) — the same bytes the jacket games' BLE phone path already
 * parses, so the badge side reuses that parser verbatim. Events also
 * print to USB serial for debugging.
 *
 * Channel note: ESP-NOW needs both ends on the same WiFi channel. The
 * bridge pins ESPNOW_CHANNEL below; a badge with WiFi idle sits on
 * channel 1. If the badge ever joins an AP, match its channel here.
 *
 * Board: "ESP32 Dev Module" from the esp32-bluepad32 core
 * (FQBN esp32-bluepad32:esp32:esp32).
 */

#include <Bluepad32.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

static const int ESPNOW_CHANNEL = 1;
static const uint8_t BROADCAST[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
static bool espnowUp = false;

static ControllerPtr controllers[BP32_MAX_GAMEPADS];

// Last-seen state per slot, so we only report changes.
static uint8_t lastDpad[BP32_MAX_GAMEPADS];
static uint16_t lastButtons[BP32_MAX_GAMEPADS];
static uint16_t lastMisc[BP32_MAX_GAMEPADS];

// Controls → Bluefruit control-pad button characters, matching what the
// games' _on_ble_press handlers expect: D-pad '5'-'8' (up/down/left/
// right), '1' restart (start/select), '2'-'4' the face buttons (all
// read as "fire" in JacVaders; ignored by JacMan).
static char mapToBluefruit(const char* kind, const char* bit) {
  if (strcmp(kind, "dpad") == 0) {
    if (strcmp(bit, "up") == 0) return '5';
    if (strcmp(bit, "down") == 0) return '6';
    if (strcmp(bit, "left") == 0) return '7';
    if (strcmp(bit, "right") == 0) return '8';
  } else if (strcmp(kind, "btn") == 0) {
    if (strcmp(bit, "a") == 0) return '2';
    if (strcmp(bit, "b") == 0) return '3';
    if (strcmp(bit, "x") == 0) return '4';
    if (strcmp(bit, "y") == 0) return '2';
  } else if (strcmp(kind, "misc") == 0) {
    if (strcmp(bit, "start") == 0 || strcmp(bit, "select") == 0) return '1';
  }
  return 0;
}

static void sendBluefruit(char button, bool down) {
  if (!espnowUp) {
    return;
  }
  uint8_t pkt[5];
  pkt[0] = '!';
  pkt[1] = 'B';
  pkt[2] = (uint8_t)button;
  pkt[3] = down ? '1' : '0';
  pkt[4] = (uint8_t)~(pkt[0] + pkt[1] + pkt[2] + pkt[3]);
  esp_err_t err = esp_now_send(BROADCAST, pkt, sizeof(pkt));
  if (err != ESP_OK) {
    Serial.printf("espnow send failed: %d\n", err);
  }
}

// One event per press/release edge: kind is "dpad", "btn" or "misc";
// bit names the control; down is the new state.
static void emit(int slot, const char* kind, const char* bit, bool down) {
  Serial.printf("EVT slot=%d %s %s %s\n", slot, kind, bit, down ? "down" : "up");
  char button = mapToBluefruit(kind, bit);
  if (button) {
    sendBluefruit(button, down);
  }
}

static void setupEspNow() {
  WiFi.mode(WIFI_STA);
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init FAILED - serial events only");
    return;
  }
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BROADCAST, 6);
  peer.channel = 0;  // 0 = whatever the radio is on (pinned above)
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("ESP-NOW add_peer FAILED - serial events only");
    return;
  }
  espnowUp = true;
  Serial.printf("ESP-NOW up on channel %d (broadcast)\n", ESPNOW_CHANNEL);
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
  Serial.printf("jacket-pad-bridge v2, Bluepad32 fw %s\n",
                BP32.firmwareVersion());
  setupEspNow();
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
