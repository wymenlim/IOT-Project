#ifndef GAME_PROTOCOL_H
#define GAME_PROTOCOL_H

#include <Arduino.h>
#include <esp_wifi.h>
#include <stdint.h>
#include <string.h>

// Timestamped log macro — every log line starts with [millis]
#define LOG(fmt, ...) Serial.printf("[%6lu] " fmt "\n", millis(), ##__VA_ARGS__)

// Format a MAC address into a caller-supplied 18-byte buffer
inline void macToStr(const uint8_t mac[6], char out[18]) {
  snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

enum PacketType : uint8_t {
  PACKET_RREQ = 1,
  PACKET_RREP = 2,
  PACKET_GO = 3,
  PACKET_PRESS = 4,
  PACKET_RERR = 5,
  PACKET_ACK = 6,
  PACKET_RESULT = 7,
};

#define DEDUP_CACHE_SIZE 8
#define ESPNOW_CHANNEL 1
struct GamePacket {
  uint8_t type;
  uint8_t origin_mac[6];
  uint8_t dest_mac[6];
  uint8_t sender_mac[6];
  uint16_t packet_id;
  uint8_t hop_count;
  uint32_t reaction_ms;
} __attribute__((packed));

static_assert(sizeof(GamePacket) == 26, "GamePacket size mismatch");

// reaction_ms is elapsed time from GO reception to button press, not wall-clock time.
// sender_mac is kept in-band so relay logic and logs do not depend on callback-only metadata.
// packed is required to keep the wire format stable across nodes and avoid padding drift.

struct DedupEntry {
  uint8_t origin_mac[6];
  uint16_t packet_id;
  bool valid;
};

inline void copyMac(uint8_t dest[6], const uint8_t src[6]) {
  memcpy(dest, src, 6);
}

inline bool macEquals(const uint8_t a[6], const uint8_t b[6]) {
  return memcmp(a, b, 6) == 0;
}

inline bool isLocalMac(const uint8_t candidate[6], const uint8_t local[6]) {
  return macEquals(candidate, local);
}

inline void initPacket(GamePacket &pkt,
                       uint8_t type,
                       const uint8_t origin_mac[6],
                       const uint8_t dest_mac[6],
                       const uint8_t sender_mac[6],
                       uint16_t packet_id,
                       uint32_t reaction_ms) {
  pkt.type = type;
  copyMac(pkt.origin_mac, origin_mac);
  copyMac(pkt.dest_mac, dest_mac);
  copyMac(pkt.sender_mac, sender_mac);
  pkt.packet_id = packet_id;
  pkt.hop_count = 0;
  pkt.reaction_ms = reaction_ms;
}

inline void setRelayFields(GamePacket &pkt, const uint8_t sender_mac[6]) {
  copyMac(pkt.sender_mac, sender_mac);
  pkt.hop_count++;
}

inline uint16_t nextPacketId(uint16_t &counter) {
  counter++;
  if (counter == 0) {
    counter = 1;
  }
  return counter;
}

inline void resetDedupCache(DedupEntry cache[DEDUP_CACHE_SIZE]) {
  for (int i = 0; i < DEDUP_CACHE_SIZE; ++i) {
    cache[i].valid = false;
    cache[i].packet_id = 0;
  }
}

inline bool isDuplicateAndRemember(DedupEntry cache[DEDUP_CACHE_SIZE],
                                   uint8_t &next_index,
                                   const uint8_t origin_mac[6],
                                   uint16_t packet_id) {
  for (int i = 0; i < DEDUP_CACHE_SIZE; ++i) {
    if (cache[i].valid &&
        cache[i].packet_id == packet_id &&
        macEquals(cache[i].origin_mac, origin_mac)) {
      return true;
    }
  }

  DedupEntry &slot = cache[next_index];
  slot.valid = true;
  slot.packet_id = packet_id;
  copyMac(slot.origin_mac, origin_mac);
  next_index = (next_index + 1) % DEDUP_CACHE_SIZE;
  return false;
}

inline bool configureEspNowChannel() {
  if (esp_wifi_set_promiscuous(true) != ESP_OK) {
    return false;
  }

  if (esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
    esp_wifi_set_promiscuous(false);
    return false;
  }

  return esp_wifi_set_promiscuous(false) == ESP_OK;
}

#endif
