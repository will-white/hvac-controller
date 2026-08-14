#ifndef OTA_H
#define OTA_H

// Over-the-air updates via ArduinoOTA (espota). Disabled unless OTA_PASSWORD
// is set in secrets.h. On update start: all relays off, learning/health data
// flushed, task WDT unsubscribed (flash writes block the loop).
void ota_poll();

#endif // OTA_H
