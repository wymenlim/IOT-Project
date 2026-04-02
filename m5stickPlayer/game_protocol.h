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

#define PROTOCOL_VERSION 0x02

enum PacketType : uint8_t {
  PACKET_RREQ = 1,
  PACKET_RREP = 2,
  PACKET_GO = 3,
  PACKET_PRESS = 4,
  PACKET_RERR = 5,
  PACKET_ACK = 6,
  PACKET_RESULT = 7,
  PACKET_AUTH_REQ = 8,
  PACKET_AUTH_RESP = 9,
};

// Result codes for RESULT packets (encoded in reaction_ms field)
enum ResultCode : uint8_t {
  RESULT_WIN = 0,
  RESULT_LOSE = 1,
  RESULT_TIE = 2,
};

// Encoding helpers for RESULT packets (reaction_ms field layout):
// Byte 0: player_id (0-255)
// Byte 1: result code (0=win, 1=lose, 2=tie) + padding
// Byte 2: tie_partner_id (0-255) for TIE results
// Byte 3: reserved
inline uint32_t encodeResult(uint8_t playerId, uint8_t resultCode, uint8_t tiePartnerId = 0) {
  return ((uint32_t)playerId) | (((uint32_t)resultCode) << 8) | (((uint32_t)tiePartnerId) << 16);
}

inline uint8_t decodeResultPlayerId(uint32_t encoded) {
  return (uint8_t)(encoded & 0xFF);
}

inline uint8_t decodeResultCode(uint32_t encoded) {
  return (uint8_t)((encoded >> 8) & 0xFF);
}

inline uint8_t decodeResultTiePartnerId(uint32_t encoded) {
  return (uint8_t)((encoded >> 16) & 0xFF);
}

#define ESPNOW_CHANNEL 1
#define DEFAULT_TTL 6
#define AUTH_HASH_LEN 8
struct GamePacket {
  uint8_t type;
  uint8_t version;
  uint8_t auth_hash[AUTH_HASH_LEN];
  uint8_t origin_mac[6];
  uint8_t dest_mac[6];
  uint8_t sender_mac[6];
  uint16_t packet_id;
  uint8_t hop_count;
  uint32_t reaction_ms;
  uint8_t ttl;
} __attribute__((packed));

static_assert(sizeof(GamePacket) == 36, "GamePacket size mismatch");
// reaction_ms is elapsed time from GO reception to button press, not wall-clock time.
// sender_mac is kept in-band so relay logic and logs do not depend on callback-only metadata.
// packed is required to keep the wire format stable across nodes and avoid padding drift.
// auth_hash provides basic authentication using a shared secret.
// version ensures protocol compatibility across nodes.

#define MAX_ROUTE_ENTRIES 10
#define ROUTE_EXPIRY_MS 120000
struct RouteEntry {
  bool valid;
  uint8_t dest_mac[6];
  uint8_t next_hop_mac[6]; //where to send packet to next
  uint8_t hop_count; //how many times packet have hopped (distance)
  unsigned long expiry_time; // Route lifetime
};

#define MAX_SEEN_ENTRIES 30
#define SEEN_EXPIRY_MS 5000
struct SeenEntry {
  uint8_t       origin_mac[6];
  uint16_t      packet_id;
  unsigned long expiry_time;
  bool          valid;
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
                       uint32_t reaction_ms,
                       uint8_t ttl) {
  pkt.type = type;
  pkt.version = PROTOCOL_VERSION;
  memset(pkt.auth_hash, 0, AUTH_HASH_LEN);
  copyMac(pkt.origin_mac, origin_mac);
  copyMac(pkt.dest_mac, dest_mac);
  copyMac(pkt.sender_mac, sender_mac);
  pkt.packet_id = packet_id;
  pkt.hop_count = 0;
  pkt.reaction_ms = reaction_ms;
  pkt.ttl = ttl;
}

inline void setRelayFields(GamePacket &pkt, const uint8_t sender_mac[6]) {
  copyMac(pkt.sender_mac, sender_mac);
  pkt.hop_count++;
  if (pkt.ttl >= 1) {
    pkt.ttl--;
  }
}

inline uint16_t nextPacketId(uint16_t &counter) {
  counter++;
  if (counter == 0) {
    counter = 1;
  }
  return counter;
}

inline void resetRouteTable(RouteEntry table[MAX_ROUTE_ENTRIES]) {
  for (int i = 0; i < MAX_ROUTE_ENTRIES; ++i) {
    table[i].valid = false;
    table[i].hop_count = 0;
    table[i].expiry_time = 0;
  }
}

inline void expireRoutes(RouteEntry table[MAX_ROUTE_ENTRIES]) {
  unsigned long now = millis();
  for (int i = 0; i < MAX_ROUTE_ENTRIES; ++i) {
    if (table[i].valid && now >= table[i].expiry_time) {
      table[i].valid = false;
    }
  }
}

inline int findRoute(RouteEntry table[MAX_ROUTE_ENTRIES],
                     const uint8_t dest_mac[6]) {
  for (int i = 0; i < MAX_ROUTE_ENTRIES; ++i) {
    if (table[i].valid && macEquals(table[i].dest_mac, dest_mac)) {
      return i;
    }
  }
  return -1;
}

inline void invalidateRoute(RouteEntry table[MAX_ROUTE_ENTRIES],
                            const uint8_t dest_mac[6]) {
  int idx = findRoute(table, dest_mac);
  if (idx >= 0) {
    table[idx].valid = false;
  }
}

inline bool addRoute(RouteEntry table[MAX_ROUTE_ENTRIES],
                     const uint8_t dest_mac[6],
                     const uint8_t next_hop_mac[6],
                     uint8_t hop_count) {
  expireRoutes(table);
  int idx = findRoute(table, dest_mac);

  if (idx >= 0) {
    if (hop_count < table[idx].hop_count) {
      copyMac(table[idx].next_hop_mac, next_hop_mac);
      table[idx].hop_count = hop_count;
      table[idx].expiry_time = millis() + ROUTE_EXPIRY_MS;
      return true;
    }
    if (hop_count == table[idx].hop_count &&
        macEquals(table[idx].next_hop_mac, next_hop_mac)) {
      table[idx].expiry_time = millis() + ROUTE_EXPIRY_MS;
      return true;
    }
    return false;
  }

  for (int i = 0; i < MAX_ROUTE_ENTRIES; ++i) {
    if (!table[i].valid) {
      table[i].valid = true;
      copyMac(table[i].dest_mac, dest_mac);
      copyMac(table[i].next_hop_mac, next_hop_mac);
      table[i].hop_count = hop_count;
      table[i].expiry_time = millis() + ROUTE_EXPIRY_MS;
      return true;
    }
  }

  int oldest_idx = 0;
  for (int i = 1; i < MAX_ROUTE_ENTRIES; ++i) {
    if (table[i].expiry_time < table[oldest_idx].expiry_time) {
      oldest_idx = i;
    }
  }

  table[oldest_idx].valid = true;
  copyMac(table[oldest_idx].dest_mac, dest_mac);
  copyMac(table[oldest_idx].next_hop_mac, next_hop_mac);
  table[oldest_idx].hop_count = hop_count;
  table[oldest_idx].expiry_time = millis() + ROUTE_EXPIRY_MS;
  return true;
}

inline void resetSeenTable(SeenEntry table[MAX_SEEN_ENTRIES]) {
  for (int i = 0; i < MAX_SEEN_ENTRIES; ++i) {
    table[i].valid = false;
    table[i].packet_id = 0;
    table[i].expiry_time = 0;
  }
}

inline void expireSeenEntries(SeenEntry table[MAX_SEEN_ENTRIES]) {
  unsigned long now = millis();
  for (int i = 0; i < MAX_SEEN_ENTRIES; ++i) {
    if (table[i].valid && now >= table[i].expiry_time) {
      table[i].valid = false;
    }
  }
}

inline bool isSeen(SeenEntry table[MAX_SEEN_ENTRIES],
                   const uint8_t origin_mac[6],
                   uint16_t packet_id) {
  expireSeenEntries(table);
  for (int i = 0; i < MAX_SEEN_ENTRIES; ++i) {
    if (table[i].valid &&
        table[i].packet_id == packet_id &&
        macEquals(table[i].origin_mac, origin_mac)) {
      return true;
    }
  }
  return false;
}

inline void markSeen(SeenEntry table[MAX_SEEN_ENTRIES],
                     const uint8_t origin_mac[6],
                     uint16_t packet_id) {
  for (int i = 0; i < MAX_SEEN_ENTRIES; ++i) {
    if (table[i].valid &&
        table[i].packet_id == packet_id &&
        macEquals(table[i].origin_mac, origin_mac)) {
      return;
    }
  }

  for (int i = 0; i < MAX_SEEN_ENTRIES; ++i) {
    if (!table[i].valid) {
      table[i].valid = true;
      table[i].packet_id = packet_id;
      table[i].expiry_time = millis() + SEEN_EXPIRY_MS;
      copyMac(table[i].origin_mac, origin_mac);
      return;

    }
  }

  int oldest_idx = 0;
  for (int i = 1; i < MAX_SEEN_ENTRIES; ++i) {
    if (table[i].expiry_time < table[oldest_idx].expiry_time) {
      oldest_idx = i;
    }
  }

  table[oldest_idx].valid = true;
  table[oldest_idx].packet_id = packet_id;
  table[oldest_idx].expiry_time = millis() + SEEN_EXPIRY_MS;
  copyMac(table[oldest_idx].origin_mac, origin_mac);
  return;
}

inline bool seenCheck(SeenEntry table[MAX_SEEN_ENTRIES],
                      const uint8_t origin_mac[6],
                      uint16_t packet_id) {
  expireSeenEntries(table);
  if (isSeen(table, origin_mac, packet_id)) {
    return true;
  }
  markSeen(table, origin_mac, packet_id);
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
