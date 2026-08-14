#ifndef WEATHER_H
#define WEATHER_H

// Outdoor temperature via open-meteo (plain HTTP, no TLS/JSON deps).
// Polls every WEATHER_POLL_MINS while WiFi is up and coordinates are set in
// secrets.h; results feed sensors_injectOutdoor() with its staleness window.
void weather_poll();

#endif // WEATHER_H
