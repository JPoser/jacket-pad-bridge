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
// The badge's STA MAC. Unicast frames get link-layer ACKs and ~7
// automatic retries — broadcasts have neither, and the badge's radio
// time-slices with BLE, silently eating unacknowledged frames. Set to
// your badge (mpremote: network.WLAN(STA_IF).config('mac')); zeroed =
// fall back to broadcast.
static const uint8_t BADGE_MAC[6] = {0x64, 0xe8, 0x33, 0x72, 0x12, 0x74};
static bool espnowUp = false;
static bool unicast = false;
static uint32_t sendFails = 0;

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
  // v6 field lesson: unicast ACK-and-retry sounded safer, but this
  // board's BT Classic link time-slices the one radio and clips the
  // ACK window - roughly half the frames burned the full retry budget,
  // the TX queue backed up, and inputs arrived seconds late (the badge
  // often HAD the frame while we logged it as failed). Broadcast is
  // fire-and-forget: with the badge receiver held awake by padlink's
  // PM_NONE watchdog, a rarely lost press beats a reliably late one.
  esp_err_t err = esp_now_send(BROADCAST, pkt, sizeof(pkt));
  if (err != ESP_OK) {
    Serial.printf("espnow send failed: %d\n", err);
  }
}

// Delivery report per unicast frame (broadcasts always claim success).
static void onSent(const uint8_t* mac, esp_now_send_status_t status) {
  if (status != ESP_NOW_SEND_SUCCESS) {
    sendFails++;
    Serial.printf("espnow DELIVERY FAILED (%lu total) - is the badge "
                  "listening on channel %d?\n",
                  (unsigned long)sendFails, ESPNOW_CHANNEL);
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
  // The bridge only ever transmits; let the WiFi RX chain sleep. The
  // radio wakes to send, which costs ~a millisecond we don't feel.
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
  esp_now_register_send_cb(&onSent);
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BROADCAST, 6);
  peer.channel = 0;  // 0 = whatever the radio is on (pinned above)
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("ESP-NOW add_peer FAILED - serial events only");
    return;
  }
  // Prefer unicast to the badge when a MAC is configured.
  bool haveMac = false;
  for (int i = 0; i < 6; i++) {
    if (BADGE_MAC[i]) {
      haveMac = true;
    }
  }
  if (haveMac) {
    esp_now_peer_info_t badge = {};
    memcpy(badge.peer_addr, BADGE_MAC, 6);
    badge.channel = 0;
    badge.ifidx = WIFI_IF_STA;
    badge.encrypt = false;
    // Peer kept registered for diagnostics, but sends are broadcast
    // (see the v6 note in sendBluefruit).
    unicast = (esp_now_add_peer(&badge) == ESP_OK);
  }
  espnowUp = true;
  Serial.printf("ESP-NOW up on channel %d (%s)\n", ESPNOW_CHANNEL,
                unicast ? "unicast to badge" : "broadcast");
}

static int connectedCount() {
  int n = 0;
  for (int i = 0; i < BP32_MAX_GAMEPADS; i++) {
    if (controllers[i] != nullptr) {
      n++;
    }
  }
  return n;
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
      // Pad's aboard: stop the continuous inquiry/page scanning. This is
      // the bridge's biggest radio (and heat) saving, and stops anyone
      // else pairing mid-game.
      BP32.enableNewBluetoothConnections(false);
      Serial.println("scanning off (reopens on disconnect)");
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
      if (connectedCount() == 0) {
        BP32.enableNewBluetoothConnections(true);
        Serial.println("scanning on");
      }
      return;
    }
  }
}

// Many pads in D-input mode (the 8BitDo Micro included) report the
// D-pad as the left-stick axes rather than the HID hat that dpad()
// reads. Synthesize dpad bits from axis extremes so both styles work.
// Bluepad32 axes run -512..511; half-deflection is a solid press.
static uint8_t axisDpad(ControllerPtr ctl) {
  const int T = 256;
  uint8_t d = 0;
  if (ctl->axisY() < -T) d |= DPAD_UP;
  if (ctl->axisY() > T) d |= DPAD_DOWN;
  if (ctl->axisX() > T) d |= DPAD_RIGHT;
  if (ctl->axisX() < -T) d |= DPAD_LEFT;
  return d;
}

static void processController(int slot, ControllerPtr ctl) {
  if (!ctl->isGamepad()) {
    return;
  }
  uint8_t dpad = ctl->dpad() | axisDpad(ctl);
  uint16_t buttons = ctl->buttons();
  uint16_t misc = ctl->miscButtons();

  // Bring-up visibility: raw state whenever something is held, max 2/s.
  static uint32_t lastDump = 0;
  if ((dpad || buttons || misc) && millis() - lastDump > 500) {
    lastDump = millis();
    Serial.printf("RAW dpad=0x%02x hat=0x%02x ax=%4d ay=%4d "
                  "btn=0x%04x misc=0x%02x\n",
                  dpad, ctl->dpad(), ctl->axisX(), ctl->axisY(),
                  buttons, misc);
  }

  diffBits(slot, "dpad", lastDpad[slot], dpad, DPAD_NAMES, 4);
  diffBits(slot, "btn", lastButtons[slot], buttons, BUTTON_NAMES, 10);
  diffBits(slot, "misc", lastMisc[slot], misc, MISC_NAMES, 4);

  lastDpad[slot] = dpad;
  lastButtons[slot] = buttons;
  lastMisc[slot] = misc;
}

void setup() {
  // Button parsing doesn't need 240MHz; 80 is the radio minimum and
  // runs markedly cooler on a powerbank.
  setCpuFrequencyMhz(80);
  Serial.begin(115200);
  Serial.printf("jacket-pad-bridge v3, Bluepad32 fw %s, cpu %dMHz\n",
                BP32.firmwareVersion(), getCpuFrequencyMhz());
  setupEspNow();
  BP32.setup(&onConnected, &onDisconnected);
  // Bonds are kept across reboots: pair the pad once (hold its pair
  // button), and from then on it just reconnects. To force a fresh
  // pairing, temporarily uncomment:
  // BP32.forgetBluetoothKeys();
  BP32.enableVirtualDevice(false);
  Serial.println("READY - bonded pads reconnect; new pads: pairing mode");
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
