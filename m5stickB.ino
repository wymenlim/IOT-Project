#include <M5StickCPlus.h>
#include <esp_now.h>
#include <WiFi.h>

int packetCount = 0;

void onDataReceived(const esp_now_recv_info *recvInfo, const uint8_t *data, int len) {
  packetCount++;
  
  Serial.print("Packet received! Count: ");
  Serial.println(packetCount);

  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setCursor(10, 20);
  M5.Lcd.setTextSize(2);
  M5.Lcd.println("Received!");
  M5.Lcd.setCursor(10, 50);
  M5.Lcd.print("Count: ");
  M5.Lcd.println(packetCount);
}

void setup() {
  M5.begin();
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    return;
  }

  esp_now_register_recv_cb(onDataReceived);

  M5.Lcd.setTextSize(2);
  M5.Lcd.println("Waiting...");
  Serial.println("Receiver ready");
}

void loop() {}