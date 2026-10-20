#pragma once

#include <stdbool.h>

// Remote access through an MQTT broker (see docs/remote-access.md). The device keeps an
// outgoing connection open, so nothing needs to reach it from the internet:
//   <prefix>/<id>/state            retained, the short state: all any phone may see
//   <prefix>/<id>/state/<key id>   retained, the full state, sealed for that admin key
//   <prefix>/<id>/status           retained, "online", or "offline" (left as the last will)
//   <prefix>/<id>/cmd              signed commands from phones
//   <prefix>/<id>/reply            replies to them

// Starts the task that owns the MQTT client. It connects when remote access is set up.
void remote_start(void);

// Reconnects with the current settings.
void remote_restart(void);

// Republishes the state, after a short delay that collects changes arriving together.
void remote_notify(void);

bool remote_enabled(void);
bool remote_connected(void);
