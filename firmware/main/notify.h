#pragma once

#include "intercom.h"

// Push notifications for rings through ntfy (https://ntfy.sh), so phones hear about a
// visitor even with the app closed. Off until a topic URL is set in the app's settings.
void notify_start(void);

// Queues a notification for a ring on `line`. Never blocks; safe from any task.
void notify_ring(line_t line);
