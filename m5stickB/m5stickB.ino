#include <M5StickCPlus.h>
#include <esp_now.h>
#include <WiFi.h>

uint8_t macA[] = {0x0C, 0x8B, 0x95, 0xA8, 0x1D, 0x2C};
uint8_t macC[] = {0x4C, 0x75, 0x25, 0xCB, 0x7E, 0x54};
uint8_t macD[] = {0xD4, 0xD4, 0xDA, 0x85, 0x4D, 0x98};

struct ButtonPacket {
  char senderMAC[18];
  unsigned long pressTime;
  uint8_t hopCount;
};

bool lastButtonState = false;
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 50;
String myMAC;
int packetCount = 0;

void registerPeer(uint8_t* mac) {
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = 0;
  peer.encrypt = false;
  esp_now_add_peer(&peer);
}

void onSendCallback(const wifi_tx_info_t *txInfo, esp_now_send_status_t status) {
  Serial.print("Send status: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Success" : "Failed");
}

void onDataReceived(const esp_now_recv_info *recvInfo, const uint8_t *data, int len) {
  if (len != sizeof(ButtonPacket)) return;

  ButtonPacket pkt;
  memcpy(&pkt, data, sizeof(pkt));

  if (String(pkt.senderMAC) == myMAC) return;

  packetCount++;
  pkt.hopCount++;
  Serial.printf("Relaying packet from %s (hop %d)\n", pkt.senderMAC, pkt.hopCount);
  esp_now_send(macD, (uint8_t*)&pkt, sizeof(pkt));

  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setCursor(10, 20);
  M5.Lcd.setTextSize(2);
  M5.Lcd.println("Relayed!");
  M5.Lcd.setCursor(10, 50);
  M5.Lcd.print("Count: ");
  M5.Lcd.println(packetCount);
  M5.Lcd.setCursor(10, 80);
  M5.Lcd.setTextSize(1);
  M5.Lcd.println(pkt.senderMAC);
}

void setup() {
  M5.begin();
  Serial.begin(115200);
  delay(1000);
  WiFi.mode(WIFI_STA);
  myMAC = WiFi.macAddress();
  Serial.println("My MAC: " + myMAC);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    return;
  }

  esp_now_register_send_cb(onSendCallback);
  esp_now_register_recv_cb(onDataReceived);

  registerPeer(macA);
  registerPeer(macC);
  registerPeer(macD);

  M5.Lcd.setTextSize(2);
  M5.Lcd.println("Node B\nReady!");
  Serial.println("Node B ready");
}

void loop() {
  M5.update();
  bool currentPress = M5.BtnA.isPressed();

  if (currentPress && !lastButtonState &&
     (millis() - lastDebounceTime > debounceDelay)) {
    lastDebounceTime = millis();

    ButtonPacket pkt;
    strncpy(pkt.senderMAC, myMAC.c_str(), 18);
    pkt.pressTime = millis();
    pkt.hopCount = 0;

    esp_now_send(macD, (uint8_t*)&pkt, sizeof(pkt));
    Serial.printf("Button pressed! Sent to D at %lu ms\n", pkt.pressTime);

    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setCursor(10, 30);
    M5.Lcd.setTextSize(2);
    M5.Lcd.printf("Sent!\n%lu ms", pkt.pressTime);
  }

  lastButtonState = currentPress;
}