#include <M5StickCPlus.h>
#include <esp_now.h>
#include <WiFi.h>

// Known sender MACs
const char* MAC_A = "0C:8B:95:A8:1D:2C";
const char* MAC_C = "4C:75:25:CB:7E:54";

struct PressEvent {
  char mac[18];
  unsigned long timestamp;
  bool received;
};

PressEvent pressA = {"", 0, false};
PressEvent pressC = {"", 0, false};

bool roundActive = true;

void declareWinner() {
  roundActive = false;
  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setCursor(10, 20);
  M5.Lcd.setTextSize(2);

  if (pressA.received && pressC.received) {
    long diff = (long)pressA.timestamp - (long)pressC.timestamp;
    if (diff < 0) {
      M5.Lcd.println("Winner: A!");
      Serial.printf("Winner: A | diff: %ld ms\n", -diff);
    } else if (diff > 0) {
      M5.Lcd.println("Winner: C!");
      Serial.printf("Winner: C | diff: %ld ms\n", diff);
    } else {
      M5.Lcd.println("TIE!");
      Serial.println("TIE!");
    }
  } else if (pressA.received) {
    M5.Lcd.println("Winner: A!\n(C no press)");
    Serial.println("Winner: A (C did not press)");
  } else {
    M5.Lcd.println("Winner: C!\n(A no press)");
    Serial.println("Winner: C (A did not press)");
  }

  // Reset after 3 seconds
  delay(3000);
  pressA = {"", 0, false};
  pressC = {"", 0, false};
  roundActive = true;
  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setCursor(10, 20);
  M5.Lcd.println("Ready!\nWaiting...");
  Serial.println("--- New round ---");
}

void onDataReceived(const esp_now_recv_info *recvInfo, const uint8_t *data, int len) {
  if (!roundActive) return;

  char senderMAC[18];
  snprintf(senderMAC, sizeof(senderMAC), "%02X:%02X:%02X:%02X:%02X:%02X",
    recvInfo->src_addr[0], recvInfo->src_addr[1], recvInfo->src_addr[2],
    recvInfo->src_addr[3], recvInfo->src_addr[4], recvInfo->src_addr[5]);

  unsigned long receiveTime = millis();
  Serial.printf("Received from %s at %lu ms\n", senderMAC, receiveTime);

  if (strcmp(senderMAC, MAC_A) == 0 && !pressA.received) {
    strncpy(pressA.mac, senderMAC, 18);
    pressA.timestamp = receiveTime;
    pressA.received = true;
  } else if (strcmp(senderMAC, MAC_C) == 0 && !pressC.received) {
    strncpy(pressC.mac, senderMAC, 18);
    pressC.timestamp = receiveTime;
    pressC.received = true;
  }

  // Declare winner once both pressed, or after one presses
  if (pressA.received && pressC.received) {
    declareWinner();
  }
}

void setup() {
  M5.begin();
  Serial.begin(115200);
  delay(1000);
  WiFi.mode(WIFI_STA);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    return;
  }

  esp_now_register_recv_cb(onDataReceived);

  // Register A as peer
  esp_now_peer_info_t peerA = {};
  uint8_t macA[] = {0x0C, 0x8B, 0x95, 0xA8, 0x1D, 0x2C};
  memcpy(peerA.peer_addr, macA, 6);
  peerA.channel = 0;
  peerA.encrypt = false;
  esp_now_add_peer(&peerA);

  // Register C as peer
  esp_now_peer_info_t peerC = {};
  uint8_t macC[] = {0x4C, 0x75, 0x25, 0xCB, 0x7E, 0x54};
  memcpy(peerC.peer_addr, macC, 6);
  peerC.channel = 0;
  peerC.encrypt = false;
  esp_now_add_peer(&peerC);

  M5.Lcd.setTextSize(2);
  M5.Lcd.println("Ready!\nWaiting...");
  Serial.println("D server ready");
  Serial.println("--- New round ---");
}

void loop() {
  M5.update();
}