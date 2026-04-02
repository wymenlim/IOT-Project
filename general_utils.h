#pragma once

#include "espnow_utils.h"

#define RREQ_JITTER_MIN_MS 20
#define RREQ_JITTER_MAX_MS 80
#define ROUTE_REDISCOVERY_MS 3000
#define PRESS_ACK_TIMEOUT_MS 5000

enum ButtonUiEvent : uint8_t {
  BUTTON_UI_NONE = 0,
  BUTTON_UI_GO,
  BUTTON_UI_RESULT,
  BUTTON_UI_DELIVERED,
};

inline void sendRouteRequest(const uint8_t* myMac,
                             const uint8_t* serverMac,
                             const uint8_t* broadcastMac,
                             uint16_t &packetCounter,
                             const char *label) {
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

  LOG("%s: Sending RREQ (id=%u ttl=%d)", label, rreq.packet_id, rreq.ttl);
  delay(random(RREQ_JITTER_MIN_MS, RREQ_JITTER_MAX_MS + 1));
  sendPacket(broadcastMac, rreq, label);
}

inline void handleButtonNodeReceive(const esp_now_recv_info *recvInfo,
                                    const uint8_t *data,
                                    int len,
                                    const uint8_t *myMac,
                                    const uint8_t *broadcastMac,
                                    uint16_t &packetCounter,
                                    SeenEntry seenTable[MAX_SEEN_ENTRIES],
                                    RouteEntry routeTable[MAX_ROUTE_ENTRIES],
                                    bool &gameStarted,
                                    bool &pendingPressValid,
                                    bool &awaitingAck,
                                    unsigned long &ackDeadline,
                                    ButtonUiEvent &uiEvent,
                                    unsigned long &deliveredReactionMs,
                                    GamePacket &pendingPress,
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

    bool already_seen = isSeen(seenTable, pkt.origin_mac, pkt.type, pkt.packet_id);
    addRoute(routeTable, pkt.origin_mac, recvInfo->src_addr, pkt.hop_count + 1);
    if (already_seen) {
      LOG("DROP: duplicate (origin=%s id=%u)", originStr, pkt.packet_id);
      return;
    }
    markSeen(seenTable, pkt.origin_mac, pkt.type, pkt.packet_id);

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

  else if (pkt.type == PACKET_RREP) {

    bool already_seen = isSeen(seenTable, pkt.origin_mac, pkt.type, pkt.packet_id);
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
    markSeen(seenTable,pkt.origin_mac,pkt.type,pkt.packet_id);

    LOG("RREP: learned route to %s via %s hops=%d",
        originStr, srcStr, pkt.hop_count + 1);

    if (isLocalMac(pkt.dest_mac, myMac)) {
      LOG("RREP: route discovery complete");
      if (pendingPressValid && !awaitingAck) {
        LOG("PRESS retry pending | id=%u reaction_ms=%lu",
            pendingPress.packet_id, (unsigned long)pendingPress.reaction_ms);
      }
      return;
    }

    if (sendViaRoute(routeTable, pkt.dest_mac, pkt, "RREP forward", myMac)) {
      LOG("RREP: forwarded toward requester");
    } else {
      LOG("RREP: forward failed (no route, TTL exhausted, or send error)");
    }
    return;
  }

  if (pkt.type == PACKET_GO) {
    if (seenCheck(seenTable, pkt.origin_mac, pkt.type, pkt.packet_id)) {
      LOG("GO: DROP duplicate (origin=%s id=%u)", originStr, pkt.packet_id);
      return;
    }

    if (!isLocalMac(pkt.dest_mac,myMac)) {
      sendViaRoute(routeTable, pkt.dest_mac, pkt, "GO forward", myMac);
      return;
    }

    startTime = millis();
    gameStarted = true;
    pendingPressValid = false;
    awaitingAck = false;
    ackDeadline = 0;
    uiEvent = BUTTON_UI_GO;
    lastButtonState = false;
    lastDebounceTime = 0;
    LOG("GO accepted | timer started at %lu ms", startTime);
    return;
  }

  if (pkt.type == PACKET_PRESS && !isLocalMac(pkt.dest_mac, myMac)) {
    if (seenCheck(seenTable, pkt.origin_mac, pkt.type, pkt.packet_id)) {
      LOG("PRESS: DROP duplicate (origin=%s id=%u)", originStr, pkt.packet_id);
      return;
    }

    sendViaRoute(routeTable, pkt.dest_mac, pkt, "PRESS forward", myMac);
    return;
  }

  if (pkt.type == PACKET_ACK && !isLocalMac(pkt.dest_mac, myMac)) {
    if (seenCheck(seenTable, pkt.origin_mac, pkt.type, pkt.packet_id)) {
      LOG("ACK: DROP duplicate (origin=%s id=%u)", originStr, pkt.packet_id);
      return;
    }

    sendViaRoute(routeTable, pkt.dest_mac, pkt, "ACK forward", myMac);
    return;
  }

  if (pkt.type == PACKET_RESULT && !isLocalMac(pkt.dest_mac, myMac)) {
    if (seenCheck(seenTable, pkt.origin_mac, pkt.type, pkt.packet_id)) {
      LOG("RESULT: DROP duplicate (origin=%s id=%u)", originStr, pkt.packet_id);
      return;
    }

    sendViaRoute(routeTable, pkt.dest_mac, pkt, "RESULT forward", myMac);
    return;
  }

  if (!isLocalMac(pkt.dest_mac, myMac)) {
    LOG("DROP: not for me (dest=%s)", destStr);
    return;
  }

  if (pkt.type == PACKET_ACK) {
    if (!awaitingAck || !pendingPressValid) {
      LOG("ACK: no pending press; ignoring");
      return;
    }

    if (!macEquals(pendingPress.origin_mac, pkt.dest_mac) ||
        pendingPress.packet_id != pkt.packet_id) {
      LOG("ACK: pending press mismatch (ack id=%u pending id=%u)",
          pkt.packet_id, pendingPress.packet_id);
      return;
    }

    pendingPressValid = false;
    awaitingAck = false;
    ackDeadline = 0;
    gameStarted = false;
    deliveredReactionMs = pendingPress.reaction_ms;
    uiEvent = BUTTON_UI_DELIVERED;
    lastButtonState = false;
    LOG("ACK received | press delivery confirmed (id=%u)", pkt.packet_id);
  } else if (pkt.type == PACKET_RESULT) {
    gameStarted = false;
    pendingPressValid = false;
    awaitingAck = false;
    ackDeadline = 0;
    uiEvent = BUTTON_UI_RESULT;
    lastButtonState = false;
    LOG("RESULT received | round complete, resetting");
  } else {
    LOG("DROP: unhandled type=%d", pkt.type);
  }
}

inline void handleButtonNodeLoop(const uint8_t *myMac,
                                 const uint8_t *serverMac,
                                 const uint8_t *broadcastMac,
                                 uint16_t &packetCounter,
                                 RouteEntry routeTable[MAX_ROUTE_ENTRIES],
                                 bool &gameStarted,
                                 bool &pendingPressValid,
                                 bool &awaitingAck,
                                 unsigned long &ackDeadline,
                                 ButtonUiEvent &uiEvent,
                                 unsigned long &deliveredReactionMs,
                                 GamePacket &pendingPress,
                                 bool &lastButtonState,
                                 unsigned long &lastDebounceTime,
                                 unsigned long &lastRouteRequestTime,
                                 unsigned long debounceDelay,
                                 unsigned long startTime) {
  M5.update();
  if (M5.BtnB.wasPressed()) {
    invalidateRoute(routeTable, serverMac);
    lastRouteRequestTime = millis();
    LOG("MANUAL RREQ: button pressed, restarting route discovery");
    sendRouteRequest(myMac, serverMac, broadcastMac, packetCounter, "MANUAL RREQ");
    if (!gameStarted && !pendingPressValid && !awaitingAck) {
      M5.Lcd.fillScreen(BLACK);
      M5.Lcd.setCursor(10, 20);
      M5.Lcd.setTextSize(2);
      M5.Lcd.println("Rediscovering");
      M5.Lcd.setTextSize(1);
      M5.Lcd.println("Waiting for route...");
    }
  }

  if (uiEvent != BUTTON_UI_NONE) {
    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setCursor(10, 20);
    if (uiEvent == BUTTON_UI_GO) {
      M5.Lcd.setTextSize(3);
      M5.Lcd.println("GO!");
    } else if (uiEvent == BUTTON_UI_RESULT) {
      M5.Lcd.setTextSize(2);
      M5.Lcd.println("Round done");
      M5.Lcd.setTextSize(1);
      M5.Lcd.println("Waiting for GO...");
    } else if (uiEvent == BUTTON_UI_DELIVERED) {
      M5.Lcd.setCursor(10, 30);
      M5.Lcd.setTextSize(2);
      M5.Lcd.printf("Delivered!\n%lu ms", deliveredReactionMs);
    }
    uiEvent = BUTTON_UI_NONE;
  }

  if (!gameStarted) {
    return;
  }

  if (awaitingAck) {
    if ((long)(millis() - ackDeadline) >= 0) {
      awaitingAck = false;
      ackDeadline = 0;
      invalidateRoute(routeTable, serverMac);
      lastRouteRequestTime = millis();
      LOG("ACK timeout | restarting route discovery for pending PRESS");
      sendRouteRequest(myMac, serverMac, broadcastMac, packetCounter, "PRESS RREQ");
      M5.Lcd.fillScreen(BLACK);
      M5.Lcd.setCursor(10, 30);
      M5.Lcd.setTextSize(2);
      M5.Lcd.println("ACK timeout");
      M5.Lcd.println("Routing...");
    }
    return;
  }

  if (pendingPressValid) {
    if (sendViaRoute(routeTable, pendingPress.dest_mac, pendingPress, "PRESS retry")) {
      awaitingAck = true;
      ackDeadline = millis() + PRESS_ACK_TIMEOUT_MS;
      M5.Lcd.fillScreen(BLACK);
      M5.Lcd.setCursor(10, 30);
      M5.Lcd.setTextSize(2);
      M5.Lcd.println("Sent!");
      M5.Lcd.println("Wait ACK");
      return;
    }

    if (millis() - lastRouteRequestTime >= ROUTE_REDISCOVERY_MS) {
      lastRouteRequestTime = millis();
      sendRouteRequest(myMac, serverMac, broadcastMac, packetCounter, "PRESS RREQ");
    }
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

    LOG("PRESS sending to server | reaction_ms=%lu hop=%d id=%u",
        (unsigned long)pkt.reaction_ms, pkt.hop_count, pkt.packet_id);
    if (!sendViaRoute(routeTable, serverMac, pkt, "PRESS to server")) {
      pendingPress = pkt;
      pendingPressValid = true;
      awaitingAck = false;
      ackDeadline = 0;
      lastButtonState = false;
      lastRouteRequestTime = millis();
      LOG("PRESS: route unavailable; queueing packet and restarting discovery");
      sendRouteRequest(myMac, serverMac, broadcastMac, packetCounter, "PRESS RREQ");
      M5.Lcd.fillScreen(BLACK);
      M5.Lcd.setCursor(10, 30);
      M5.Lcd.setTextSize(2);
      M5.Lcd.println("Routing...");
      return;
    }

    pendingPress = pkt;
    pendingPressValid = true;
    awaitingAck = true;
    ackDeadline = millis() + PRESS_ACK_TIMEOUT_MS;
    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setCursor(10, 30);
    M5.Lcd.setTextSize(2);
    M5.Lcd.println("Sent!");
    M5.Lcd.println("Wait ACK");
  }
  lastButtonState = currentPress;
}

inline void sendInitialRREQ(const uint8_t* myMac,
                            const uint8_t* serverMac,
                            const uint8_t* broadcastMac,
                            uint16_t &packetCounter) {
  sendRouteRequest(myMac, serverMac, broadcastMac, packetCounter, "BOOT RREQ");
}
