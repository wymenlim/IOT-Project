#include <M5StickCPlus.h>
#include <esp_now.h>
#include <WiFi.h>
#include "../game_protocol.h"
#include "../espnow_utils.h"
#include "../general_utils.h"

uint8_t myMac[6];
uint8_t macA[] = {0x0C, 0x8B, 0x95, 0xA8, 0x1D, 0x2C};
uint8_t macD[] = {0xD4, 0xD4, 0xDA, 0x85, 0x4D, 0x98};
uint8_t broadcastMac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

bool lastButtonState = false;
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 50;
bool gameStarted = false;
unsigned long startTime = 0;
uint16_t packetCounter = 0;
SeenEntry seenTable[MAX_SEEN_ENTRIES];
RouteEntry routeTable[MAX_ROUTE_ENTRIES];

void onDataReceived(const esp_now_recv_info *recvInfo, const uint8_t *data, int len) {
  handleButtonNodeReceive(recvInfo, data, len, myMac, broadcastMac, packetCounter,
                          seenTable, routeTable, gameStarted, lastButtonState,
                          lastDebounceTime, startTime);
}

void setup() {
  M5.begin();
  Serial.begin(115200);
  delay(1000);
  randomSeed(esp_random());
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
  registerPeer(broadcastMac);
  resetSeenTable(seenTable);
  resetRouteTable(routeTable);

  delay(100);
  sendInitialRREQ(myMac, macD, broadcastMac, packetCounter);

  M5.Lcd.setTextSize(2);
  M5.Lcd.println("Node B\nWaiting\nfor GO...");
  LOG("Node B setup complete");
}

void loop() {
  handleButtonNodeLoop(myMac, macD, packetCounter, gameStarted, lastButtonState,
                       lastDebounceTime, debounceDelay, startTime);
}
