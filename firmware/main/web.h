#pragma once

// The local web server: the app itself, Wi-Fi setup, and the API on the home network.
void web_start(void);

// Pushes the state to every signed-in page. Safe to call from any task.
void web_notify(void);
