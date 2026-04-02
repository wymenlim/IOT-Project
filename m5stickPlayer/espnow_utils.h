#pragma once

#include <esp_now.h>

#include "game_protocol.h"
#include "auth_utils.h"

inline void registerPeer(uint8_t* mac) {
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = ESPNOW_CHANNEL;

  bool isBroadcast = (mac[0] == 0xFF && mac[1] == 0xFF && mac[2] == 0xFF &&
                      mac[3] == 0xFF && mac[4] == 0xFF && mac[5] == 0xFF);
  if (isBroadcast) {
    peer.encrypt = false;
  } else {
    peer.encrypt = true;
    memcpy(peer.lmk, ESPNOW_LMK, 16);
  }

  char macStr[18];
  macToStr(mac, macStr);

  esp_err_t res = esp_now_add_peer(&peer);
  if (res == ESP_OK) {
    LOG("Registered peer %s on ch%d encrypt=%s", macStr, ESPNOW_CHANNEL, isBroadcast ? "off" : "on");
  } else {
    LOG("ERROR: Failed to register peer %s (err=%d)", macStr, res);
  }
}

inline void registerPeerIfNeeded(const uint8_t* mac) {
  if (!esp_now_is_peer_exist(mac)) {
    registerPeer((uint8_t*)mac);
  }
}

inline bool sendPacket(const uint8_t* mac, GamePacket &pkt, const char* label) {
  signPacket(pkt);
  
  esp_err_t err = esp_now_send(mac, (uint8_t*)&pkt, sizeof(pkt));
  if (err != ESP_OK) {
    LOG("ERROR: %s failed immediately (err=%d)", label, err);
    return false;
  }
  return true;
}

inline bool sendViaRoute(RouteEntry table[MAX_ROUTE_ENTRIES],
                         const uint8_t destMac[6],
                         GamePacket &pkt,
                         const char *label,
                         const uint8_t *relayMac = nullptr) {
  expireRoutes(table);
  int idx = findRoute(table, destMac);
  char destStr[18];
  macToStr(destMac, destStr);

  if (idx < 0) {
    LOG("%s: DROP no route to %s", label, destStr);
    return false;
  }

  if (relayMac != nullptr) {
    if (pkt.ttl <= 1) {
      LOG("%s: DROP TTL exhausted for %s", label, destStr);
      return false;
    }
    setRelayFields(pkt, relayMac);
  }

  char nextHopStr[18];
  macToStr(table[idx].next_hop_mac, nextHopStr);
  registerPeerIfNeeded(table[idx].next_hop_mac);
  if (!sendPacket(table[idx].next_hop_mac, pkt, label)) {
    return false;
  }
  LOG("%s: via %s toward %s (ttl=%d hop=%d route_hops=%d)",
      label, nextHopStr, destStr, pkt.ttl, pkt.hop_count, table[idx].hop_count);
  return true;
}
