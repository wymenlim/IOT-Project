#include <M5StickCPlus.h>
#include <esp_now.h>
#include <WiFi.h>
#include "../game_protocol.h"

uint8_t myMac[] = {0xD4, 0xD4, 0xDA, 0x85, 0x4D, 0x98};
uint8_t macA[] = {0x0C, 0x8B, 0x95, 0xA8, 0x1D, 0x2C};
uint8_t macB[] = {0x4C, 0x75, 0x25, 0xCB, 0x89, 0x98};
uint8_t macC[] = {0x4C, 0x75, 0x25, 0xCB, 0x7E, 0x54};

struct PressEvent {
  unsigned long reactionMs;
  uint8_t hopCount;
  bool received;
};

PressEvent pressA = {0, 0, false};
PressEvent pressB = {0, 0, false};
PressEvent pressC = {0, 0, false};

bool roundActive = false;
uint16_t packetCounter = 0;
DedupEntry dedupCache[DEDUP_CACHE_SIZE];
uint8_t dedupIndex = 0;

void registerPeer(uint8_t* mac) {
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = 0;
  peer.encrypt = false;
  esp_now_add_peer(&peer);
}

int playerIndexFromMac(const uint8_t originMac[6]) {
  if (macEquals(originMac, macA)) return 0;
  if (macEquals(originMac, macB)) return 1;
  if (macEquals(originMac, macC)) return 2;
  return -1;
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
  GamePacket goA;
  initPacket(goA, PACKET_GO, myMac, macA, myMac, nextPacketId(packetCounter), 0);
  esp_now_send(macA, (uint8_t*)&goA, sizeof(goA));

  GamePacket goB;
  initPacket(goB, PACKET_GO, myMac, macB, myMac, nextPacketId(packetCounter), 0);
  esp_now_send(macB, (uint8_t*)&goB, sizeof(goB));
  // C gets GO via B

  roundActive = true;
  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setCursor(10, 20);
  M5.Lcd.setTextSize(3);
  M5.Lcd.println("GO!");
  Serial.println("START sent! Round is live.");
}

void sendResultToPlayers() {
  GamePacket resultA;
  initPacket(resultA, PACKET_RESULT, myMac, macA, myMac, nextPacketId(packetCounter), 0);
  esp_now_send(macA, (uint8_t*)&resultA, sizeof(resultA));

  GamePacket resultB;
  initPacket(resultB, PACKET_RESULT, myMac, macB, myMac, nextPacketId(packetCounter), 0);
  esp_now_send(macB, (uint8_t*)&resultB, sizeof(resultB));
}

void declareWinner() {
  roundActive = false;

  struct Entry { const char* name; PressEvent* e; };
  Entry entries[] = {{"A", &pressA}, {"B", &pressB}, {"C", &pressC}};

  const char* winner = nullptr;
  unsigned long earliest = ULONG_MAX;
  for (auto& e : entries) {
    if (e.e->received && e.e->reactionMs < earliest) {
      earliest = e.e->reactionMs;
      winner = e.name;
    }
  }

  Serial.println("=== ROUND RESULT ===");
  for (auto& e : entries) {
    if (e.e->received) {
      Serial.printf("  Player %s | reactionTime: %lu ms | hopCount: %d",
        e.name, e.e->reactionMs, e.e->hopCount);
      if (winner) {
        Serial.printf(" | diff: +%lu ms\n", e.e->reactionMs - earliest);
      } else {
        Serial.printf("\n");
      }
    } else {
      Serial.printf("  Player %s | did not press\n", e.name);
    }
  }
  if (winner) {
    Serial.printf("  >> WINNER: Player %s <<\n", winner);
  } else {
    Serial.println("  >> NO WINNER <<");
  }
  Serial.println("====================");

  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setCursor(10, 10);
  M5.Lcd.setTextSize(2);
  if (winner) {
    M5.Lcd.printf("Winner: %s!\n\n", winner);
  } else {
    M5.Lcd.println("No winner\n");
  }
  M5.Lcd.setTextSize(1);
  for (auto& e : entries) {
    if (e.e->received) {
      M5.Lcd.printf("%s: %lums hop:%d\n", e.name, e.e->reactionMs, e.e->hopCount);
    } else {
      M5.Lcd.printf("%s: no press\n", e.name);
    }
  }

  sendResultToPlayers();
  delay(3000);
  resetRound();
}

void onDataReceived(const esp_now_recv_info *recvInfo, const uint8_t *data, int len) {
  if (!roundActive) return;
  if (len != sizeof(GamePacket)) return;

  GamePacket pkt;
  memcpy(&pkt, data, sizeof(pkt));

  if (pkt.type != PACKET_PRESS) return;
  if (!isLocalMac(pkt.dest_mac, myMac)) return;
  if (isDuplicateAndRemember(dedupCache, dedupIndex, pkt.origin_mac, pkt.packet_id)) return;

  int playerIndex = playerIndexFromMac(pkt.origin_mac);
  if (playerIndex == 0 && !pressA.received) {
    pressA = {pkt.reaction_ms, pkt.hop_count, true};
    Serial.printf("[D] Player A pressed | reaction time: %lu ms | hopCount: %d\n",
      pkt.reaction_ms, pkt.hop_count);
  } else if (playerIndex == 1 && !pressB.received) {
    pressB = {pkt.reaction_ms, pkt.hop_count, true};
    Serial.printf("[D] Player B pressed | reaction time: %lu ms | hopCount: %d\n",
      pkt.reaction_ms, pkt.hop_count);
  } else if (playerIndex == 2 && !pressC.received) {
    pressC = {pkt.reaction_ms, pkt.hop_count, true};
    Serial.printf("[D] Player C pressed | reaction time: %lu ms | hopCount: %d\n",
      pkt.reaction_ms, pkt.hop_count);
  } else {
    return;
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
  resetDedupCache(dedupCache);

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
      sendResultToPlayers();
      resetRound();
    }
  }
}
