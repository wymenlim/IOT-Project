#pragma once

#include <Arduino.h>
#include <mbedtls/md.h>
#include "game_protocol.h"

// Shared secret (compile-time constant for all nodes)
#define SHARED_SECRET "GameSecret2026"
#define SECRET_LEN 14

// ESP-NOW hardware encryption keys (must match on all nodes)
// PMK: set once per node via esp_now_set_pmk()
// LMK: set per unicast peer in registerPeer()
static const uint8_t ESPNOW_PMK[16] = {
  0x47, 0x61, 0x6D, 0x65, 0x50, 0x4D, 0x4B, 0x32,
  0x30, 0x32, 0x36, 0x58, 0x59, 0x5A, 0x21, 0x40
};
static const uint8_t ESPNOW_LMK[16] = {
  0x47, 0x61, 0x6D, 0x65, 0x4C, 0x4D, 0x4B, 0x32,
  0x30, 0x32, 0x36, 0x41, 0x42, 0x43, 0x23, 0x24
};

// Fields that remain constant across relay hops.
// sender_mac, hop_count, and ttl are excluded — they change at each hop.
struct AuthPayload {
  uint8_t  type;
  uint8_t  version;
  uint8_t  origin_mac[6];
  uint8_t  dest_mac[6];
  uint16_t packet_id;
  uint32_t reaction_ms;
} __attribute__((packed));

inline void computePacketHash(const GamePacket &pkt, uint8_t hash[AUTH_HASH_LEN]) {
  AuthPayload payload;
  payload.type        = pkt.type;
  payload.version     = pkt.version;
  memcpy(payload.origin_mac, pkt.origin_mac, 6);
  memcpy(payload.dest_mac,   pkt.dest_mac,   6);
  payload.packet_id   = pkt.packet_id;
  payload.reaction_ms = pkt.reaction_ms;

  uint8_t hmac[32];
  mbedtls_md_hmac(
    mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
    (const uint8_t *)SHARED_SECRET, SECRET_LEN,
    (const uint8_t *)&payload, sizeof(payload),
    hmac
  );
  memcpy(hash, hmac, AUTH_HASH_LEN);
}

inline bool verifyPacketHash(const GamePacket &pkt) {
  uint8_t expected[AUTH_HASH_LEN];
  computePacketHash(pkt, expected);
  return memcmp(pkt.auth_hash, expected, AUTH_HASH_LEN) == 0;
}

inline void signPacket(GamePacket &pkt) {
  pkt.version = PROTOCOL_VERSION;
  computePacketHash(pkt, pkt.auth_hash);
}
