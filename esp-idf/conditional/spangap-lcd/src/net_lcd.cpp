/**
 * net_lcd.cpp — on-device Settings → Net → WiFi + System panes (LVGL).
 *
 * Compiled only when spangap-lcd is staged (conditional/spangap-lcd/), invoked
 * via the when:-gated netLcdRegister init hook.
 *
 * The WiFi pane mirrors the browser NetworkPanel's core capability: enable +
 * live status, an editable list of known networks (join / delete), a scan that
 * lists nearby networks to pick from, and SSID/password entry — so a device with
 * no browser can still get onto a network. The array work goes through the same
 * paths as the browser: `wifi.connect=<idx>` to join a known net, and the
 * `wifi.cmd.add`/`wifi.cmd.del` sentinels (handled in net.cpp's task loop) to
 * add/remove, so both surfaces drive one set of settings. The Access Point block
 * exposes the enable switch + timed window (both faces of ap.active_for) and the
 * AP's ssid/pass/ip/mask. Per-network static-IP / custom-MAC editing stays
 * browser-only (an accepted exception).
 */
#include "lcd.h"
#include "storage.h"

#include <string>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>

namespace {

/* Rebuilt-on-change containers + the entry fields. Nulled on pane delete so a
 * late storage callback can't touch freed objects. */
lv_obj_t* s_knownBox = nullptr;
lv_obj_t* s_scanBox  = nullptr;
lv_obj_t* s_ssidTa   = nullptr;
lv_obj_t* s_passTa   = nullptr;
bool      s_subscribed = false;

std::string arrField(const char* prefix, int idx, const char* field) {
  char k[80];
  snprintf(k, sizeof(k), "%s.%d.%s", prefix, idx, field);
  return storageGetStr(k, "");
}

lv_obj_t* mkRow(lv_obj_t* parent) {
  lv_obj_t* r = lv_obj_create(parent);
  lv_obj_remove_style_all(r);
  lv_obj_set_width(r, lv_pct(100));
  lv_obj_set_height(r, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_hor(r, 8, 0);
  lv_obj_set_style_pad_ver(r, 3, 0);
  lv_obj_set_style_pad_column(r, 6, 0);
  lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
  return r;
}

lv_obj_t* mkBtn(lv_obj_t* parent, const char* txt, lv_event_cb_t cb, void* ud) {
  lv_obj_t* b = lv_button_create(parent);
  lv_obj_set_style_pad_hor(b, 8, 0);
  lv_obj_set_style_pad_ver(b, 4, 0);
  lv_obj_t* l = lv_label_create(b);
  lv_label_set_text(l, txt);
  lv_obj_center(l);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
  if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), b);
  return b;
}

void onJoinKnown(lv_event_t* e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  char v[8]; snprintf(v, sizeof(v), "%d", idx);
  storageSet("wifi.connect", v);
}
void onDeleteKnown(lv_event_t* e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  char v[8]; snprintf(v, sizeof(v), "%d", idx);
  storageSet("wifi.cmd.del", v);
}
void onScan(lv_event_t*) { storageSet("wifi.scan", "1"); }
void onPickScanned(lv_event_t* e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  std::string ssid = arrField("wifi.scanned", idx, "ssid");
  if (s_ssidTa && !ssid.empty()) {
    lv_textarea_set_text(s_ssidTa, ssid.c_str());
    if (s_passTa && lcdInputGroup()) lv_group_focus_obj(s_passTa);
  }
}
void onJoinNew(lv_event_t*) {
  if (!s_ssidTa) return;
  std::string ssid = lv_textarea_get_text(s_ssidTa);
  std::string pass = s_passTa ? lv_textarea_get_text(s_passTa) : "";
  if (ssid.empty()) return;
  std::string payload = ssid + "\t" + pass;
  storageSet("wifi.cmd.add", payload.c_str());
  lv_textarea_set_text(s_ssidTa, "");
  if (s_passTa) lv_textarea_set_text(s_passTa, "");
}

/* ---- Access Point enable + timed window ----
 * One key encodes both: s.net.wifi.ap.active_for is -1 when the AP is disabled,
 * 0 for "up until a known network appears", N>0 for "off after N idle seconds,
 * once per boot". The generic 0/1 lcdSettingSwitch can't bind that, so the
 * switch maps enabled <-> active_for >= 0 (enabling writes 300, mirroring the
 * browser NetworkPanel), and the seconds field plus the ssid/pass/ip/mask rows
 * hide while disabled. */
lv_obj_t* s_apSwitch  = nullptr;
lv_obj_t* s_apSecsTa  = nullptr;
lv_obj_t* s_apRows[6] = {};

void apApply() {
  if (!s_apSwitch) return;
  int af = storageGetInt("s.net.wifi.ap.active_for", 300);
  bool on = af >= 0;
  if (on) lv_obj_add_state(s_apSwitch, LV_STATE_CHECKED);
  else    lv_obj_remove_state(s_apSwitch, LV_STATE_CHECKED);
  /* Don't clobber the field mid-edit (same rule as the lcdSettingText bind). */
  if (on && s_apSecsTa && !(lv_obj_get_state(s_apSecsTa) & LV_STATE_FOCUSED)) {
    char b[12];
    snprintf(b, sizeof(b), "%d", af);
    lv_textarea_set_text(s_apSecsTa, b);
  }
  for (lv_obj_t* r : s_apRows) {
    if (!r) continue;
    if (on) lv_obj_remove_flag(r, LV_OBJ_FLAG_HIDDEN);
    else    lv_obj_add_flag   (r, LV_OBJ_FLAG_HIDDEN);
  }
}

void onApToggle(lv_event_t* e) {
  lv_obj_t* sw = static_cast<lv_obj_t*>(lv_event_get_target_obj(e));
  storageSet("s.net.wifi.ap.active_for",
             lv_obj_has_state(sw, LV_STATE_CHECKED) ? 300 : -1);
}

void onApSecsCommit(lv_event_t* e) {
  lv_obj_t* ta = static_cast<lv_obj_t*>(lv_event_get_target(e));
  int v = atoi(lv_textarea_get_text(ta));
  storageSet("s.net.wifi.ap.active_for", v < 0 ? 0 : v);
}

void onApStorage(const char*, const char*) {
  lcdRun(ON_LCD { apApply(); });
}

lv_obj_t* mkField(lv_obj_t* parent, const char* placeholder, bool password, lv_obj_t** slot) {
  lv_obj_t* ta = lv_textarea_create(parent);
  lv_textarea_set_one_line(ta, true);
  lv_textarea_set_password_mode(ta, password);
  lv_textarea_set_placeholder_text(ta, placeholder);
  lv_obj_set_width(ta, lv_pct(100));
  if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), ta);
  lv_obj_add_event_cb(ta, onJoinNew, LV_EVENT_READY, nullptr);   /* Enter = join */
  *slot = ta;
  return ta;
}

void rebuildKnown() {
  if (!s_knownBox) return;
  lv_obj_clean(s_knownBox);
  int n = storageArrayCount("s.net.wifi.nets");
  if (n <= 0) {
    lv_obj_t* l = lv_label_create(s_knownBox);
    lv_label_set_text(l, "  (none saved)");
    lv_obj_set_style_text_color(l, lv_color_hex(0x8a93a0), 0);
    return;
  }
  std::string cur = storageGetStr("wifi.sta.ssid", "");
  for (int i = 0; i < n; i++) {
    std::string ssid = arrField("s.net.wifi.nets", i, "ssid");
    if (ssid.empty()) continue;
    lv_obj_t* r = mkRow(s_knownBox);
    lv_obj_t* lbl = lv_label_create(r);
    lv_label_set_text(lbl, (ssid + (ssid == cur ? "  (connected)" : "")).c_str());
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);   /* readable on dark bg */
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(lbl, 1);
    mkBtn(r, "Join", onJoinKnown, (void*)(intptr_t)i);
    mkBtn(r, LV_SYMBOL_TRASH, onDeleteKnown, (void*)(intptr_t)i);
  }
}

void rebuildScan() {
  if (!s_scanBox) return;
  lv_obj_clean(s_scanBox);
  int n = storageArrayCount("wifi.scanned");
  if (n <= 0) {
    lv_obj_t* l = lv_label_create(s_scanBox);
    lv_label_set_text(l, "  tap Scan to list networks");
    lv_obj_set_style_text_color(l, lv_color_hex(0x8a93a0), 0);
    return;
  }
  for (int i = 0; i < n; i++) {
    std::string ssid = arrField("wifi.scanned", i, "ssid");
    if (ssid.empty()) continue;
    std::string rssi = arrField("wifi.scanned", i, "rssi");
    std::string lock = arrField("wifi.scanned", i, "locked");
    lv_obj_t* r = mkRow(s_scanBox);
    lv_obj_t* lbl = lv_label_create(r);
    std::string txt = ssid + (lock == "1" ? "  *" : "") + (rssi.empty() ? "" : ("  " + rssi));
    lv_label_set_text(lbl, txt.c_str());
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);   /* readable on dark bg */
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(lbl, 1);
    mkBtn(r, "Pick", onPickScanned, (void*)(intptr_t)i);
  }
}

/* Storage-change handler: WiFi nets / scan results / live SSID feed the dynamic
 * lists. Hop onto the lcd task to touch LVGL safely regardless of which task the
 * storage actor dispatches from. Guarded by the nulled-on-delete pointers. */
void onWifiStorage(const char* /*key*/, const char* /*val*/) {
  lcdRun(ON_LCD {
    rebuildKnown();
    rebuildScan();
  });
}

void onPaneDelete(lv_event_t*) {
  s_knownBox = s_scanBox = s_ssidTa = s_passTa = nullptr;
  s_apSwitch = s_apSecsTa = nullptr;
  memset(s_apRows, 0, sizeof(s_apRows));
  if (s_subscribed) {
    /* These prefixes are ours alone (no lcdSetting* binding uses them), so a
     * scope unsubscribe is safe. We deliberately do NOT subscribe wifi.sta.ssid
     * here — a lcdSettingValue binding owns that key, and unsubscribing by scope
     * would take its callback down too; the "(connected)" marker is computed at
     * build time instead. */
    storageUnsubscribe("s.net.wifi.nets");
    storageUnsubscribe("wifi.scanned");
    storageUnsubscribe("s.net.wifi.ap.active_for");
    storageSet("wifi.scan", "0");   /* stop the periodic scan when leaving */
    s_subscribed = false;
  }
}

void wifiSettingsPane(void* arg) {
  lv_obj_t* p = static_cast<lv_obj_t*>(arg);

  lcdSettingSection(p, "WiFi");
  lcdSettingSwitch (p, "Enable", "s.net.wifi.enable");
  lcdSettingValue  (p, "Status", "wifi.sta.state");   /* live, ~1 Hz */
  lcdSettingValue  (p, "SSID",   "wifi.sta.ssid");
  lcdSettingValue  (p, "IP",     "wifi.sta.ip");
  lcdSettingValue  (p, "Signal", "wifi.sta.rssi");

  lcdSettingSection(p, "Saved networks");
  s_knownBox = lv_obj_create(p);
  lv_obj_remove_style_all(s_knownBox);
  lv_obj_set_width(s_knownBox, lv_pct(100));
  lv_obj_set_height(s_knownBox, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(s_knownBox, LV_FLEX_FLOW_COLUMN);
  lv_obj_remove_flag(s_knownBox, LV_OBJ_FLAG_SCROLLABLE);

  lcdSettingSection(p, "Add a network");
  mkField(p, "SSID", false, &s_ssidTa);
  mkField(p, "Password", true, &s_passTa);
  mkBtn(p, "Join", onJoinNew, nullptr);

  lcdSettingSection(p, "Nearby");
  mkBtn(p, "Scan", onScan, nullptr);
  s_scanBox = lv_obj_create(p);
  lv_obj_remove_style_all(s_scanBox);
  lv_obj_set_width(s_scanBox, lv_pct(100));
  lv_obj_set_height(s_scanBox, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(s_scanBox, LV_FLEX_FLOW_COLUMN);
  lv_obj_remove_flag(s_scanBox, LV_OBJ_FLAG_SCROLLABLE);

  lcdSettingSection(p, "Access Point");
  {
    /* Label layout mirrors lcd_settings' rows: 1/3 right-aligned label column. */
    lv_obj_t* r = mkRow(p);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t* lbl = lv_label_create(r);
    lv_label_set_text(lbl, "Enable");
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_width(lbl, lv_pct(33));
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_RIGHT, 0);
    s_apSwitch = lv_switch_create(r);
    lv_obj_set_size(s_apSwitch, 36, 18);
    lv_obj_set_style_bg_color(s_apSwitch, lv_color_hex(0x3a4150), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_apSwitch, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_bg_color(s_apSwitch, lv_color_hex(0x2563a0),
        (lv_style_selector_t)LV_PART_INDICATOR | (lv_style_selector_t)LV_STATE_CHECKED);
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), s_apSwitch);
    lv_obj_add_event_cb(s_apSwitch, onApToggle, LV_EVENT_VALUE_CHANGED, nullptr);
  }
  {
    lv_obj_t* r = mkRow(p);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t* lbl = lv_label_create(r);
    lv_label_set_text(lbl, "Active for (s)");
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_width(lbl, lv_pct(33));
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_RIGHT, 0);
    s_apSecsTa = lv_textarea_create(r);
    lv_textarea_set_one_line(s_apSecsTa, true);
    lv_textarea_set_accepted_chars(s_apSecsTa, "0123456789");
    lv_obj_set_flex_grow(s_apSecsTa, 1);
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), s_apSecsTa);
    lv_obj_add_event_cb(s_apSecsTa, onApSecsCommit, LV_EVENT_READY,     nullptr);
    lv_obj_add_event_cb(s_apSecsTa, onApSecsCommit, LV_EVENT_DEFOCUSED, nullptr);
    s_apRows[0] = r;
  }
  s_apRows[1] = lcdSettingCaption(p,
      "0: up until a known network is found. Otherwise the AP shuts down "
      "after this many seconds without traffic, once per boot.");
  s_apRows[2] = lcdSettingText(p, "Name",     "s.net.wifi.ap.ssid");
  s_apRows[3] = lcdSettingText(p, "Password", "s.net.wifi.ap.pass");
  s_apRows[4] = lcdSettingText(p, "IP",       "s.net.wifi.ap.ip");
  s_apRows[5] = lcdSettingText(p, "Netmask",  "s.net.wifi.ap.mask");

  rebuildKnown();
  rebuildScan();
  apApply();

  if (!s_subscribed) {
    storageSubscribeChanges("s.net.wifi.nets", onWifiStorage);
    storageSubscribeChanges("wifi.scanned",    onWifiStorage);
    storageSubscribeChanges("s.net.wifi.ap.active_for", onApStorage);
    s_subscribed = true;
  }
  /* Tear the subscription down (and stop scanning) when the pane is destroyed. */
  lv_obj_add_event_cb(p, onPaneDelete, LV_EVENT_DELETE, nullptr);
}

}  // namespace

/* Register the on-device Net settings panes — a when:-gated init: hook
 * (spangap/spangap-lcd). Plain C++ linkage to match the generated dispatcher's
 * forward decl. */
void netLcdRegister(void) {
  lcdRegisterSettings("Internet/WiFi", "WiFi", wifiSettingsPane, 1);
}
