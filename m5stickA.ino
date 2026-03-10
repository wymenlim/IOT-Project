#include <M5StickCPlus.h>
#include <esp_now.h>
#include <WiFi.h>

uint8_t receiverMAC[] = {0x4C, 0x75, 0x25, 0xCB, 0x89, 0x98};

bool lastButtonState = false;
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 50;
unsigned long sentTime = 0;

void onSendCallback(const wifi_tx_info_t *txInfo, esp_now_send_status_t status) {
  unsigned long ackTime = millis();
  unsigned long roundTrip = ackTime - sentTime;
  Serial.print("ACK: ");
  Serial.print(status == ESP_NOW_SEND_SUCCESS ? "Success" : "Failed");
  Serial.print(" | Round trip: ");
  Serial.print(roundTrip);
  Serial.println(" ms");

  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setCursor(10, 30);
  M5.Lcd.setTextSize(2);
  M5.Lcd.print(roundTrip);
  M5.Lcd.println(" ms");
}

void setup() {
  M5.begin();
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    return;
  }

  esp_now_register_send_cb(onSendCallback);

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, receiverMAC, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;
  esp_now_add_peer(&peerInfo);

  M5.Lcd.setTextSize(2);
  M5.Lcd.println("Ready!\nPress A");
  Serial.println("Sender ready");
}

void loop() {
  M5.update();
  bool currentPress = M5.BtnA.isPressed();

  if (currentPress && !lastButtonState &&
     (millis() - lastDebounceTime > debounceDelay)) {
    lastDebounceTime = millis();
    sentTime = millis();
    uint8_t dummy = 1;
    esp_now_send(receiverMAC, &dummy, sizeof(dummy));
    Serial.println("Button pressed — packet sent!");
  }

  lastButtonState = currentPress;
}