#include <M5StickCPlus.h>
#include <esp_now.h>
#include <WiFi.h>
#include "../game_protocol.h"

// C only knows B — out of range of D
uint8_t myMac[] = {0x4C, 0x75, 0x25, 0xCB, 0x7E, 0x54};
uint8_t macB[] = {0x4C, 0x75, 0x25, 0xCB, 0x89, 0x98};
uint8_t macD[] = {0xD4, 0xD4, 0xDA, 0x85, 0x4D, 0x98};

bool lastButtonState = false;
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 50;
bool gameStarted = false;
unsigned long startTime = 0;
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

  if (!isLocalMac(pkt.dest_mac, myMac)) {
    return;
  }

  if (pkt.type == PACKET_GO) {
    startTime = millis();
    gameStarted = true;
    Serial.println("GO! Timer started via relay.");
    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setCursor(10, 30);
    M5.Lcd.setTextSize(3);
    M5.Lcd.println("GO!");
  } else if (pkt.type == PACKET_RESULT) {
    gameStarted = false;
    Serial.println("Round complete. Resetting player state.");
    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setCursor(10, 20);
    M5.Lcd.setTextSize(2);
    M5.Lcd.println("Round done");
    M5.Lcd.setTextSize(1);
    M5.Lcd.println("Waiting for GO...");
  }
}

void setup() {
  M5.begin();
  Serial.begin(115200);
  delay(1000);
  WiFi.mode(WIFI_STA);
  Serial.println("Node C ready");

  esp_now_init();
  esp_now_register_recv_cb(onDataReceived);

  registerPeer(macB);
  resetDedupCache(dedupCache);

  M5.Lcd.setTextSize(2);
  M5.Lcd.println("Node C\nOut of range\nWaiting GO...");
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

    // Send via B since C can't reach D
    esp_now_send(macB, (uint8_t*)&pkt, sizeof(pkt));
    Serial.printf("Pressed! Reaction time: %lu ms | sending via B\n", pkt.reaction_ms);

    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setCursor(10, 30);
    M5.Lcd.setTextSize(2);
    M5.Lcd.printf("Via B!\n%lu ms", pkt.reaction_ms);

    gameStarted = false;
  }
  lastButtonState = currentPress;
}
