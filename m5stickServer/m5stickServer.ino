#include <M5StickCPlus.h>
#include <esp_now.h>
#include <WiFi.h>
#include "game_protocol.h"
#include "espnow_utils.h"
#include "general_utils.h"

uint8_t myMac[6];
uint8_t macA[] = {0x0C, 0x8B, 0x95, 0xA8, 0x1D, 0x2C};
uint8_t macB[] = {0x4C, 0x75, 0x25, 0xCB, 0x89, 0x98};
uint8_t macC[] = {0x4C, 0x75, 0x25, 0xCB, 0x7E, 0x54};
uint8_t broadcastMac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

struct PressEvent {
  unsigned long reactionMs;
  uint8_t hopCount;
  bool received;
};

PressEvent pressA = {0, 0, false};
PressEvent pressB = {0, 0, false};
PressEvent pressC = {0, 0, false};
bool playerActiveA = false;
bool playerActiveB = false;
bool playerActiveC = false;

bool roundActive = false;
bool winnerPending = false;
uint16_t packetCounter = 0;
SeenEntry seenTable[MAX_SEEN_ENTRIES];
RouteEntry routeTable[MAX_ROUTE_ENTRIES];

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
  playerActiveA = false;
  playerActiveB = false;
  playerActiveC = false;
  roundActive = false;
  winnerPending = false;
  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setCursor(10, 20);
  M5.Lcd.setTextSize(2);
  M5.Lcd.println("Press D btn\nto START!");
  LOG("--- Round reset. Press D button to START ---");
}

bool sendGoToPlayer(const uint8_t destMac[6], const char *playerName) {
  GamePacket goPkt;
  initPacket(goPkt, PACKET_GO, myMac, destMac, myMac,
             nextPacketId(packetCounter), 0, DEFAULT_TTL);
  char label[24];
  snprintf(label, sizeof(label), "GO to %s", playerName);
  return sendViaRoute(routeTable, destMac, goPkt, label);
}

void broadcastStart() {
  bool sentA = sendGoToPlayer(macA, "A");
  bool sentB = sendGoToPlayer(macB, "B");
  bool sentC = sendGoToPlayer(macC, "C");

  if (!(sentA || sentB || sentC)) {
    LOG("GO: aborting round start because no players have a route");
    return;
  }

  playerActiveA = sentA;
  playerActiveB = sentB;
  playerActiveC = sentC;
  roundActive = true;
  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setCursor(10, 20);
  M5.Lcd.setTextSize(3);
  M5.Lcd.println("GO!");
  LOG("Round is live. Waiting for active players: A=%d B=%d C=%d",
      playerActiveA, playerActiveB, playerActiveC);
}

void sendResultToPlayers() {
  if (playerActiveA) {
    GamePacket resultA;
    initPacket(resultA, PACKET_RESULT, myMac, macA, myMac, nextPacketId(packetCounter), 0, DEFAULT_TTL);
    LOG("SEND RESULT to A | id=%u", resultA.packet_id);
    sendPacket(macA, resultA, "RESULT to A");
  }

  if (playerActiveB) {
    GamePacket resultB;
    initPacket(resultB, PACKET_RESULT, myMac, macB, myMac, nextPacketId(packetCounter), 0, DEFAULT_TTL);
    LOG("SEND RESULT to B | id=%u", resultB.packet_id);
    sendPacket(macB, resultB, "RESULT to B");
  }

  if (playerActiveC) {
    GamePacket resultC;
    initPacket(resultC, PACKET_RESULT, myMac, macC, myMac, nextPacketId(packetCounter), 0, DEFAULT_TTL);
    LOG("SEND RESULT to C | id=%u", resultC.packet_id);
    sendPacket(macC, resultC, "RESULT to C");
  }
}

void declareWinner() {
  roundActive = false;
  winnerPending = false;

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
    } else if ((strcmp(e.name, "A") == 0 && !playerActiveA) ||
               (strcmp(e.name, "B") == 0 && !playerActiveB) ||
               (strcmp(e.name, "C") == 0 && !playerActiveC)) {
      LOG("  Player %s | not active this round", e.name);
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
    } else if ((strcmp(e.name, "A") == 0 && !playerActiveA) ||
               (strcmp(e.name, "B") == 0 && !playerActiveB) ||
               (strcmp(e.name, "C") == 0 && !playerActiveC)) {
      M5.Lcd.printf("%s: inactive\n", e.name);
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

  if (pkt.type == PACKET_RREQ) {
    if (isLocalMac(pkt.origin_mac, myMac)) {
      LOG("RREQ: ignore self-origin");
      return;
    }

    bool already_seen = isSeen(seenTable, pkt.origin_mac, pkt.packet_id);
    addRoute(routeTable, pkt.origin_mac, recvInfo->src_addr, pkt.hop_count + 1);
    if (already_seen) {
      LOG("DROP: duplicate (origin=%s id=%u)", originStr, pkt.packet_id);
      return;
    }
    markSeen(seenTable, pkt.origin_mac, pkt.packet_id);

    LOG("RREQ: reverse route origin=%s via=%s hops=%d", originStr, srcStr, pkt.hop_count + 1);

    if (isLocalMac(pkt.dest_mac, myMac)) {
      GamePacket rrep;
      initPacket(rrep, PACKET_RREP, myMac, pkt.origin_mac, myMac,
                 nextPacketId(packetCounter), 0, DEFAULT_TTL);
      registerPeerIfNeeded(recvInfo->src_addr);
      if (sendPacket(recvInfo->src_addr, rrep, "RREP")) {
        LOG("RREQ: I am dest, sent RREP to %s", srcStr);
      } else {
        LOG("RREQ: failed to send RREP to %s", srcStr);
      }
    } else if (pkt.ttl > 1) {
      setRelayFields(pkt, myMac);
      delay(random(RREQ_JITTER_MIN_MS, RREQ_JITTER_MAX_MS + 1));
      if (sendPacket(broadcastMac, pkt, "RREQ relay")) {
        LOG("RREQ: relayed (ttl=%d hop=%d)", pkt.ttl, pkt.hop_count);
      } else {
        LOG("RREQ: relay send failed");
      }
    } else {
      LOG("RREQ: DROP ttl exhausted");
    }
    return;
  }

  if (!roundActive) {
    LOG("DROP: round not active");
    return;
  }

  if (pkt.type != PACKET_PRESS) {
    LOG("DROP: not a PRESS (type=%d)", pkt.type);
    return;
  }

  if (!isLocalMac(pkt.dest_mac, myMac)) {
    LOG("DROP: not for me (dest=%s)", destStr);
    return;
  }

  if (seenCheck(seenTable, pkt.origin_mac, pkt.packet_id)) {
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
  bool playerActive[] = {playerActiveA, playerActiveB, playerActiveC};

  if (!playerActive[playerIndex]) {
    LOG("DROP: player %s not active in this round", playerNames[playerIndex]);
    return;
  }

  if (slots[playerIndex]->received) {
    LOG("DUP PRESS from player %s | re-sending ACK", playerNames[playerIndex]);

    GamePacket ackPkt;
    initPacket(ackPkt, PACKET_ACK, myMac, pkt.origin_mac, myMac,
               pkt.packet_id, 0, DEFAULT_TTL);
    if (!sendViaRoute(routeTable, pkt.origin_mac, ackPkt, "PRESS ACK duplicate")) {
      LOG("ACK: failed to route back to player %s", playerNames[playerIndex]);
    }
    return;
  }

  *slots[playerIndex] = {pkt.reaction_ms, pkt.hop_count, true};
  LOG("ACCEPTED PRESS from player %s | reaction_ms=%lu hop=%d",
      playerNames[playerIndex], (unsigned long)pkt.reaction_ms, pkt.hop_count);

  GamePacket ackPkt;
  initPacket(ackPkt, PACKET_ACK, myMac, pkt.origin_mac, myMac,
             pkt.packet_id, 0, DEFAULT_TTL);
  if (!sendViaRoute(routeTable, pkt.origin_mac, ackPkt, "PRESS ACK")) {
    LOG("ACK: failed to route back to player %s", playerNames[playerIndex]);
  }

  bool allReceived = (!playerActiveA || pressA.received) &&
                     (!playerActiveB || pressB.received) &&
                     (!playerActiveC || pressC.received);
  LOG("Press state: A=%d/%d B=%d/%d C=%d/%d",
      pressA.received, playerActiveA,
      pressB.received, playerActiveB,
      pressC.received, playerActiveC);

  if (allReceived) {
    winnerPending = true;
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
  LOG("Server | actual MAC: %s", actualStr);

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
  registerPeer(broadcastMac);
  resetSeenTable(seenTable);
  resetRouteTable(routeTable);

  LOG("Server ready");
  resetRound();
}

void loop() {
  M5.update();
  if (winnerPending) {
    declareWinner();
    return;
  }

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
