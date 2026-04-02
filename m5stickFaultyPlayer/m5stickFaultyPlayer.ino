#include <M5StickCPlus.h>
#include <esp_now.h>
#include <WiFi.h>
#include "game_protocol.h"
#include "espnow_utils.h"
#include "general_utils.h"

uint8_t myMac[6];
uint8_t serverMac[6] = {0}; // Will be discovered via GO packet
uint8_t broadcastMac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Demo topology: block the direct server <-> faulty-player link.
// Fill this with the real m5stickNewServer MAC before flashing for the demo.
const uint8_t blockedServerMac[6] = {0xE8, 0x9F, 0x6D, 0x09, 0x03, 0xA4};

bool lastButtonState = false;
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 50;
bool gameStarted = false;
unsigned long startTime = 0;
bool pendingPressValid = false;
bool awaitingAck = false;
unsigned long ackDeadline = 0;
ButtonUiEvent uiEvent = BUTTON_UI_NONE;
unsigned long deliveredReactionMs = 0;
GamePacket pendingPress = {};
unsigned long lastRouteRequestTime = 0;
unsigned long lastJoinIntentTime = 0;
uint16_t packetCounter = 0;
SeenEntry seenTable[MAX_SEEN_ENTRIES];
RouteEntry routeTable[MAX_ROUTE_ENTRIES];
ResultState resultState = {0, 0, 0};

bool hasConfiguredMac(const uint8_t mac[6])
{
  for (int i = 0; i < 6; ++i)
  {
    if (mac[i] != 0x00)
    {
      return true;
    }
  }
  return false;
}

bool isBroadcastMac(const uint8_t mac[6])
{
  for (int i = 0; i < 6; ++i)
  {
    if (mac[i] != 0xFF)
    {
      return false;
    }
  }
  return true;
}

bool shouldBlockDirectPeer(const uint8_t mac[6])
{
  if (isBroadcastMac(mac))
  {
    return false;
  }
  if (!hasConfiguredMac(blockedServerMac))
  {
    return false;
  }

  return macEquals(mac, blockedServerMac);
}

void logBlockedNeighbor()
{
  char macStr[18];
  macToStr(blockedServerMac, macStr);
  if (!hasConfiguredMac(blockedServerMac))
  {
    LOG("Demo topology: blocked server MAC not configured yet");
    return;
  }

  LOG("Demo topology: FaultyPlayer blocks direct server link to %s", macStr);
}

void onDataReceived(const esp_now_recv_info *recvInfo, const uint8_t *data, int len)
{
  char srcStr[18];
  macToStr(recvInfo->src_addr, srcStr);
  if (shouldBlockDirectPeer(recvInfo->src_addr))
  {
    LOG("DROP: source %s is blocked for FaultyPlayer", srcStr);
    return;
  }

  handleButtonNodeReceive(recvInfo, data, len, myMac, broadcastMac, packetCounter,
                          seenTable, routeTable, gameStarted, pendingPressValid,
                          awaitingAck, ackDeadline, uiEvent, deliveredReactionMs,
                          pendingPress, lastButtonState, lastDebounceTime, startTime, serverMac, resultState);
}

void setup()
{
  M5.begin();
  Serial.begin(115200);
  delay(1000);
  randomSeed(esp_random());
  WiFi.mode(WIFI_STA);
  esp_wifi_get_mac(WIFI_IF_STA, myMac);

  char actualStr[18];
  macToStr(myMac, actualStr);
  LOG("Player Node | actual MAC: %s", actualStr);

  if (!configureEspNowChannel())
  {
    LOG("ERROR: configureEspNowChannel() FAILED");
  }
  else
  {
    LOG("WiFi channel locked to %d", ESPNOW_CHANNEL);
  }

  if (esp_now_init() != ESP_OK)
  {
    LOG("ERROR: esp_now_init() FAILED — no traffic will flow");
  }
  else
  {
    LOG("esp_now_init() OK");
  }

  if (esp_now_set_pmk(ESPNOW_PMK) != ESP_OK)
  {
    LOG("ERROR: esp_now_set_pmk() FAILED");
  }
  else
  {
    LOG("ESP-NOW PMK set OK");
  }

  esp_now_register_send_cb([](const wifi_tx_info_t *info, esp_now_send_status_t status)
                           {
  char macStr[18];
  macToStr(info->des_addr, macStr);
  LOG("SEND to %s: %s", macStr,
      status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL (no ack - wrong MAC or out of range?)"); });

  esp_now_register_recv_cb(onDataReceived);

  registerPeer(broadcastMac);
  resetSeenTable(seenTable);
  resetRouteTable(routeTable);
  logBlockedNeighbor();

  delay(100);
  // Send initial route request to discover server
  sendInitialRREQ(myMac, broadcastMac, packetCounter);

  M5.Lcd.setTextSize(2);
  M5.Lcd.println("Player Node\nWaiting\nfor GO...");
  LOG("Player setup complete");
}

void loop()
{
  handleButtonNodeLoop(myMac, broadcastMac, packetCounter, routeTable,
                       gameStarted, pendingPressValid, awaitingAck, ackDeadline,
                       uiEvent, deliveredReactionMs, pendingPress, lastButtonState, lastDebounceTime,
                       lastRouteRequestTime, lastJoinIntentTime, debounceDelay, startTime, serverMac, resultState);
}
