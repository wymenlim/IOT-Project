#include <M5StickCPlus.h>
#include <esp_now.h>
#include <WiFi.h>
#include "../game_protocol.h"

uint8_t myMac[6];
uint8_t macA[] = {0x0C, 0x8B, 0x95, 0xA8, 0x1D, 0x2C};
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

void sendPacket(const uint8_t* mac, GamePacket &pkt, const char* label) {
  esp_err_t err = esp_now_send(mac, (uint8_t*)&pkt, sizeof(pkt));
  if (err != ESP_OK) {
    LOG("ERROR: %s failed immediately (err=%d)", label, err);
  }
}

void onDataReceived(const esp_now_recv_info *recvInfo, const uint8_t *data, int len) {
  char srcStr[18];
  macToStr(recvInfo->src_addr, srcStr);
  LOG("RECV from %s | len=%d", srcStr, len);

  if (len != sizeof(GamePacket)) {
    LOG("DROP: wrong len (got %d, want %d)", len, (int)sizeof(GamePacket));
    return;
  }

  GamePacket pkt;
  memcpy(&pkt, data, sizeof(pkt));

  char originStr[18]; macToStr(pkt.origin_mac, originStr);
  char destStr[18];   macToStr(pkt.dest_mac, destStr);
  LOG("PKT type=%d origin=%s dest=%s hop=%d id=%u",
      pkt.type, originStr, destStr, pkt.hop_count, pkt.packet_id);

  if (isDuplicateAndRemember(dedupCache, dedupIndex, pkt.origin_mac, pkt.packet_id)) {
    LOG("DROP: duplicate (origin=%s id=%u)", originStr, pkt.packet_id);
    return;
  }

  if (pkt.type == PACKET_GO && isLocalMac(pkt.dest_mac, myMac)) {
    startTime = millis();
    gameStarted = true;
    lastButtonState = false;
    lastDebounceTime = 0;
    LOG("GO accepted | timer started at %lu ms", startTime);

    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setCursor(10, 30);
    M5.Lcd.setTextSize(3);
    M5.Lcd.println("GO!");
    return;
  }

  if (pkt.type == PACKET_RESULT) {
    if (isLocalMac(pkt.dest_mac, myMac)) {
      gameStarted = false;
      lastButtonState = false;
      LOG("RESULT received | round complete, resetting");
      M5.Lcd.fillScreen(BLACK);
      M5.Lcd.setCursor(10, 20);
      M5.Lcd.setTextSize(2);
      M5.Lcd.println("Round done");
      M5.Lcd.setTextSize(1);
      M5.Lcd.println("Waiting for GO...");
    }
    return;
  }

  LOG("DROP: no matching rule for type=%d dest=%s origin=%s", pkt.type, destStr, originStr);
}

void setup() {
  M5.begin();
  Serial.begin(115200);
  delay(1000);
  WiFi.mode(WIFI_STA);
  esp_wifi_get_mac(WIFI_IF_STA, myMac);

  char actualStr[18];
  macToStr(myMac, actualStr);
  LOG("Node B | actual MAC: %s", actualStr);

  if (!configureEspNowChannel()) {
    LOG("ERROR: configureEspNowChannel() FAILED");
  } else {
    LOG("WiFi channel locked to %d", ESPNOW_CHANNEL);
  }

  if (esp_now_init() != ESP_OK) {
    LOG("ERROR: esp_now_init() FAILED — no traffic will flow");
  } else {
    LOG("esp_now_init() OK");
  }

  esp_now_register_send_cb([](const wifi_tx_info_t *info, esp_now_send_status_t status) {
    char macStr[18];
    macToStr(info->des_addr, macStr);
    LOG("SEND to %s: %s", macStr,
        status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL (no ack — wrong MAC or out of range?)");
  });

  esp_now_register_recv_cb(onDataReceived);

  registerPeer(macA);
  registerPeer(macD);
  resetDedupCache(dedupCache);

  M5.Lcd.setTextSize(2);
  M5.Lcd.println("Node B\nWaiting\nfor GO...");
  LOG("Node B setup complete");
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

    LOG("PRESS sending to D | reaction_ms=%lu hop=%d id=%u",
        (unsigned long)pkt.reaction_ms, pkt.hop_count, pkt.packet_id);
    sendPacket(macD, pkt, "PRESS to D");

    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setCursor(10, 30);
    M5.Lcd.setTextSize(2);
    M5.Lcd.printf("Sent!\n%lu ms", pkt.reaction_ms);

    gameStarted = false;
  }
  lastButtonState = currentPress;
}
