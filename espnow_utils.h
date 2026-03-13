#pragma once

#include <esp_now.h>

#include "game_protocol.h"

inline void registerPeer(uint8_t* mac) {
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

inline void registerPeerIfNeeded(const uint8_t* mac) {
  if (!esp_now_is_peer_exist(mac)) {
    registerPeer((uint8_t*)mac);
  }
}

inline void sendPacket(const uint8_t* mac, GamePacket &pkt, const char* label) {
  esp_err_t err = esp_now_send(mac, (uint8_t*)&pkt, sizeof(pkt));
  if (err != ESP_OK) {
    LOG("ERROR: %s failed immediately (err=%d)", label, err);
  }
}
