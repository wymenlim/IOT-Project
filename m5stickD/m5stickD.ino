#include <M5StickCPlus.h>
#include <esp_now.h>
#include <WiFi.h>
#include "../game_protocol.h"

uint8_t myMac[6];
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
  peer.channel = ESPNOW_CHANNEL;
  peer.encrypt = false;
  char macStr[18];
  macToStr(mac, macStr);
  esp_err_t res = esp_now_add_peer(&peer);
  if (res == ESP_OK) {
    LOG("Registered peer %s on ch%d", macStr, ESPNOW_CHANNEL);
  } else {
    LOG("ERROR: Failed to register peer %s (err=%d)", macStr, res);
  }
}

void sendPacket(const uint8_t* mac, GamePacket &pkt, const char* label) {
  esp_err_t err = esp_now_send(mac, (uint8_t*)&pkt, sizeof(pkt));
  if (err != ESP_OK) {
    LOG("ERROR: %s failed immediately (err=%d)", label, err);
  }
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
  LOG("--- Round reset. Press D button to START ---");
}

void broadcastStart() {
  GamePacket goA;
  initPacket(goA, PACKET_GO, myMac, macA, myMac, nextPacketId(packetCounter), 0);
  LOG("SEND GO to A | id=%u", goA.packet_id);
  sendPacket(macA, goA, "GO to A");

  GamePacket goB;
  initPacket(goB, PACKET_GO, myMac, macB, myMac, nextPacketId(packetCounter), 0);
  LOG("SEND GO to B | id=%u", goB.packet_id);
  sendPacket(macB, goB, "GO to B");

  GamePacket goC;
  initPacket(goC, PACKET_GO, myMac, macC, myMac, nextPacketId(packetCounter), 0);
  LOG("SEND GO to C | id=%u", goC.packet_id);
  sendPacket(macC, goC, "GO to C");

  roundActive = true;
  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setCursor(10, 20);
  M5.Lcd.setTextSize(3);
  M5.Lcd.println("GO!");
  LOG("Round is live. Waiting for PRESS from A, B, C.");
}

void sendResultToPlayers() {
  GamePacket resultA;
  initPacket(resultA, PACKET_RESULT, myMac, macA, myMac, nextPacketId(packetCounter), 0);
  LOG("SEND RESULT to A | id=%u", resultA.packet_id);
  sendPacket(macA, resultA, "RESULT to A");

  GamePacket resultB;
  initPacket(resultB, PACKET_RESULT, myMac, macB, myMac, nextPacketId(packetCounter), 0);
  LOG("SEND RESULT to B | id=%u", resultB.packet_id);
  sendPacket(macB, resultB, "RESULT to B");

  GamePacket resultC;
  initPacket(resultC, PACKET_RESULT, myMac, macC, myMac, nextPacketId(packetCounter), 0);
  LOG("SEND RESULT to C | id=%u", resultC.packet_id);
  sendPacket(macC, resultC, "RESULT to C");
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

  LOG("=== ROUND RESULT ===");
  for (auto& e : entries) {
    if (e.e->received) {
      LOG("  Player %s | reactionMs=%lu | hop=%d | diff=+%lu ms",
          e.name, e.e->reactionMs, e.e->hopCount,
          e.e->reactionMs - earliest);
    } else {
      LOG("  Player %s | did not press", e.name);
    }
  }
  if (winner) {
    LOG("  >> WINNER: Player %s <<", winner);
  } else {
    LOG("  >> NO WINNER <<");
  }
  LOG("====================");

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
  char srcStr[18];
  macToStr(recvInfo->src_addr, srcStr);
  LOG("RECV from %s | len=%d | roundActive=%d", srcStr, len, roundActive);

  if (!roundActive) {
    LOG("DROP: round not active");
    return;
  }

  if (len != sizeof(GamePacket)) {
    LOG("DROP: wrong len (got %d, want %d)", len, (int)sizeof(GamePacket));
    return;
  }

  GamePacket pkt;
  memcpy(&pkt, data, sizeof(pkt));

  char originStr[18]; macToStr(pkt.origin_mac, originStr);
  char destStr[18];   macToStr(pkt.dest_mac, destStr);
  LOG("PKT type=%d origin=%s dest=%s hop=%d id=%u reaction_ms=%lu",
      pkt.type, originStr, destStr, pkt.hop_count, pkt.packet_id,
      (unsigned long)pkt.reaction_ms);

  if (pkt.type != PACKET_PRESS) {
    LOG("DROP: not a PRESS (type=%d)", pkt.type);
    return;
  }

  if (!isLocalMac(pkt.dest_mac, myMac)) {
    LOG("DROP: not for me (dest=%s)", destStr);
    return;
  }

  if (isDuplicateAndRemember(dedupCache, dedupIndex, pkt.origin_mac, pkt.packet_id)) {
    LOG("DROP: duplicate (origin=%s id=%u)", originStr, pkt.packet_id);
    return;
  }

  int playerIndex = playerIndexFromMac(pkt.origin_mac);
  if (playerIndex < 0) {
    LOG("DROP: unknown origin MAC %s", originStr);
    return;
  }

  const char* playerNames[] = {"A", "B", "C"};
  PressEvent* slots[] = {&pressA, &pressB, &pressC};

  if (slots[playerIndex]->received) {
    LOG("DROP: already have PRESS from player %s", playerNames[playerIndex]);
    return;
  }

  *slots[playerIndex] = {pkt.reaction_ms, pkt.hop_count, true};
  LOG("ACCEPTED PRESS from player %s | reaction_ms=%lu hop=%d",
      playerNames[playerIndex], (unsigned long)pkt.reaction_ms, pkt.hop_count);

  bool allReceived = pressA.received && pressB.received && pressC.received;
  LOG("Press state: A=%d B=%d C=%d", pressA.received, pressB.received, pressC.received);

  if (allReceived) {
    declareWinner();
  }
}

void setup() {
  M5.begin();
  Serial.begin(115200);
  delay(1000);
  WiFi.mode(WIFI_STA);
  esp_wifi_get_mac(WIFI_IF_STA, myMac);

  char actualStr[18];
  macToStr(myMac, actualStr);
  LOG("Node D | actual MAC: %s", actualStr);

  if (!configureEspNowChannel()) {
    LOG("ERROR: configureEspNowChannel() FAILED");
  } else {
    LOG("WiFi channel locked to %d", ESPNOW_CHANNEL);
  }

  if (esp_now_init() != ESP_OK) {
    LOG("ERROR: esp_now_init() FAILED — no traffic will flow");
  } else {
    LOG("esp_now_init() OK");
  }

  esp_now_register_send_cb([](const wifi_tx_info_t *info, esp_now_send_status_t status) {
    char macStr[18];
    macToStr(info->des_addr, macStr);
    LOG("SEND to %s: %s", macStr,
        status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL (no ack — wrong MAC or out of range?)");
  });

  esp_now_register_recv_cb(onDataReceived);

  registerPeer(macA);
  registerPeer(macB);
  registerPeer(macC);
  resetDedupCache(dedupCache);

  LOG("Node D server ready");
  resetRound();
}

void loop() {
  M5.update();
  if (M5.BtnA.wasPressed()) {
    if (!roundActive) {
      LOG("D button pressed | starting round");
      broadcastStart();
    } else {
      LOG("D button pressed | ending round early");
      declareWinner();
    }
  }
}
