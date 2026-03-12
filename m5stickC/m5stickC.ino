#include <M5StickCPlus.h>
#include <esp_now.h>
#include <WiFi.h>

#define MY_NODE_ID 3

// C only knows B — out of range of D
uint8_t macB[] = {0x4C, 0x75, 0x25, 0xCB, 0x89, 0x98};

struct ButtonPacket {
  uint8_t nodeId;
  unsigned long pressTime;
  uint8_t hopCount;
};

struct StartPacket {
  uint8_t type;
};

bool lastButtonState = false;
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 50;
bool gameStarted = false;
unsigned long startTime = 0;

void registerPeer(uint8_t* mac) {
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = 0;
  peer.encrypt = false;
  esp_now_add_peer(&peer);
}

void onDataReceived(const esp_now_recv_info *recvInfo, const uint8_t *data, int len) {
  if (len == sizeof(StartPacket)) {
    StartPacket spkt;
    memcpy(&spkt, data, sizeof(spkt));
    if (spkt.type == 0xAA) {
      startTime = millis();
      gameStarted = true;
      Serial.println("GO! Timer started via relay.");
      M5.Lcd.fillScreen(BLACK);
      M5.Lcd.setCursor(10, 30);
      M5.Lcd.setTextSize(3);
      M5.Lcd.println("GO!");
    }
    return;
  }

  // Relay ButtonPacket back to B toward D
  if (len == sizeof(ButtonPacket)) {
    ButtonPacket pkt;
    memcpy(&pkt, data, sizeof(pkt));
    if (pkt.nodeId == MY_NODE_ID) return;
    pkt.hopCount++;
    esp_now_send(macB, (uint8_t*)&pkt, sizeof(pkt));
    Serial.printf("Relayed packet from node %d (hop %d)\n", pkt.nodeId, pkt.hopCount);
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

    ButtonPacket pkt;
    pkt.nodeId = MY_NODE_ID;
    pkt.pressTime = millis() - startTime;
    pkt.hopCount = 0;

    // Send via B since C can't reach D
    esp_now_send(macB, (uint8_t*)&pkt, sizeof(pkt));
    Serial.printf("Pressed! Reaction time: %lu ms | sending via B\n", pkt.pressTime);

    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setCursor(10, 30);
    M5.Lcd.setTextSize(2);
    M5.Lcd.printf("Via B!\n%lu ms", pkt.pressTime);

    gameStarted = false;
  }
  lastButtonState = currentPress;
}