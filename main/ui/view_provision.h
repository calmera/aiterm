// "Set up from phone" screen: QR codes for joining the SoftAP and for opening the page.
#pragma once
#include "ui.h"
// Push the provisioning view. If sta_url is non-NULL (device already on Wi-Fi) it also shows
// the station URL. Must be called on the UI task.
void view_provision_show(const char* ap_ssid, const char* ap_password, const char* sta_url);
void view_provision_update(void);  // refresh dynamic info (clients, station IP)
