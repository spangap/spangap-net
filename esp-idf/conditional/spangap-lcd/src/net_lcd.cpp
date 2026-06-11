/**
 * net_lcd.cpp — on-device Settings → Net → Wifi pane (LVGL).
 *
 * The Wifi settings pane that net.cpp used to register inside netInit when
 * spangap-lcd was staged. This file lives under conditional/spangap-lcd/,
 * compiled only when the lcd straddle is staged, so no #if is needed. It is
 * invoked via the when:-gated netLcdRegister init hook (spangap/spangap-lcd).
 */
#include "lcd.h"

/* On-device Settings → Net → Wifi pane. Mirrors the browser's NetworkPanel with
 * the controls the simple lcdSetting* helpers can express; the STA known-network
 * list (an array with per-network DHCP/MAC editing) stays browser-only. Runs on
 * the lcd task; storage keys must be static (the helpers store them by pointer). */
static void wifiSettingsPane(void* arg) {
  lv_obj_t* p = static_cast<lv_obj_t*>(arg);

  lcdSettingSection(p, "WiFi");
  lcdSettingSwitch (p, "Enable", "s.net.wifi.enable");
  lcdSettingValue  (p, "Status", "wifi.sta.state");   /* live, ~1 Hz */
  lcdSettingValue  (p, "SSID",   "wifi.sta.ssid");
  lcdSettingValue  (p, "IP",     "wifi.sta.ip");
  lcdSettingValue  (p, "Signal", "wifi.sta.rssi");

  lcdSettingSection(p, "Access Point");
  lcdSettingText   (p, "Name",     "s.net.wifi.ap.ssid");
  lcdSettingText   (p, "Password", "s.net.wifi.ap.pass");
  lcdSettingText   (p, "IP",       "s.net.wifi.ap.ip");
  lcdSettingText   (p, "Netmask",  "s.net.wifi.ap.mask");
}

/* On-device Settings → System pane. Mirrors the browser SystemPanel: device
 * hostname + time/date (NTP). Top-level group (placement 1 sorts it above
 * Internet / Mesh Network, matching the web menu order). The timezone is a
 * free-text IANA name (e.g. "Europe/Berlin"); the browser offers the picker. */
static void systemSettingsPane(void* arg) {
  lv_obj_t* p = static_cast<lv_obj_t*>(arg);
  lcdSettingSection(p, "This System");
  lcdSettingText   (p, "Hostname", "s.net.hostname");
  lcdSettingSection(p, "Time & Date");
  lcdSettingText   (p, "Timezone",   "s.ntp.tz");
  lcdSettingText   (p, "NTP Server", "s.ntp.server");
}

/* Register the on-device Net settings panes — a when:-gated init: hook
 * (spangap/spangap-lcd). Plain C++ linkage to match the generated dispatcher's
 * forward decl. */
void netLcdRegister(void) {
  lcdRegisterSettings("System", "System", systemSettingsPane, 1);
  lcdRegisterSettings("Internet/WiFi", "WiFi", wifiSettingsPane, 1);
}
