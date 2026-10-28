#ifndef NET_TIME_H
#define NET_TIME_H

// WiFi + SNTP. Credentials come from netconfig (NVS, seeded from the gitignored
// src/secrets.h); with no credentials the firmware runs offline and
// isTimeValid() stays false, which drops the schedule to a safe hold.
void net_time_begin();
void net_time_poll();      // non-blocking reconnect state machine
bool isWifiConnected();
bool isTimeValid();        // true once SNTP has produced a sane wall-clock time

#endif // NET_TIME_H
