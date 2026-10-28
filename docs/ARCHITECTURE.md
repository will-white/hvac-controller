# System architecture

How every device in the chosen design talks to every other (architecture
decision of 2026-10-28 in ROADMAP.md): Zigbee sensors reach the thermostat
through Zigbee2MQTT and the MQTT broker, Home Assistant and Matterbridge carry
it to Apple Home / Google Home / Alexa, and the thermostat drives the HVAC
equipment directly.

![Network diagram](architecture.svg)

Line colour is the medium: blue = WiFi / LAN / internet, purple = Zigbee radio,
green = wired (USB, I²C), orange = 24 VAC (solid = relay contacts, dashed =
sensing). Dashed boxes are optional or not set up yet.

## Every link

| # | From → to | Medium | What travels |
|---|---|---|---|
| 1 | Aqara probes → USB dongle | Zigbee, 2.4 GHz | Temperature, humidity, battery; on change plus a periodic heartbeat |
| 2 | Dongle → Zigbee2MQTT | USB serial | Zigbee traffic from the coordinator |
| 3 | Zigbee2MQTT → broker | MQTT, LAN | JSON on `zigbee2mqtt/<probe>`, temperature in °C. Turn on `retain` so a rebooted thermostat gets a reading at once |
| 4 | Thermostat ⇄ broker | MQTT over WiFi, TCP 1883 | **In:** the probe topics from `secrets.h` (`Z2M_INDOOR_TOPIC`, `Z2M_OUTDOOR_TOPIC`), plus `cmd/mode`, `cmd/heat_sp`, `cmd/cool_sp`, `cmd/hold_off`. **Out:** state every 30 s, HA discovery (retained), availability with last-will |
| 5 | Home Assistant ⇄ broker | MQTT, LAN | Climate entity with heat and cool setpoints, outdoor and health sensors, commands |
| 6 | Matterbridge ⇄ Home Assistant | HA WebSocket API | Thermostat state and commands (not set up yet) |
| 7 | Apple / Google / Alexa ⇄ Matterbridge | Matter, LAN | The thermostat as a Matter thermostat device (not set up yet) |
| 8 | Phone or browser ⇄ thermostat | HTTP, port 80 | Dashboard plus `/api/status`, `mode`, `hold`, `holdoff`, `reset`. Works on the LAN with the home server down |
| 9 | Thermostat → open-meteo | Plain HTTP, every 10 min | Outdoor temperature, trusted for 30 min; a fresh outdoor probe outranks it |
| 10 | Thermostat → pool.ntp.org | SNTP | Wall-clock time; without it the schedule yields to a safe fallback hold |
| 11 | Laptop ⇄ thermostat | USB-C serial / WiFi | Console at 115200 baud and USB flashing; password-gated OTA over WiFi |
| 12 | EVB ⇄ MOD-IO2 and SHT4x | UEXT I²C, SDA 6 / SCL 7 | Relay mask to 0x21; temperature and humidity from 0x44 when fitted |
| 13 | EVB relays → equipment | Relay contacts, 24 VAC | W heat, Y cool, G fan, fresh-air damper (GPIO 10/11/22/23) |
| 14 | MOD-IO2 → humidifier | Relay contact | Humidifier on/off; relay 2 is a spare |
| 15 | Equipment → opto inputs | 24 VAC sense | Proof that heat, cool and fan calls really run (OPT1–3, GPIO 1/2/3); window contact (OPT4, GPIO 15). Field wiring not done yet |

## What happens when a link breaks

- **Home server, broker or WiFi down** *(risk)*: the indoor probe goes stale
  after 90 minutes (`REMOTE_SENSOR_STALE_MINS`). Without the SHT4x fitted, the
  thermostat latches a fault with heat off until someone runs `reset fault`.
  Home Assistant and Matter control stop too. Tracked as "Hub-outage
  resilience" in ROADMAP.md.
- **Internet down**: no weather, so the economizer switches off. The clock keeps
  running from its last sync and heating and cooling continue normally.
- **Zigbee dongle or probe fails**: same 90-minute path as a server outage.
  Battery below 15% is logged as a health warning first.
- **Thermostat stops or hangs** *(risk)*: EVB relays open on their own, so
  furnace, A/C, fan and damper stop. The MOD-IO2 keeps its humidifier relay
  closed until the task watchdog reboots the C6.

`architecture.svg` is hand-placed SVG; keep it in step with this table when a
link changes.
