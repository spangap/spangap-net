/**
 * mdns_lcd.cpp — on-device Settings → Net → mDNS pane (LVGL).
 *
 * The mDNS settings pane that spangap_mdns.cpp used to register inside mdnsInit
 * when spangap-lcd was staged. This file lives under conditional/spangap-lcd/,
 * compiled only when the lcd straddle is staged, so no #if is needed. It is
 * invoked via the when:-gated mdnsLcdRegister init hook (spangap/spangap-lcd).
 */
#include "lcd.h"

/* On-device Settings → Net → mDNS pane. The browser exposes a single toggle; on
 * device we surface the actual advertised ports (0 = don't advertise). */
static void mdnsSettingsPane(void* arg) {
    lv_obj_t* p = static_cast<lv_obj_t*>(arg);
    lcdSettingSection(p, "mDNS");
    lcdSettingText   (p, "HTTP port",  "s.net.mdns.http");
    lcdSettingText   (p, "HTTPS port", "s.net.mdns.https");
}

/* Register the on-device Net → mDNS settings pane — a when:-gated init: hook
 * (spangap/spangap-lcd). Plain C++ linkage to match the generated dispatcher's
 * forward decl. */
void mdnsLcdRegister(void) {
    lcdRegisterSettings("Net/mDNS", "mDNS", mdnsSettingsPane);
}
