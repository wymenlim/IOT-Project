#include <M5StickCPlus.h>
#include <esp_now.h>
#include <WiFi.h>
#include "../game_protocol.h"

uint8_t myMac[] = {0x4C, 0x75, 0x25, 0xCB, 0x89, 0x98};
uint8_t macA[] = {0x0C, 0x8B, 0x95, 0xA8, 0x1D, 0x2C};
uint8_t macC[] = {0x4C, 0x75, 0x25, 0xCB, 0x7E, 0x54};
uint8_t macD[] = {0xD4, 0xD4, 0xDA, 0x85, 0x4D, 0x98};

bool lastButtonState = false;
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 50;
bool gameStarted = false;
unsigned long startTime = 0;
int relayCount = 0;
uint16_t packetCounter = 0;
DedupEntry dedupCache[DEDUP_CACHE_SIZE];
uint8_t dedupIndex = 0;

void registerPeer(uint8_t* mac) {
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = 0;
  peer.encrypt = false;
  esp_now_add_peer(&peer);
}

void onDataReceived(const esp_now_recv_info *recvInfo, const uint8_t *data, int len) {
  if (len != sizeof(GamePacket)) {
    return;
  }

  GamePacket pkt;
  memcpy(&pkt, data, sizeof(pkt));

  if (isDuplicateAndRemember(dedupCache, dedupIndex, pkt.origin_mac, pkt.packet_id)) {
    return;
  }

  if (pkt.type == PACKET_GO && isLocalMac(pkt.dest_mac, myMac)) {
    startTime = millis();
    gameStarted = true;
    Serial.println("GO! Timer started. Forwarding GO to C...");

    GamePacket relayPkt = pkt;
    copyMac(relayPkt.dest_mac, macC);
    setRelayFields(relayPkt, myMac);

    esp_now_send(macC, (uint8_t*)&relayPkt, sizeof(relayPkt));

    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setCursor(10, 30);
    M5.Lcd.setTextSize(3);
    M5.Lcd.println("GO!");
    return;
  }

  if (pkt.type == PACKET_RESULT) {
    if (isLocalMac(pkt.dest_mac, myMac)) {
      gameStarted = false;
      Serial.println("Round complete. Resetting player state.");
      M5.Lcd.fillScreen(BLACK);
      M5.Lcd.setCursor(10, 20);
      M5.Lcd.setTextSize(2);
      M5.Lcd.println("Round done");
      M5.Lcd.setTextSize(1);
      M5.Lcd.println("Waiting for GO...");
    }

    if (macEquals(pkt.origin_mac, macD) && isLocalMac(pkt.dest_mac, myMac)) {
      GamePacket relayPkt = pkt;
      copyMac(relayPkt.dest_mac, macC);
      setRelayFields(relayPkt, myMac);
      esp_now_send(macC, (uint8_t*)&relayPkt, sizeof(relayPkt));
    }
    return;
  }

  if (pkt.type == PACKET_PRESS &&
      macEquals(pkt.dest_mac, macD) &&
      macEquals(pkt.origin_mac, macC)) {
    GamePacket relayPkt = pkt;
    setRelayFields(relayPkt, myMac);
    relayCount++;
    esp_now_send(macD, (uint8_t*)&relayPkt, sizeof(relayPkt));
    Serial.printf("Relayed PRESS from C (hop %d)\n", relayPkt.hop_count);

    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setCursor(10, 20);
    M5.Lcd.setTextSize(2);
    M5.Lcd.println("Relayed!");
    M5.Lcd.setCursor(10, 50);
    M5.Lcd.printf("Count: %d", relayCount);
    return;
  }
}

void setup() {
  M5.begin();
  Serial.begin(115200);
  delay(1000);
  WiFi.mode(WIFI_STA);
  Serial.println("Node B ready");

  esp_now_init();
  esp_now_register_recv_cb(onDataReceived);

  registerPeer(macA);
  registerPeer(macC);
  registerPeer(macD);
  resetDedupCache(dedupCache);

  M5.Lcd.setTextSize(2);
  M5.Lcd.println("Node B\nWaiting\nfor GO...");
}

void loop() {
  M5.update();
  if (!gameStarted) return;

  bool currentPress = M5.BtnA.isPressed();
  if (currentPress && !lastButtonState &&
     (millis() - lastDebounceTime > debounceDelay)) {
    lastDebounceTime = millis();

    GamePacket pkt;
    initPacket(
      pkt,
      PACKET_PRESS,
      myMac,
      macD,
      myMac,
      nextPacketId(packetCounter),
      millis() - startTime
    );

    esp_now_send(macD, (uint8_t*)&pkt, sizeof(pkt));
    Serial.printf("[B] Button pressed | reaction time: %lu ms\n", pkt.reaction_ms);

    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setCursor(10, 30);
    M5.Lcd.setTextSize(2);
    M5.Lcd.printf("Sent!\n%lu ms", pkt.reaction_ms);

    gameStarted = false;
  }
  lastButtonState = currentPress;
}
