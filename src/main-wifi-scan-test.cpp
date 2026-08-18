// Minimal WiFi scan probe (wifi_scan_oldcore env, pinned to an OLDER pioarduino
// release): does the driver's scan path work on this board with a different
// core? Raw RF provably works on 55.03.311, but every driver-managed RX path
// (scan/join/AP-auth) acts deaf - this isolates the framework as the variable.
#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  for (unsigned long t0 = millis(); !Serial && millis() - t0 < 3000; ) delay(10);
  Serial.println("\n=== WiFi scan probe (older core) ===");
  Serial.printf("Arduino core: %d.%d.%d\n", ESP_ARDUINO_VERSION_MAJOR, ESP_ARDUINO_VERSION_MINOR, ESP_ARDUINO_VERSION_PATCH);
  WiFi.mode(WIFI_STA);
  esp_wifi_set_ps(WIFI_PS_NONE);
}

void loop() {
  unsigned long t0 = millis();
  int n = WiFi.scanNetworks();
  Serial.printf("[%4lus] scan: %d network(s) in %lums\n", millis() / 1000, n, millis() - t0);
  for (int i = 0; i < n && i < 10; i++) {
    Serial.printf("   %-32s ch%-2d %4ddBm\n", WiFi.SSID(i).c_str(), (int)WiFi.channel(i), (int)WiFi.RSSI(i));
  }
  WiFi.scanDelete();
  delay(5000);
}
