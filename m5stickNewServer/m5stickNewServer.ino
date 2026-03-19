#include <M5StickCPlus.h>
#include <esp_now.h>
#include <WiFi.h>
#include "game_protocol.h"
#include "espnow_utils.h"
#include "auth_utils.h"

uint8_t myMac[6];
uint8_t broadcastMac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

#define MAX_PLAYERS 10

// ============ DYNAMIC PLAYER MANAGEMENT ============
struct PlayerInfo {
  uint8_t mac[6];
  bool active;
  bool hasRoute;
  unsigned long discoveryTime;
};

struct PressEvent {
  unsigned long reactionMs;
  uint8_t hopCount;
  bool received;
};

// ============ GAME STATE ============
bool roundActive = false;
bool winnerPending = false;
uint16_t packetCounter = 0;
SeenEntry seenTable[MAX_SEEN_ENTRIES];
RouteEntry routeTable[MAX_ROUTE_ENTRIES];

PlayerInfo players[MAX_PLAYERS];
PressEvent playerPresses[MAX_PLAYERS];
int activePlayerCount = 0;

void registerPlayer(const uint8_t playerMac[6]) {
  // Check if already registered
  for (int i = 0; i < activePlayerCount; i++) {
    if (macEquals(players[i].mac, playerMac)) {
      return; // Already registered
    }
  }
  
  // Add new player
  if (activePlayerCount < MAX_PLAYERS) {
    copyMac(players[activePlayerCount].mac, playerMac);
    players[activePlayerCount].active = true;
    players[activePlayerCount].hasRoute = false;
    players[activePlayerCount].discoveryTime = millis();
    
    char macStr[18];
    macToStr(playerMac, macStr);
    LOG("New player registered: %s (%d total)", macStr, activePlayerCount + 1);
    activePlayerCount++;
  } else {
    LOG("Cannot register more players (MAX_PLAYERS=%d reached)", MAX_PLAYERS);
  }
}

int findPlayerIndex(const uint8_t playerMac[6]) {
  for (int i = 0; i < activePlayerCount; i++) {
    if (macEquals(players[i].mac, playerMac)) {
      return i;
    }
  }
  return -1;
}

void updatePlayerRoute(const uint8_t playerMac[6]) {
  int idx = findPlayerIndex(playerMac);
  if (idx >= 0) {
    players[idx].hasRoute = (findRoute(routeTable, playerMac) >= 0);
  }
}

int countValidRoutes() {
  int count = 0;
  for (int i = 0; i < activePlayerCount; i++) {
    updatePlayerRoute(players[i].mac);
    if (players[i].hasRoute) {
      count++;
    }
  }
  return count;
}

void resetPlayerPresses() {
  for (int i = 0; i < activePlayerCount; i++) {
    playerPresses[i] = {0, 0, false};
  }
}

void resetRound() {
  resetPlayerPresses();
  roundActive = false;
  winnerPending = false;
  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setCursor(10, 20);
  M5.Lcd.setTextSize(2);
  M5.Lcd.printf("Players: %d\n", activePlayerCount);
  M5.Lcd.setTextSize(1);
  M5.Lcd.println("Press A to START!");
  LOG("--- Round reset. %d players registered ---", activePlayerCount);
}

bool sendGoToPlayer(const uint8_t destMac[6], int playerIdx) {
  char macStr[18];
  macToStr(destMac, macStr);
  
  // CHECK: Verify route exists before sending
  if (findRoute(routeTable, destMac) < 0) {
    LOG("GO: player %d (%s) has NO ROUTE - dropping", playerIdx, macStr);
    return false;
  }
  
  GamePacket goPkt;
  initPacket(goPkt, PACKET_GO, myMac, destMac, myMac,
             nextPacketId(packetCounter), 0, DEFAULT_TTL);
  char label[32];
  snprintf(label, sizeof(label), "GO to player %d (%s)", playerIdx, macStr);
  
  bool success = sendViaRoute(routeTable, destMac, goPkt, label);
  if (success) {
    LOG("GO: sent to player %d (%s) - waiting for ACK", playerIdx, macStr);
  } else {
    LOG("GO: FAILED to send to player %d (%s) - route send failed", playerIdx, macStr);
  }
  return success;
}

void broadcastStart() {
  int sentCount = 0;
  
  // LOG all players and their route status before sending
  LOG("=== STARTING ROUND - Player Route Status ===");
  for (int i = 0; i < activePlayerCount; i++) {
    char macStr[18];
    macToStr(players[i].mac, macStr);
    int routeIdx = findRoute(routeTable, players[i].mac);
    if (routeIdx >= 0) {
      LOG("  Player %d (%s) has route | hop_count=%d | valid=true", 
          i, macStr, routeTable[routeIdx].hop_count);
    } else {
      LOG("  Player %d (%s) NO ROUTE | skip GO", i, macStr);
    }
  }
  LOG("==========================================");
  
  for (int i = 0; i < activePlayerCount; i++) {
    if (sendGoToPlayer(players[i].mac, i)) {
      sentCount++;
    }
    // DELAY: Give ESP-NOW time to buffer/send each packet before sending next one
    // This prevents buffer overflow when sending to 3+ peers in rapid succession
    if (i < activePlayerCount - 1) {
      delay(50);  // 50ms delay between sending to each player
    }
  }

  if (sentCount == 0) {
    LOG("GO: aborting round start because no players have a route");
    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setCursor(10, 20);
    M5.Lcd.setTextSize(2);
    M5.Lcd.println("No routes!");
    M5.Lcd.println("Press B to");
    M5.Lcd.println("discover");
    delay(2000);
    resetRound();
    return;
  }

  roundActive = true;
  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setCursor(10, 20);
  M5.Lcd.setTextSize(3);
  M5.Lcd.println("GO!");
  LOG("Round is live. GO sent to %d/%d players | waiting for PRESS packets",
      sentCount, activePlayerCount);
}

void sendResultToPlayers() {
  // Find fastest reaction time among players who pressed
  unsigned long fastestTime = ULONG_MAX;
  for (int i = 0; i < activePlayerCount; i++) {
    if (playerPresses[i].received && playerPresses[i].reactionMs < fastestTime) {
      fastestTime = playerPresses[i].reactionMs;
    }
  }

  // Count how many players tied at the fastest time
  int tiedCount = 0;
  int tiedPlayers[MAX_PLAYERS];
  for (int i = 0; i < activePlayerCount; i++) {
    if (playerPresses[i].received && playerPresses[i].reactionMs == fastestTime) {
      tiedPlayers[tiedCount++] = i;
    }
  }

  // Send RESULT packet to each player with their status
  for (int i = 0; i < activePlayerCount; i++) {
    GamePacket resultPkt;
    uint8_t resultCode;
    uint8_t tiePartnerId = 0;

    if (!playerPresses[i].received) {
      // Player didn't press - they lose
      resultCode = RESULT_LOSE;
    } else if (tiedCount > 1) {
      // Multiple winners (tie)
      if (playerPresses[i].reactionMs == fastestTime) {
        resultCode = RESULT_TIE;
        // Store first other tied player ID for display
        for (int j = 0; j < tiedCount; j++) {
          if (tiedPlayers[j] != i) {
            tiePartnerId = tiedPlayers[j];
            break;
          }
        }
      } else {
        resultCode = RESULT_LOSE;
      }
    } else {
      // Single winner
      resultCode = (playerPresses[i].reactionMs == fastestTime) ? RESULT_WIN : RESULT_LOSE;
    }

    // Encode player ID and result code in reaction_ms field
    uint32_t encodedResult = encodeResult(i, resultCode, tiePartnerId);
    
    initPacket(resultPkt, PACKET_RESULT, myMac, players[i].mac, myMac, 
               nextPacketId(packetCounter), encodedResult, DEFAULT_TTL);
    
    char label[32];
    snprintf(label, sizeof(label), "RESULT to player %d", i);
    if (!sendViaRoute(routeTable, players[i].mac, resultPkt, label)) {
      LOG("RESULT: failed to route to player %d", i);
    }
  }
}

void declareWinner() {
  roundActive = false;
  winnerPending = false;

  // Find fastest reaction time
  unsigned long fastest = ULONG_MAX;
  for (int i = 0; i < activePlayerCount; i++) {
    if (playerPresses[i].received && playerPresses[i].reactionMs < fastest) {
      fastest = playerPresses[i].reactionMs;
    }
  }

  // Find all winners
  int winnerCount = 0;
  int winners[MAX_PLAYERS];
  for (int i = 0; i < activePlayerCount; i++) {
    if (playerPresses[i].received && playerPresses[i].reactionMs == fastest) {
      winners[winnerCount++] = i;
    }
  }

  LOG("=== ROUND RESULT ===");
  for (int i = 0; i < activePlayerCount; i++) {
    char macStr[18];
    macToStr(players[i].mac, macStr);
    if (playerPresses[i].received) {
      LOG("  Player %d (%s) | reactionMs=%lu | hop=%d | diff=+%lu ms",
          i, macStr, playerPresses[i].reactionMs, playerPresses[i].hopCount,
          playerPresses[i].reactionMs - fastest);
    } else {
      LOG("  Player %d (%s) | did not press", i, macStr);
    }
  }
  
  if (winnerCount > 0) {
    if (winnerCount == 1) {
      LOG("  >> WINNER: Player %d <<", winners[0]);
    } else {
      LOG("  >> TIE: Players", "");
      for (int i = 0; i < winnerCount; i++) {
        LOG("     Player %d", winners[i]);
      }
    }
  } else {
    LOG("  >> NO WINNER (no presses received) <<", "");
  }
  LOG("====================");

  // Display results on server
  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setCursor(10, 10);
  M5.Lcd.setTextSize(2);
  if (winnerCount == 1) {
    M5.Lcd.printf("Winner: P%d!\n\n", winners[0]);
  } else if (winnerCount > 1) {
    M5.Lcd.printf("TIE!\n");
    M5.Lcd.setTextSize(1);
    for (int i = 0; i < winnerCount; i++) {
      M5.Lcd.printf("P%d ", winners[i]);
    }
    M5.Lcd.println();
  } else {
    M5.Lcd.println("No winner\n");
  }
  
  M5.Lcd.setTextSize(1);
  for (int i = 0; i < activePlayerCount; i++) {
    if (playerPresses[i].received) {
      M5.Lcd.printf("P%d: %lums h:%d\n", i, playerPresses[i].reactionMs, playerPresses[i].hopCount);
    } else {
      M5.Lcd.printf("P%d: no press\n", i);
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

  // NEW: Version and signature check
  if (pkt.version != PROTOCOL_VERSION) {
    LOG("DROP: incompatible protocol version %d (want %d)", pkt.version, PROTOCOL_VERSION);
    return;
  }

  if (!verifyPacketHash(pkt)) {
    LOG("DROP: authentication failure (invalid signature)");
    return;
  }

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

    // Auto-register player if not already registered
    registerPlayer(pkt.origin_mac);
    
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
      delay(random(20, 81));
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

  if (pkt.type == PACKET_RREP) {
    bool already_seen = isSeen(seenTable, pkt.origin_mac, pkt.packet_id);
    if (already_seen) {
      LOG("DROP: duplicate (origin=%s id=%u)", originStr, pkt.packet_id);
      return;
    }

    int idx = findRoute(routeTable, pkt.origin_mac);
    if (idx >= 0 && pkt.hop_count + 1 >= routeTable[idx].hop_count) {
      LOG("RREP: not better, drop");
      return;
    }

    addRoute(routeTable, pkt.origin_mac, recvInfo->src_addr, pkt.hop_count + 1);
    markSeen(seenTable, pkt.origin_mac, pkt.packet_id);

    LOG("RREP: learned route to %s via %s hops=%d",
        originStr, srcStr, pkt.hop_count + 1);

    if (!isLocalMac(pkt.dest_mac, myMac)) {
      if (sendViaRoute(routeTable, pkt.dest_mac, pkt, "RREP forward", myMac)) {
        LOG("RREP: forwarded toward requester");
      } else {
        LOG("RREP: forward failed");
      }
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

  int playerIdx = findPlayerIndex(pkt.origin_mac);
  if (playerIdx < 0) {
    LOG("DROP: unknown player MAC %s", originStr);
    return;
  }

  if (playerPresses[playerIdx].received) {
    LOG("DUP PRESS from player %d | re-sending ACK", playerIdx);

    GamePacket ackPkt;
    initPacket(ackPkt, PACKET_ACK, myMac, pkt.origin_mac, myMac,
               pkt.packet_id, 0, DEFAULT_TTL);
    if (!sendViaRoute(routeTable, pkt.origin_mac, ackPkt, "PRESS ACK duplicate")) {
      LOG("ACK: failed to route back to player %d", playerIdx);
    }
    return;
  }

  playerPresses[playerIdx] = {pkt.reaction_ms, pkt.hop_count, true};
  LOG("ACCEPTED PRESS from player %d | reaction_ms=%lu hop=%d",
      playerIdx, (unsigned long)pkt.reaction_ms, pkt.hop_count);

  GamePacket ackPkt;
  initPacket(ackPkt, PACKET_ACK, myMac, pkt.origin_mac, myMac,
             pkt.packet_id, 0, DEFAULT_TTL);
  if (!sendViaRoute(routeTable, pkt.origin_mac, ackPkt, "PRESS ACK")) {
    LOG("ACK: failed to route back to player %d", playerIdx);
  }

  bool allReceived = true;
  for (int i = 0; i < activePlayerCount; i++) {
    if (!playerPresses[i].received) {
      allReceived = false;
      break;
    }
  }

  LOG("Press state: %d/%d players received", countValidRoutes(), activePlayerCount);

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

  esp_now_register_send_cb([](const uint8_t *mac_addr, esp_now_send_status_t status) {
    char macStr[18];
    macToStr(mac_addr, macStr);
    LOG("SEND to %s: %s", macStr,
        status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL (no ack — wrong MAC or out of range?)");
  });

  esp_now_register_recv_cb(onDataReceived);

  registerPeer(broadcastMac);
  resetSeenTable(seenTable);
  resetRouteTable(routeTable);

  LOG("Server ready - waiting for player discovery (up to %d players)", MAX_PLAYERS);
  resetRound();
}

void loop() {
  M5.update();
  
  if (winnerPending) {
    declareWinner();
    return;
  }

  // Button B: Manual broadcast to discover players
  if (M5.BtnB.wasPressed()) {
    LOG("Button B pressed | broadcasting discovery beacon");
    GamePacket beacon;
    initPacket(beacon, PACKET_RREQ, myMac, broadcastMac, myMac,
               nextPacketId(packetCounter), 0, DEFAULT_TTL);
    if (sendPacket(broadcastMac, beacon, "DISCOVERY BEACON")) {
      LOG("Beacon sent; players should respond with routes within 3 seconds");
    }
    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setCursor(10, 20);
    M5.Lcd.setTextSize(2);
    M5.Lcd.println("Broadcasting\nfor routes...");
    delay(2000);
    resetRound();
  }

  // Button A: Start game or end early
  if (M5.BtnA.wasPressed()) {
    if (!roundActive) {
      LOG("Button A pressed | starting round");
      
      int validRoutes = countValidRoutes();
      LOG("Valid routes: %d/%d", validRoutes, activePlayerCount);
      
      if (activePlayerCount == 0 || validRoutes == 0) {
        LOG("ERROR: No players discovered or no routes! Press B first to discover players.");
        M5.Lcd.fillScreen(BLACK);
        M5.Lcd.setCursor(10, 20);
        M5.Lcd.setTextSize(2);
        M5.Lcd.printf("Players: %d\n", activePlayerCount);
        M5.Lcd.setTextSize(1);
        if (activePlayerCount == 0) {
          M5.Lcd.println("Press B to\ndiscover");
        } else {
          M5.Lcd.println("No routes!\nPress B");
        }
        delay(2000);
        resetRound();
        return;
      }
      broadcastStart();
    } else {
      LOG("Button A pressed | ending round early");
      declareWinner();
    }
  }
}
