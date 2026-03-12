#include <M5StickCPlus.h>
#include <esp_now.h>
#include <WiFi.h>

uint8_t macA[] = {0x0C, 0x8B, 0x95, 0xA8, 0x1D, 0x2C};
uint8_t macB[] = {0x4C, 0x75, 0x25, 0xCB, 0x89, 0x98};
uint8_t macC[] = {0x4C, 0x75, 0x25, 0xCB, 0x7E, 0x54};

struct ButtonPacket {
  uint8_t nodeId;
  unsigned long pressTime;
  uint8_t hopCount;
};

struct StartPacket {
  uint8_t type;
};

struct PressEvent {
  unsigned long timestamp;
  uint8_t hopCount;
  bool received;
};

PressEvent pressA = {0, 0, false};
PressEvent pressB = {0, 0, false};
PressEvent pressC = {0, 0, false};

bool roundActive = false;

void registerPeer(uint8_t* mac) {
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = 0;
  peer.encrypt = false;
  esp_now_add_peer(&peer);
}

void resetRound() {
  pressA = {0, 0, false};
  pressB = {0, 0, false};
  pressC = {0, 0, false};
  roundActive = false;
  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setCursor(10, 20);
  M5.Lcd.setTextSize(2);
  M5.Lcd.println("Press D btn\nto START!");
  Serial.println("--- Round reset. Press D button to START ---");
}

void broadcastStart() {
  StartPacket spkt;
  spkt.type = 0xAA;
  esp_now_send(macA, (uint8_t*)&spkt, sizeof(spkt));
  esp_now_send(macB, (uint8_t*)&spkt, sizeof(spkt));
  // C gets START via B

  roundActive = true;
  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setCursor(10, 20);
  M5.Lcd.setTextSize(3);
  M5.Lcd.println("GO!");
  Serial.println("START sent! Round is live.");
}

void declareWinner() {
  roundActive = false;

  struct Entry { const char* name; PressEvent* e; };
  Entry entries[] = {{"A", &pressA}, {"B", &pressB}, {"C", &pressC}};

  const char* winner = nullptr;
  unsigned long earliest = ULONG_MAX;
  for (auto& e : entries) {
    if (e.e->received && e.e->timestamp < earliest) {
      earliest = e.e->timestamp;
      winner = e.name;
    }
  }

  Serial.println("=== ROUND RESULT ===");
  for (auto& e : entries) {
    if (e.e->received) {
      Serial.printf("  Player %s | reactionTime: %lu ms | hopCount: %d | diff: +%lu ms\n",
        e.name, e.e->timestamp, e.e->hopCount, e.e->timestamp - earliest);
    } else {
      Serial.printf("  Player %s | did not press\n", e.name);
    }
  }
  if (winner) Serial.printf("  >> WINNER: Player %s <<\n", winner);
  Serial.println("====================");

  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setCursor(10, 10);
  M5.Lcd.setTextSize(2);
  if (winner) M5.Lcd.printf("Winner: %s!\n\n", winner);
  M5.Lcd.setTextSize(1);
  for (auto& e : entries) {
    if (e.e->received) {
      M5.Lcd.printf("%s: %lums hop:%d\n", e.name, e.e->timestamp, e.e->hopCount);
    } else {
      M5.Lcd.printf("%s: no press\n", e.name);
    }
  }

  delay(3000);
  resetRound();
}

void onDataReceived(const esp_now_recv_info *recvInfo, const uint8_t *data, int len) {
  if (!roundActive) return;
  if (len != sizeof(ButtonPacket)) return;

  ButtonPacket pkt;
  memcpy(&pkt, data, sizeof(pkt));

  Serial.printf("Received from Node %d | reactionTime: %lu ms | hopCount: %d\n",
    pkt.nodeId, pkt.pressTime, pkt.hopCount);

  if (pkt.nodeId == 1 && !pressA.received) {
    pressA = {pkt.pressTime, pkt.hopCount, true};
  } else if (pkt.nodeId == 2 && !pressB.received) {
    pressB = {pkt.pressTime, pkt.hopCount, true};
  } else if (pkt.nodeId == 3 && !pressC.received) {
    pressC = {pkt.pressTime, pkt.hopCount, true};
  }

  if (pressA.received && pressB.received && pressC.received) {
    declareWinner();
  }
}

void setup() {
  M5.begin();
  Serial.begin(115200);
  delay(1000);
  WiFi.mode(WIFI_STA);

  esp_now_init();
  esp_now_register_recv_cb(onDataReceived);

  registerPeer(macA);
  registerPeer(macB);
  registerPeer(macC);

  Serial.println("Node D server ready");
  resetRound();
}

void loop() {
  M5.update();
  if (M5.BtnA.wasPressed()) {
    if (!roundActive) {
      broadcastStart();
    } else {
      Serial.println("Manual reset!");
      resetRound();
    }
  }
}