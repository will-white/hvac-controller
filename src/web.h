#ifndef WEB_H
#define WEB_H

// Local web dashboard on port 80: live status (with target provenance and
// health), mode buttons, hold form, fault/lockout reset, firmware updates
// (status, mode, check/install, and /update for uploading a .bin behind basic
// auth). JSON API under /api/.
// Starts once WiFi is up; no external assets, one self-contained page.
void web_poll();

#endif // WEB_H
