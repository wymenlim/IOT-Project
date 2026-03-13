#pragma once

#include "espnow_utils.h"

inline void handleButtonNodeReceive(const esp_now_recv_info *recvInfo,
                                    const uint8_t *data,
                                    int len,
                                    const uint8_t *myMac,
                                    const uint8_t *broadcastMac,
                                    uint16_t &packetCounter,
                                    SeenEntry seenTable[MAX_SEEN_ENTRIES],
                                    RouteEntry routeTable[MAX_ROUTE_ENTRIES],
                                    bool &gameStarted,
                                    bool &lastButtonState,
                                    unsigned long &lastDebounceTime,
                                    unsigned long &startTime) {
  char srcStr[18];
  macToStr(recvInfo->src_addr, srcStr);
  LOG("RECV from %s | len=%d", srcStr, len);

  if (len != sizeof(GamePacket)) {
    LOG("DROP: wrong len (got %d, want %d)", len, (int)sizeof(GamePacket));
    return;
  }

  GamePacket pkt;
  memcpy(&pkt, data, sizeof(pkt));

  char originStr[18];
  macToStr(pkt.origin_mac, originStr);
  char destStr[18];
  macToStr(pkt.dest_mac, destStr);
  LOG("PKT type=%d origin=%s dest=%s hop=%d id=%u",
      pkt.type, originStr, destStr, pkt.hop_count, pkt.packet_id);

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
      sendPacket(recvInfo->src_addr, rrep, "RREP");
      LOG("RREQ: I am dest, sent RREP to %s", srcStr);
    } else if (pkt.ttl > 1) {
      setRelayFields(pkt, myMac);
      sendPacket(broadcastMac, pkt, "RREQ relay");
      LOG("RREQ: relayed (ttl=%d hop=%d)", pkt.ttl, pkt.hop_count);
    } else {
      LOG("RREQ: DROP ttl exhausted");
    }
    return;
  }

  if (!isLocalMac(pkt.dest_mac, myMac)) {
    LOG("DROP: not for me (dest=%s)", destStr);
    return;
  }

  if (pkt.type == PACKET_GO) {
    startTime = millis();
    gameStarted = true;
    lastButtonState = false;
    lastDebounceTime = 0;
    LOG("GO accepted | timer started at %lu ms", startTime);
    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setCursor(10, 30);
    M5.Lcd.setTextSize(3);
    M5.Lcd.println("GO!");
  } else if (pkt.type == PACKET_RESULT) {
    gameStarted = false;
    lastButtonState = false;
    LOG("RESULT received | round complete, resetting");
    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setCursor(10, 20);
    M5.Lcd.setTextSize(2);
    M5.Lcd.println("Round done");
    M5.Lcd.setTextSize(1);
    M5.Lcd.println("Waiting for GO...");
  } else {
    LOG("DROP: unhandled type=%d", pkt.type);
  }
}

inline void handleButtonNodeLoop(const uint8_t *myMac,
                                 const uint8_t *serverMac,
                                 uint16_t &packetCounter,
                                 bool &gameStarted,
                                 bool &lastButtonState,
                                 unsigned long &lastDebounceTime,
                                 unsigned long debounceDelay,
                                 unsigned long startTime) {
  M5.update();
  if (!gameStarted) {
    return;
  }

  bool currentPress = M5.BtnA.isPressed();
  if (currentPress && !lastButtonState &&
      (millis() - lastDebounceTime > debounceDelay)) {
    lastDebounceTime = millis();

    GamePacket pkt;
    initPacket(
      pkt,
      PACKET_PRESS,
      myMac,
      serverMac,
      myMac,
      nextPacketId(packetCounter),
      millis() - startTime,
      DEFAULT_TTL
    );

    LOG("PRESS sending to D | reaction_ms=%lu hop=%d id=%u",
        (unsigned long)pkt.reaction_ms, pkt.hop_count, pkt.packet_id);
    sendPacket(serverMac, pkt, "PRESS to D");

    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setCursor(10, 30);
    M5.Lcd.setTextSize(2);
    M5.Lcd.printf("Sent!\n%lu ms", pkt.reaction_ms);

    gameStarted = false;
  }
  lastButtonState = currentPress;
}

inline void sendInitialRREQ(const uint8_t* myMac,
                            const uint8_t* serverMac,
                            const uint8_t* broadcastMac,
                            uint16_t &packetCounter) {
  GamePacket rreq;

  initPacket(
    rreq,
    PACKET_RREQ,
    myMac,
    serverMac,
    myMac,
    nextPacketId(packetCounter),
    0,
    DEFAULT_TTL
  );

  LOG("BOOT: Sending initial RREQ (id=%u ttl=%d)",
      rreq.packet_id, rreq.ttl);

  sendPacket(broadcastMac, rreq, "BOOT RREQ");
}
