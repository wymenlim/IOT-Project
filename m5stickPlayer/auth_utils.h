#pragma once

#include <Arduino.h>
#include "game_protocol.h"

// Shared secret (compile-time constant for all nodes)
#define SHARED_SECRET "GameSecret2026"
#define SECRET_LEN 14

// Simple HMAC-like checksum (lightweight for embedded)
inline void computePacketHash(const GamePacket &pkt, uint8_t hash[4]) {
  uint32_t value = 0;
  
  // Mix packet fields with secret
  for (int i = 0; i < SECRET_LEN; i++) {
    value = value * 31 + SHARED_SECRET[i];
  }
  
  value ^= (uint32_t)pkt.type;
  value ^= (uint32_t)pkt.origin_mac[0] << 24;
  value ^= (uint32_t)pkt.origin_mac[1] << 16;
  value ^= (uint32_t)pkt.packet_id;
  
  hash[0] = (value >> 24) & 0xFF;
  hash[1] = (value >> 16) & 0xFF;
  hash[2] = (value >> 8) & 0xFF;
  hash[3] = value & 0xFF;
}

inline bool verifyPacketHash(const GamePacket &pkt) {
  uint8_t expected[4];
  computePacketHash(pkt, expected);
  return memcmp(pkt.auth_hash, expected, 4) == 0;
}

inline void signPacket(GamePacket &pkt) {
  pkt.version = PROTOCOL_VERSION;
  computePacketHash(pkt, pkt.auth_hash);
}
