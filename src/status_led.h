#ifndef STATUS_LED_H
#define STATUS_LED_H

// EVB user-LED blink codes (highest priority wins):
//   fast continuous blink  - FAULT (sensor implausible; `reset fault`)
//   double-flash / 2s      - safety LOCKOUT (`reset lockout`)
//   solid ON               - service mode (control suspended)
//   triple-flash / 2s      - health warning(s) pending (console `health`)
//   single blip / 2s       - normal heartbeat (control loop alive)
void statusLed_begin();
void statusLed_poll(); // call every loop(); real time, cheap

#endif // STATUS_LED_H
