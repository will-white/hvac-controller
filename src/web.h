#ifndef WEB_H
#define WEB_H

// Local web dashboard on port 80: live status (with target provenance and
// health), mode buttons, hold form, fault/lockout reset. JSON API under /api/.
// Starts once WiFi is up; no external assets, one self-contained page.
void web_poll();

#endif // WEB_H
