#ifndef MQTT_H
#define MQTT_H

// MQTT integration (disabled unless mqtt_host is set; see netconfig.h):
//  - Home Assistant climate entity + outdoor/health sensors via MQTT discovery
//  - command topics (mode, dual setpoints -> temporary holds, hold release)
//  - Zigbee2MQTT ingestion: Aqara probe topics feed sensors_injectRemote()
//    (indoor) and sensors_injectOutdoorProbe() (outdoor)
void mqtt_begin();
void mqtt_poll();
bool mqtt_connected();

#endif // MQTT_H
