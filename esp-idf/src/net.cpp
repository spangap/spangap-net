/**
 * Net — the WiFi state machine, and the link backend the relay runs on.
 *
 * WiFi: scan/connect/AP state machine (wifi1–wifi9 STA, wifi0 AP fallback).
 * The sockets, the event bus and the byte proxy are net_relay.cpp's; this file
 * owns the radio, the task that drives the relay's loop, and the `net` CLI.
 */
#include "net_priv.h"
#include "spangap.h"
#include "mem.h"
#include "storage.h"
#include "compat.h"
#include "pm.h"
#include "log.h"
#include "cli.h"
#include "fs.h"
#include "its.h"
#include "tls.h"
#include "auth.h"
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <fcntl.h>
#include "esp_wifi.h"
#include "esp_sleep.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"   /* esp_netif_get_netif_impl → lwIP struct netif */
#include "esp_event.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"            /* struct netif + input/linkoutput fn pointers */
#include <freertos/semphr.h>
#include <cstring>
#include <cstdio>
#include <string>
#include <map>
#include <vector>
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_mac.h"
#include <atomic>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <memory>
#include <sys/time.h>
#include <cJSON.h>
#include "lwip/inet.h"
#include "lwip/inet_chksum.h"
#include "lwip/prot/icmp.h"
#include "lwip/prot/ip.h"
#include "lwip/prot/ip4.h"

#define MAX_STA_NETWORKS 9

/* A known network that's visible in the scan but won't complete a connection
 * gets this many connect attempts before we give up on it and fall back to the
 * built-in AP. Without this a single failed association (which can take the
 * full connect timeout) immediately drops us to AP. */
#define WIFI_CONNECT_RETRIES 3

/* Scans per search window: a scan that misses gets exactly one more try
 * (a second scan catches APs the first one's channel dwell missed) before
 * falling back to AP / radio-off. Replaces the old s.net.wifi.timeout
 * time-based window, which kept the radio scanning for 20s per cycle. */
#define WIFI_SCANS_PER_CYCLE 2

static TaskHandle_t netHandle = nullptr;
static SemaphoreHandle_t readySem = nullptr;

/* esp_netif handles */
static esp_netif_t* sta_netif = nullptr;
static esp_netif_t* ap_netif = nullptr;

/* Event-driven connection signaling */
static SemaphoreHandle_t wifiConnectedSem = nullptr;
static volatile bool staConnected = false;

/* An IPv6 address reached VALID (GOT_IP6) — the net task republishes the
 * wifi.sta.ip6* keys on its next pass instead of waiting for the 30 s tick.
 * SLAAC runs on the router's schedule, well after the v4 bring-up publish. */
static volatile bool ip6Dirty = false;

/* Set once per boot when sys.boot_complete fires (after every straddle's init
 * hook and the boot script). The net task spins on this — pumping itsPoll so
 * the subscription callback can actually run — before the initial radio
 * bring-up: WiFi's driver control structs are heap-allocated in PSRAM, and a
 * flash program/erase during the boot storm suspends the cache mid-write and
 * corrupts them, later surfacing as a StoreProhibited in wifi_nvs_set /
 * ieee80211_send_setup on ppTask. */
static volatile bool s_bootComplete = false;
static volatile bool cmdUp = false;
static volatile bool cmdDown = false;
static volatile bool cmdForceDown = false;
static uint32_t connectTimeMs = 0;
#define WIFI_IDLE_TIMEOUT_MS 30000

/* ITS aux command interface */
enum { NET_CMD_UP = 1, NET_CMD_DOWN, NET_CMD_FORCE_DOWN, NET_CMD_CONNECT, NET_CMD_DISCONNECT,
       NET_CMD_WIFI_ADD, NET_CMD_WIFI_DEL, NET_CMD_SCAN };
static volatile int cmdConnectIdx = -1;  /* target network index for NET_CMD_CONNECT */
/* On-device WiFi add/delete sentinels (LCD WiFi pane). Captured once by the
 * wifi.cmd.* subscribers, applied in the net task loop (see netTaskFn). */
static volatile bool cmdWifiAdd = false;
static char          cmdWifiAddBuf[96];
static volatile bool cmdWifiDel = false;
static volatile int  cmdWifiDelIdx = -1;
/* The settings collection's sentinels (wifi.net.*). Their subscribers run on
 * this task — they are registered from it — but they only RAISE a flag: the
 * payload stays in storage and the loop reads it at the top of the next pass,
 * so an array rewrite never happens inside a storage-notification callback and
 * nothing has to squeeze a JSON object through an ITS aux frame. */
static volatile bool cmdNetAdd = false, cmdNetSet = false;
static volatile bool cmdNetRemove = false, cmdNetOrder = false, cmdNetConnect = false;

/* RTC state: survives deep sleep. On cold boot, boot file runs "net up". */
RTC_DATA_ATTR static bool rtcWantUp = false;

/* The master off switch s.net.wifi.enable gates EVERY "should we be up?" check —
 * boot bring-up, scan, AP-retry. rtcWantUp is only a runtime cache (it can drift:
 * preserved across resets, set by net up/down), so gating on it alone let a stale
 * rtcWantUp=1 bring the radio up against an explicit enable=0. enable is the
 * persistent truth, so it wins here unconditionally. */
static bool wantUp() { return rtcWantUp && storageGetInt("s.net.wifi.enable", 1) != 0; }

static pm_lock_handle_t netDeepLock = nullptr;

/* ---- The relay's endpoint table, event bus and byte proxy are
 *      net_relay.cpp's; this file drives them. ---- */

/* ---- WiFi event handler ---- */

static void wifi_event_handler(void* arg, esp_event_base_t base,
                               int32_t id, void* data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
    staConnected = false;
  else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
    /* Kick off IPv6: forms the link-local now and, with SLAAC
     * (CONFIG_LWIP_IPV6_AUTOCONFIG), a global address from router RAs — needed
     * to reach v6-only hosts like play.rop.nl off-VPN. */
    esp_netif_create_ip6_linklocal(sta_netif);
  }
  else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    staConnected = true;
    xSemaphoreGive(wifiConnectedSem);
  }
  else if (base == IP_EVENT && id == IP_EVENT_GOT_IP6) {
    /* esp_netif re-posts GOT_IP6 on every state transition into VALID (ND6
     * re-validates the link-local ~1 Hz while soliciting a router), so log only
     * when the address actually changes to avoid a heartbeat of duplicates. */
    ip_event_got_ip6_t* e = (ip_event_got_ip6_t*)data;
    static esp_ip6_addr_t lastLogged = {};
    if (memcmp(&lastLogged.addr, &e->ip6_info.ip.addr, sizeof(lastLogged.addr)) == 0)
      return;
    lastLogged = e->ip6_info.ip;
    ip6Dirty = true;
    const char* scope;
    switch (esp_netif_ip6_get_addr_type(&e->ip6_info.ip)) {
      case ESP_IP6_ADDR_IS_GLOBAL:       scope = "global";     break;
      case ESP_IP6_ADDR_IS_LINK_LOCAL:   scope = "link-local"; break;
      case ESP_IP6_ADDR_IS_SITE_LOCAL:   scope = "site-local"; break;
      case ESP_IP6_ADDR_IS_UNIQUE_LOCAL: scope = "unique-local"; break;
      default:                           scope = "other";      break;
    }
    info("net: IPv6 %s " IPV6STR "\n", scope, IPV62STR(e->ip6_info.ip));
  }
}

/* ---- WiFi helpers ---- */

#if CONFIG_ESP_WIFI_REMOTE_ENABLED
/* A co-processor (esp_wifi_remote) can report the radio started twice for one
 * start, and IDF's default handler attaches the netif to lwIP on every START —
 * lwIP asserts on the second. A START for a netif already attached detaches it
 * first, so the default handler's attach that follows is a clean re-attach. */
static bool s_staAttached = false, s_apAttached = false;

static void remoteStartDedupe(void*, esp_event_base_t, int32_t id, void*) {
  switch (id) {
    case WIFI_EVENT_STA_START:
      if (s_staAttached && sta_netif) esp_netif_action_stop(sta_netif, nullptr, 0, nullptr);
      s_staAttached = true;
      break;
    case WIFI_EVENT_STA_STOP: s_staAttached = false; break;
    case WIFI_EVENT_AP_START:
      if (s_apAttached && ap_netif) esp_netif_action_stop(ap_netif, nullptr, 0, nullptr);
      s_apAttached = true;
      break;
    case WIFI_EVENT_AP_STOP:  s_apAttached = false; break;
    default: break;
  }
}
#endif

static void wifiNetifInit() {
  /* esp_wifi warns about this on every single STA config write, and it is not a
     warning: a WPA2-length passphrase raising the authmode threshold off OPEN is
     what we asked for by having a passphrase at all. Keep the line, at the level
     it is worth. */
  logRule("Password length matches WPA2 standards", 'I');

  esp_netif_init();
  esp_event_loop_create_default();
#if CONFIG_ESP_WIFI_REMOTE_ENABLED
  /* Ahead of the defaults the next two calls register, so it runs first. */
  esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &remoteStartDedupe, nullptr);
#endif
  sta_netif = esp_netif_create_default_wifi_sta();
  ap_netif  = esp_netif_create_default_wifi_ap();
  esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr);
  esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, nullptr);
  esp_event_handler_register(IP_EVENT, IP_EVENT_GOT_IP6, &wifi_event_handler, nullptr);
}

static void wifiHwStart(wifi_mode_t mode = WIFI_MODE_STA) {
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  esp_wifi_init(&cfg);
  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  esp_wifi_set_mode(mode);
  esp_wifi_start();
  esp_sleep_enable_wifi_wakeup();
}

/** Number of configured STA networks (from s.net.wifi.nets array). */
static int staNetCount() { return storageArrayCount("s.net.wifi.nets"); }

/** Read a field from s.net.wifi.nets[idx]. */
static void staNetGet(int idx, const char* field, char* out, size_t len, const char* def = "") {
  char key[48];
  snprintf(key, sizeof(key), "s.net.wifi.nets.%d.%s", idx, field);
  storageGetStr(key, out, len, def);
}

static int staNetFindBySsid(const char* ssid);   /* defined below; used by the task loop */

/** Remove known network [idx], shifting later entries down and dropping the
 *  tail — the array-correct delete (a bare delete of an index would leave a
 *  hole). Shared by the `net delete` CLI verb, the wifi.cmd.del sentinel and
 *  the settings collection's wifi.net.remove. */
/* The fields one known network carries. `id` is a small opaque number this
 * task hands out and never reuses while an entry lives: the settings collection
 * addresses items by it, and it has to survive the reorders and deletes that
 * shuffle every index. */
static const char* const STA_NET_FIELDS[] =
    { "id", "ssid", "pass", "ip", "gw", "mask", "dns", "mac" };

static void staNetDeleteIdx(int idx) {
  int total = staNetCount();
  if (idx < 0 || idx >= total) return;
  storageBegin();
  for (int i = idx; i < total - 1; i++) {
    char src[64], dst[64];
    for (const char* f : STA_NET_FIELDS) {
      snprintf(src, sizeof(src), "s.net.wifi.nets.%d.%s", i + 1, f);
      snprintf(dst, sizeof(dst), "s.net.wifi.nets.%d.%s", i, f);
      std::string v = storageGetStr(src, "");
      if (v.empty()) storageUnset(dst);
      else           storageSet(dst, v.c_str());
    }
  }
  char tail[64];
  snprintf(tail, sizeof(tail), "s.net.wifi.nets.%d", total - 1);
  storageDeleteTree(tail);
  storageEnd();
}

/* ---- the known-networks collection ----
 *
 * The settings surfaces never write s.net.wifi.nets. They write wifi.net.add /
 * .remove / .set / .order and this task applies them, which is what lets one
 * description drive the browser and the display and keeps validation in exactly
 * one place. A rejection is a sentence on wifi.net.error.
 *
 * Everything below runs on the net task (the sentinels forward over ITS like
 * connect does), so array writes never happen on the storage actor. */

static std::string staNetField(int idx, const char* field) {
  char key[64];
  snprintf(key, sizeof(key), "s.net.wifi.nets.%d.%s", idx, field);
  return storageGetStr(key, "");
}

static int staNetFindById(const char* id) {
  if (!id || !*id) return -1;
  int n = staNetCount();
  for (int i = 0; i < n; i++) if (staNetField(i, "id") == id) return i;
  return -1;
}

/** The next unused id. Ids are only ever compared, so a running maximum is
 *  enough and nothing has to remember what was handed out before. */
static std::string staNetNextId() {
  int best = 0, n = staNetCount();
  for (int i = 0; i < n; i++) {
    int v = atoi(staNetField(i, "id").c_str());
    if (v > best) best = v;
  }
  char buf[12];
  snprintf(buf, sizeof(buf), "%d", best + 1);
  return buf;
}

/** Give every entry an id, for a store written before ids existed. Idempotent. */
static void staNetEnsureIds() {
  int n = staNetCount();
  bool any = false;
  for (int i = 0; i < n; i++) if (staNetField(i, "id").empty()) { any = true; break; }
  if (!any) return;
  storageBegin();
  for (int i = 0; i < n; i++) {
    if (!staNetField(i, "id").empty()) continue;
    char k[64], v[12];
    snprintf(v, sizeof(v), "%d", i + 1);
    snprintf(k, sizeof(k), "s.net.wifi.nets.%d.id", i);
    storageSet(k, v);
  }
  storageEnd();
}

static void staNetError(const char* why) { storageSet("wifi.net.error", why); }

/** Accepted-mutation ack, shared by every wifi.net.* sentinel: the open form
 *  closes when this moves. A monotonic per-boot counter, not a read-increment —
 *  reads see the committed tree, and the actor may not have applied the
 *  previous bump yet. */
static void staNetAck() {
  static int ack = 0;
  storageSet("wifi.net.done", ++ack);
}

/** What is wrong with this network, or "" if nothing is. The one place that
 *  decides — an add form and an item editor both land here, and neither carries
 *  a rule of its own. */
static std::string staNetRejection(const std::string& ssid, const std::string& ip,
                                   const std::string& mask) {
  if (ssid.empty())      return "A network needs an SSID.";
  if (ssid.size() > 32)  return "An SSID is at most 32 characters.";
  /* A manual address is all-or-nothing: an IP without a netmask cannot be
   * turned into a route, and the radio would associate and then go nowhere. */
  if (!ip.empty() && mask.empty()) return "A manual IP needs a netmask.";
  if (ip.empty() && !mask.empty()) return "A netmask without an IP does nothing.";
  return "";
}

/** Write one entry's fields at `idx`, dropping the empty ones so an absent
 *  field stays absent rather than becoming "". Caller holds the transaction. */
static void staNetWrite(int idx, const std::string& id,
                        const std::map<std::string, std::string>& fields) {
  char k[64];
  snprintf(k, sizeof(k), "s.net.wifi.nets.%d.id", idx);
  storageSet(k, id.c_str());
  for (const char* f : STA_NET_FIELDS) {
    if (strcmp(f, "id") == 0) continue;
    snprintf(k, sizeof(k), "s.net.wifi.nets.%d.%s", idx, f);
    auto it = fields.find(f);
    if (it == fields.end() || it->second.empty()) storageUnset(k);
    else                                          storageSet(k, it->second.c_str());
  }
}

/** The fields of a `wifi.net.add` / `.set` payload, which is the form's field
 *  object as JSON. Only members the payload actually carries land in the map, so
 *  a caller can tell "left blank" (present, empty — erase it) from "not offered"
 *  (absent), which `staNetSetJson` carries forward. */
static std::map<std::string, std::string> staNetParse(const char* json, std::string* idOut) {
  std::map<std::string, std::string> out;
  cJSON* o = cJSON_Parse(json);
  if (!o) return out;
  for (const char* f : STA_NET_FIELDS) {
    cJSON* m = cJSON_GetObjectItem(o, f);
    if (cJSON_IsString(m)) out[f] = m->valuestring;
  }
  cJSON* id = cJSON_GetObjectItem(o, "_id");
  if (idOut && cJSON_IsString(id)) *idOut = id->valuestring;
  cJSON_Delete(o);
  return out;
}

/** Add a network from a form payload, or say why not. An SSID already known is
 *  an update of that entry rather than a second copy of it — which is also what
 *  makes adopting a scanned network idempotent. */
static void staNetAddJson(const char* json) {
  auto f = staNetParse(json, nullptr);
  std::string why = staNetRejection(f["ssid"], f["ip"], f["mask"]);
  if (!why.empty()) { staNetError(why.c_str()); return; }
  int idx = staNetFindBySsid(f["ssid"].c_str());
  std::string id = (idx >= 0) ? staNetField(idx, "id") : staNetNextId();
  if (idx < 0) idx = staNetCount();
  storageBegin();
  staNetWrite(idx, id, f);
  staNetError("");
  storageEnd();
  staNetAck();
  info("added '%s' at %d\n", f["ssid"].c_str(), idx);
  /* Adding a network is asking to be on it. */
  char v[8];
  snprintf(v, sizeof(v), "%d", idx);
  storageSet("wifi.connect", v);
}

/** Commit an item editor's fields against the entry it names.
 *
 *  ABSENT is not EMPTY. A field the editor carries and left blank arrives as an
 *  empty string and erases what was there — that is how a fixed IP is handed
 *  back to DHCP. A field the editor does not carry at all is not being edited,
 *  so it is filled in from the entry before anything is judged or written. The
 *  detail page shows the SSID in its heading rather than as a row, and without
 *  this every Save from it would erase the SSID and then be rejected for having
 *  none. */
static void staNetSetJson(const char* json) {
  std::string id;
  auto f = staNetParse(json, &id);
  int idx = staNetFindById(id.c_str());
  if (idx < 0) { staNetError("That network is no longer configured."); return; }
  for (const char* fld : STA_NET_FIELDS)
    if (strcmp(fld, "id") != 0 && f.find(fld) == f.end())
      f[fld] = staNetField(idx, fld);
  std::string why = staNetRejection(f["ssid"], f["ip"], f["mask"]);
  if (!why.empty()) { staNetError(why.c_str()); return; }
  storageBegin();
  staNetWrite(idx, id, f);
  staNetError("");
  storageEnd();
  staNetAck();
}

/** Apply an id order as a PREFERENCE PERMUTATION: recognized ids move into the
 *  stated relative order, unknown ids are ignored, and anything the payload
 *  never mentions keeps its place. That is what makes a drag idempotent and
 *  harmless against an add or delete that raced it. */
static void staNetOrder(const char* csv) {
  int n = staNetCount();
  if (n <= 1) return;
  std::vector<std::string> wanted;
  for (const char* p = csv; p && *p; ) {
    const char* comma = strchr(p, ',');
    wanted.push_back(comma ? std::string(p, comma - p) : std::string(p));
    if (!comma) break;
    p = comma + 1;
  }
  /* Snapshot every entry, then rebuild: the positions the payload mentions are
   * filled from `wanted` in order, the rest keep the slots they already had. */
  std::vector<std::map<std::string, std::string>> items(n);
  std::vector<std::string> ids(n);
  for (int i = 0; i < n; i++) {
    ids[i] = staNetField(i, "id");
    for (const char* f : STA_NET_FIELDS)
      if (strcmp(f, "id") != 0) items[i][f] = staNetField(i, f);
  }
  std::vector<int> slots;          /* the positions being permuted */
  std::vector<int> order;          /* which entry lands in each of them */
  for (int i = 0; i < n; i++)
    for (const std::string& w : wanted)
      if (ids[i] == w) { slots.push_back(i); break; }
  for (const std::string& w : wanted)
    for (int i = 0; i < n; i++)
      if (ids[i] == w) { order.push_back(i); break; }
  if (slots.size() != order.size() || slots.empty()) return;
  storageBegin();
  for (size_t s = 0; s < slots.size(); s++)
    staNetWrite(slots[s], ids[order[s]], items[order[s]]);
  staNetError("");
  storageEnd();
  staNetAck();
}

/* Access points seen this boot, one record per SSID. The cache accumulates
 * across ALL scans — a network that only shows up in a later scan still lands
 * here, so the environment fills in over time rather than being whatever the
 * most recent single scan happened to see. Reset only by a real reboot.
 *
 * `named` carries the other half: each network is announced at info via
 * `scan found` exactly once, so the log names every network it ever sees
 * without repeating itself every scan cycle.
 *
 * `net scan` prints this cache; it starts no scan of its own. */
#define SCAN_CACHE_MAX 64
struct scan_seen_t {
  char   ssid[33];
  int8_t rssi;      /* loudest this network has been, not the latest */
  bool   open;
  bool   named;     /* `scan found` already logged for it this boot */
};
static scan_seen_t scanSeen[SCAN_CACHE_MAX];
/* Volatile because it publishes: the net task appends records and `net scan`
 * reads them from the CLI task. An entry is filled in completely before the
 * count that exposes it is raised, so a reader never sees a half-written SSID.
 * Nothing stronger is needed — the only other cross-task write is an int8 RSSI
 * on an entry that is already published, where a stale read costs a dBm. */
static volatile int scanSeenCount = 0;

static scan_seen_t* scanSeenFind(const char* ssid) {
  for (int i = 0; i < scanSeenCount; i++)
    if (strcmp(scanSeen[i].ssid, ssid) == 0) return &scanSeen[i];
  return nullptr;
}

/* Record one sighting; nullptr only when the cache is full. The RSSI/open
 * update runs on every sighting — ahead of the `named` check, which skips
 * repeats outright — so a network that is louder in a later scan is remembered
 * at its best rather than at whatever the first scan happened to hear. */
static scan_seen_t* scanSeenNote(const char* ssid, int8_t rssi, bool open) {
  scan_seen_t* s = scanSeenFind(ssid);
  if (s) {
    if (rssi > s->rssi) s->rssi = rssi;
    s->open = open;
    return s;
  }
  if (scanSeenCount >= SCAN_CACHE_MAX) return nullptr;
  s = &scanSeen[scanSeenCount];
  safeStrncpy(s->ssid, ssid, sizeof(s->ssid));
  s->rssi  = rssi;
  s->open  = open;
  s->named = false;
  /* Last: the record is complete before it becomes visible. Spelled out rather
   * than `++` — that form is deprecated on a volatile. */
  scanSeenCount = scanSeenCount + 1;
  return s;
}

/* THE scan. Every scan this device runs comes through here — the connect
 * search, the browser's refresh beat, `net scan` — because a scan is one
 * sighting of the neighbourhood and there is only ever one of those. It feeds
 * the boot cache, publishes `wifi.scanned`, and answers the connect search's
 * question, so no two views of the radio's last look around can disagree.
 *
 * `wifi.scanned` is a JSON array, one element per network: {ssid, bssid, rssi,
 * locked} plus `name` and `detail` — the two lines a settings row shows,
 * rendered here. A settings surface should be able to list what the radio can
 * see without knowing that an empty SSID means a hidden network or which dBm
 * figures deserve which bars.
 *
 * `searching` says the scan belongs to a connect attempt, and governs LOGGING
 * only: the summary line explains a state transition there, and is noise on a
 * 20-second refresh beat. Returns the index of the first CONFIGURED network
 * present — configured order, not signal order: the stored list is a
 * preference, so the first one in range wins however loud the others are. */
static int wifiScanRun(bool searching) {
  esp_wifi_scan_stop();   /* drop any wedged/in-flight scan left by a prior attempt */
  wifi_scan_config_t scan_config = {};
  esp_err_t e = esp_wifi_scan_start(&scan_config, true);
  if (e != ESP_OK) { info("scan start failed: %s\n", esp_err_to_name(e)); return -1; }
  uint16_t ap_count = 0;
  esp_wifi_scan_get_ap_num(&ap_count);
  int nNets = staNetCount();
  if (ap_count == 0) {
    storageSetTree("wifi.scanned", cJSON_CreateArray());
    if (!searching)  dbg("0 Access Points found\n");
    else if (nNets)  info("0 Access Points found, none of our %d known networks in range\n", nNets);
    else             info("0 Access Points found\n");
    return -1;
  }
  wifi_ap_record_t* ap_list = (wifi_ap_record_t*)gp_alloc(ap_count * sizeof(wifi_ap_record_t));
  if (!ap_list) return -1;
  esp_wifi_scan_get_ap_records(&ap_count, ap_list);

  /* Loudest first, in place: the cache, the published list and the duplicate
   * collapse below all want the strongest AP of a network to be the one they
   * meet first — which is also what makes noting only the first sighting of an
   * SSID equivalent to noting them all and keeping the best. */
  for (int i = 0; i < ap_count - 1; i++)
    for (int j = i + 1; j < ap_count; j++)
      if (ap_list[j].rssi > ap_list[i].rssi) {
        wifi_ap_record_t tmp = ap_list[i]; ap_list[i] = ap_list[j]; ap_list[j] = tmp;
      }

  /* Hidden networks (empty SSID) collapse into one "unique SSID" for the
   * summary count — good enough for a count — but each keeps its own published
   * row, because they are distinct APs and nothing else tells them apart. */
  cJSON* arr = cJSON_CreateArray();
  int uniq = 0;
  for (int i = 0; i < ap_count; i++) {
    const wifi_ap_record_t& ap = ap_list[i];
    dbg("  '%s' ch%d %ddBm\n", (const char*)ap.ssid, ap.primary, ap.rssi);
    bool dup = false;
    for (int j = 0; j < i && !dup; j++)
      dup = strcmp((const char*)ap_list[j].ssid, (const char*)ap.ssid) == 0;
    if (!dup) uniq++;

    /* Announce each SSID not named yet this boot, at info so the environment
     * is visible without debug logging and every scan can add networks that
     * only just came into range. */
    if (ap.ssid[0] && !dup) {
      scan_seen_t* s = scanSeenNote((const char*)ap.ssid, ap.rssi,
                                    ap.authmode == WIFI_AUTH_OPEN);
      if (s && !s->named) {
        s->named = true;
        info("scan found \"%s\" %ddBm%s\n", s->ssid, s->rssi, s->open ? " open" : "");
      }
    }
    if (dup && ap.ssid[0]) continue;   /* published list: one row per named network */

    cJSON* obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "ssid", (const char*)ap.ssid);
    char bssid[18];
    snprintf(bssid, sizeof(bssid), "%02x:%02x:%02x:%02x:%02x:%02x",
             ap.bssid[0], ap.bssid[1], ap.bssid[2], ap.bssid[3], ap.bssid[4], ap.bssid[5]);
    cJSON_AddStringToObject(obj, "bssid", bssid);
    cJSON_AddNumberToObject(obj, "rssi", ap.rssi);
    bool locked = ap.authmode != WIFI_AUTH_OPEN;
    cJSON_AddNumberToObject(obj, "locked", locked ? 1 : 0);
    cJSON_AddStringToObject(obj, "name", ap.ssid[0] ? (const char*)ap.ssid : "(hidden)");
    int r = ap.rssi;
    const char* bars = r >= -55 ? "\xE2\x96\x82\xE2\x96\x84\xE2\x96\x86\xE2\x96\x88"
                     : r >= -65 ? "\xE2\x96\x82\xE2\x96\x84\xE2\x96\x86"
                     : r >= -75 ? "\xE2\x96\x82\xE2\x96\x84"
                                : "\xE2\x96\x82";
    char detail[48];
    snprintf(detail, sizeof(detail), "%s  %d dBm%s", bars, r, locked ? "  \xF0\x9F\x94\x92" : "");
    cJSON_AddStringToObject(obj, "detail", detail);
    cJSON_AddItemToArray(arr, obj);
  }
  storageSetTree("wifi.scanned", arr);

  int bestIdx = -1;
  char matched[33] = "";
  for (int s = 0; s < nNets && bestIdx < 0; s++) {
    char ssid[33];
    staNetGet(s, "ssid", ssid, sizeof(ssid));
    for (int i = 0; i < ap_count; i++)
      if (strcmp((const char*)ap_list[i].ssid, ssid) == 0) {
        bestIdx = s;
        safeStrncpy(matched, ssid, sizeof(matched));
        break;
      }
  }
  free(ap_list);
  if (!searching)
    dbg("%d different Access Points, %d unique SSIDs\n", ap_count, uniq);
  else if (bestIdx >= 0)
    info("%d different Access Points, %d unique SSIDs, found our known network '%s'\n",
         ap_count, uniq, matched);
  else if (nNets)
    info("%d different Access Points, %d unique SSIDs, none of our %d known networks found\n",
         ap_count, uniq, nNets);
  else
    info("%d different Access Points, %d unique SSIDs\n", ap_count, uniq);
  return bestIdx;
}

/* ---- WiFi state machine ---- */

enum wifi_state_t { ST_OFF, ST_SCANNING, ST_STA_CONNECTED, ST_AP };
static volatile wifi_state_t wifiState = ST_OFF;
/* netLinkUp / netUpstreamUp live with netRegister() in the relay, so it can
 * replay current state to late subscribers. */

static void staNetPublishStatus();

/** The STA netif's IPv6 addresses as display text: `ip6` gets the best
 *  non-link-local address (global scope preferred over unique-/site-local),
 *  `ll` the link-local. RFC 5952 compressed lowercase via inet_ntop. Empty
 *  string = absent — which is also the settings rows' hide gate. */
static void staIp6Strings(char* ip6, size_t ip6Len, char* ll, size_t llLen) {
  ip6[0] = '\0';
  ll[0] = '\0';
  if (!sta_netif) return;
  esp_ip6_addr_t addrs[CONFIG_LWIP_IPV6_NUM_ADDRESSES];
  int n = esp_netif_get_all_ip6(sta_netif, addrs);
  bool haveGlobal = false;
  for (int i = 0; i < n; i++) {
    char buf[INET6_ADDRSTRLEN];
    if (!inet_ntop(AF_INET6, addrs[i].addr, buf, sizeof(buf))) continue;
    switch (esp_netif_ip6_get_addr_type(&addrs[i])) {
      case ESP_IP6_ADDR_IS_LINK_LOCAL:
        safeStrncpy(ll, buf, llLen);
        break;
      case ESP_IP6_ADDR_IS_GLOBAL:
        safeStrncpy(ip6, buf, ip6Len);
        haveGlobal = true;
        break;
      case ESP_IP6_ADDR_IS_UNIQUE_LOCAL:
      case ESP_IP6_ADDR_IS_SITE_LOCAL:
        if (!haveGlobal) safeStrncpy(ip6, buf, ip6Len);
        break;
      default:
        break;
    }
  }
}

/** Publish current WiFi status as ephemeral keys.
 *  wifi.sta.state — "off", "connecting", "connected"
 *  wifi.sta.*     — station info (ssid, ip, router, etc.)
 *  wifi.ap.state  — "off", "active"
 *  wifi.ap.*      — access point info
 *  wifi.mac       — STA MAC address */
static void publishWifiStatus() {
  storageBegin();

  /* STA MAC address (always available — base MAC from efuse) */
  uint8_t mac[6];
  if (wifiState == ST_OFF || esp_wifi_get_mac(WIFI_IF_STA, mac) != ESP_OK)
    esp_efuse_mac_get_default(mac);
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  storageSet("wifi.mac", macStr);

  char inBuf[16], outBuf[16];
  fmtSize(netTrafficInBytes, inBuf, sizeof(inBuf));
  fmtSize(netTrafficOutBytes, outBuf, sizeof(outBuf));
  storageSet("wifi.traffic_in", inBuf);
  storageSet("wifi.traffic_out", outBuf);

  /* STA status */
  bool connecting = (wifiState == ST_SCANNING);
  storageSet("wifi.sta.state", wifiState == ST_STA_CONNECTED ? "connected"
                               : connecting ? "connecting" : "off");
  /* The same state as the words a settings row shows, and the traffic pair as
   * the one line it shows. Both surfaces render these verbatim. */
  storageSet("wifi.sta.state_text", wifiState == ST_STA_CONNECTED ? "Connected"
                                    : connecting ? "Connecting\xE2\x80\xA6" : "Off");
  char traffic[48];
  snprintf(traffic, sizeof(traffic), "in %s, out %s", inBuf, outBuf);
  storageSet("wifi.traffic", traffic);
  if (wifiState == ST_STA_CONNECTED) {
    wifi_ap_record_t ap_info = {};
    const char* ssid = "";
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) ssid = (const char*)ap_info.ssid;
    storageSet("wifi.sta.ssid", ssid);
    storageSet("wifi.sta.rssi", (int)ap_info.rssi);
    storageSet("wifi.sta.channel", (int)ap_info.primary);

    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(sta_netif, &ip_info);
    char ip[16], gw[16], mask[16];
    esp_ip4addr_ntoa(&ip_info.ip, ip, sizeof(ip));
    esp_ip4addr_ntoa(&ip_info.gw, gw, sizeof(gw));
    esp_ip4addr_ntoa(&ip_info.netmask, mask, sizeof(mask));
    storageSet("wifi.sta.ip", ip);
    storageSet("wifi.sta.router", gw);
    storageSet("wifi.sta.netmask", mask);

    esp_netif_dns_info_t dns1 = {};
    char dns1s[16];
    esp_netif_get_dns_info(sta_netif, ESP_NETIF_DNS_MAIN, &dns1);
    esp_ip4addr_ntoa(&dns1.ip.u_addr.ip4, dns1s, sizeof(dns1s));
    storageSet("wifi.sta.dns", dns1s);
    char ip6[INET6_ADDRSTRLEN], ip6ll[INET6_ADDRSTRLEN];
    staIp6Strings(ip6, sizeof(ip6), ip6ll, sizeof(ip6ll));
    storageSet("wifi.sta.ip6", ip6);
    storageSet("wifi.sta.ip6_ll", ip6ll);
    storageSet("wifi.sta.up", 1);
    /* Signal quality as a phrase. Which dBm counts as "Good" is a judgement
     * about this radio, so it is made here rather than by each surface guessing
     * the same thresholds. */
    int rssi = (int)ap_info.rssi;
    const char* quality = rssi >= -50 ? "Excellent" : rssi >= -60 ? "Good"
                        : rssi >= -70 ? "Fair" : "Weak";
    char sig[40];
    snprintf(sig, sizeof(sig), "%s (%d dBm)", quality, rssi);
    storageSet("wifi.sta.signal", sig);
  } else {
    storageSet("wifi.sta.signal", "");
    storageSet("wifi.sta.ssid", "");
    storageSet("wifi.sta.ip", "");
    storageSet("wifi.sta.router", "");
    storageSet("wifi.sta.netmask", "");
    storageSet("wifi.sta.dns", "");
    storageSet("wifi.sta.ip6", "");
    storageSet("wifi.sta.ip6_ll", "");
    storageSet("wifi.sta.rssi", 0);
    storageSet("wifi.sta.channel", 0);
    storageSet("wifi.sta.up", 0);
  }

  /* AP status — active in ST_AP or during APSTA transitions */
  wifi_mode_t mode = WIFI_MODE_NULL;
  if (wifiState != ST_OFF) esp_wifi_get_mode(&mode);
  bool apActive = (wifiState == ST_AP) || (mode == WIFI_MODE_APSTA);
  storageSet("wifi.ap.state", apActive ? "active" : "off");
  if (apActive) {
    char ssid[33];
    storageGetStr("s.net.wifi.ap.ssid", ssid, sizeof(ssid), WIFI_AP_SSID);
    storageSet("wifi.ap.ssid", ssid);

    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(ap_netif, &ip_info);
    char ip[16], mask[16];
    esp_ip4addr_ntoa(&ip_info.ip, ip, sizeof(ip));
    esp_ip4addr_ntoa(&ip_info.netmask, mask, sizeof(mask));
    storageSet("wifi.ap.ip", ip);
    storageSet("wifi.ap.netmask", mask);
    storageSet("wifi.ap.up", 1);
  } else {
    storageSet("wifi.ap.ssid", "");
    storageSet("wifi.ap.ip", "");
    storageSet("wifi.ap.netmask", "");
    storageSet("wifi.ap.up", 0);
  }
  /* The plain truthy gate for the rows that only matter while the access point
   * is on — published rather than left to a settings surface to derive, like
   * every other gate here. */
  storageSet("wifi.ap.enabled", storageGetInt("s.net.wifi.ap.enable", 1) ? 1 : 0);
  storageEnd();
  staNetPublishStatus();
}

/** One status pill per known network, as packed "text|colour", and beside it the
 *  gate that says whether joining it is a thing to offer. Which network is
 *  connected and which are merely in range is something only this task knows,
 *  and saying it in finished words means neither settings surface has to
 *  cross-reference the scan cache against the configured list.
 *
 *  The gate is published truthy for every network EXCEPT the one we are on, so
 *  the settings surfaces can hide a Connect button with a truthiness test and
 *  never a comparison. */
static bool scanCacheHas(const char* ssid) {
  return ssid && *ssid && scanSeenFind(ssid) != nullptr;
}

static void staNetPublishStatus() {
  int n = staNetCount();
  std::string connected;
  if (wifiState == ST_STA_CONNECTED) connected = storageGetStr("wifi.sta.ssid", "");
  storageBegin();
  for (int i = 0; i < n; i++) {
    std::string id   = staNetField(i, "id");
    std::string ssid = staNetField(i, "ssid");
    if (id.empty()) continue;
    bool onIt = !ssid.empty() && ssid == connected;
    const char* pill = "";
    if (onIt)                            pill = "connected|green";
    else if (scanCacheHas(ssid.c_str())) pill = "in range|blue";
    char k[64];
    snprintf(k, sizeof(k), "wifi.netstat.%s", id.c_str());
    storageSet(k, pill);
    snprintf(k, sizeof(k), "wifi.netjoinable.%s", id.c_str());
    storageSet(k, onIt ? 0 : 1);
  }
  storageEnd();
}

static bool connectSta(int idx) {
  char ssid[33], pass[65], ip[16], gw[16], mask[16], dns[16], macStr[18];
  staNetGet(idx, "ssid", ssid, sizeof(ssid));
  staNetGet(idx, "pass", pass, sizeof(pass));
  staNetGet(idx, "ip",   ip,   sizeof(ip));
  staNetGet(idx, "gw",   gw,   sizeof(gw));
  staNetGet(idx, "mask", mask, sizeof(mask));
  staNetGet(idx, "dns",  dns,  sizeof(dns));
  staNetGet(idx, "mac",  macStr, sizeof(macStr));
  info("connecting to '%s' pass(%d chars)\n", ssid, (int)strlen(pass));
  esp_wifi_disconnect();
  delay(100);
  esp_wifi_set_mode(WIFI_MODE_STA);

  /* Custom MAC: set if configured, restore default if a custom one was active */
  static bool customMacActive = false;
  if (macStr[0]) {
    uint8_t mac[6];
    if (sscanf(macStr, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
               &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) == 6) {
      esp_wifi_set_mac(WIFI_IF_STA, mac);
      customMacActive = true;
      info("custom MAC %s\n", macStr);
    }
  } else if (customMacActive) {
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    esp_wifi_set_mac(WIFI_IF_STA, mac);
    customMacActive = false;
  }
  delay(100);
  if (ip[0]) {
    esp_netif_dhcpc_stop(sta_netif);
    esp_netif_ip_info_t ip_info = {};
    ip_info.ip.addr      = ipaddr_addr(ip);
    ip_info.gw.addr      = ipaddr_addr(gw);
    ip_info.netmask.addr = ipaddr_addr(mask);
    esp_netif_set_ip_info(sta_netif, &ip_info);
    esp_netif_dns_info_t dns_info = {};
    if (dns[0])
      dns_info.ip.u_addr.ip4.addr = ipaddr_addr(dns);
    else
      dns_info.ip.u_addr.ip4.addr = ip_info.gw.addr;
    dns_info.ip.type = ESP_IPADDR_TYPE_V4;
    esp_netif_set_dns_info(sta_netif, ESP_NETIF_DNS_MAIN, &dns_info);
  } else {
    esp_netif_dhcpc_start(sta_netif);
  }
  wifi_config_t wifi_config = {};
  strncpy((char*)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
  if (pass[0])
    strncpy((char*)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password));
  esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
  xSemaphoreTake(wifiConnectedSem, 0);
  staConnected = false;
  esp_wifi_connect();
  bool ok;
  if (ip[0]) {
    uint32_t t = millis();
    while (!staConnected && millis() - t < 25000) delay(200);
    wifi_ap_record_t ap_info;
    ok = (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK);
  } else {
    ok = (xSemaphoreTake(wifiConnectedSem, pdMS_TO_TICKS(25000)) == pdTRUE);
  }
  if (ok) {
    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(sta_netif, &ip_info);
    char ip_str[16];
    esp_ip4addr_ntoa(&ip_info.ip, ip_str, sizeof(ip_str));
    esp_netif_dns_info_t dns_info;
    esp_netif_get_dns_info(sta_netif, ESP_NETIF_DNS_MAIN, &dns_info);
    char dns_str[16];
    esp_ip4addr_ntoa(&dns_info.ip.u_addr.ip4, dns_str, sizeof(dns_str));
    /* Report the configured hostname too (empty if unset): it is what
     * <hostname>.local resolves to, so flashmon can address the device
     * by its real name instead of guessing the default. */
    char hostname[32];
    storageGetStr("s.net.hostname", hostname, sizeof(hostname), "");
    info("Connected \"%s\" ip %s dns %s host %s\n", ssid, ip_str, dns_str, hostname);
    /* Re-arm the one-shot inbound-TLS beacon so a reachability probe made right
     * after this connect gets logged with the client's source IP. */
    tlsArmConnLog();
    return true;
  }
  info("%s connect failed\n", ssid);
  esp_wifi_disconnect();
  return false;
}

/* The access point's two settings:
 *   s.net.wifi.ap.enable   0 never starts it. Written while the AP is live
 *                          (the settings toggle), the ST_AP loop drops it
 *                          immediately.
 *   s.net.wifi.ap.timeout  minutes of idle before the AP is spent.
 *                          0 keeps it up until a known network appears (the
 *                          ap.retry rescan). N>0 (default 10) shuts it down
 *                          after N minutes without link traffic, once per
 *                          boot. Any TCP traffic (a browser session) restarts
 *                          the idle timer, so the AP lives exactly as long as
 *                          someone is using it, and a pocketed device doesn't
 *                          burn power beaconing. The device keeps rescanning
 *                          for known networks every ap.retry seconds (radio
 *                          off in between) — only the AP is spent; a reboot
 *                          force-summons it. The spent window survives deep
 *                          sleep (RTC RAM) so cron wakes don't re-arm it; any
 *                          real reset reloads the RTC data segment and
 *                          re-arms. */
RTC_DATA_ATTR static bool rtcApWindowUsed = false;

/** The idle window in milliseconds; 0 means there isn't one. */
static uint32_t apIdleMs() {
  return (uint32_t)storageGetInt("s.net.wifi.ap.timeout", 10) * 60 * 1000;
}

static bool startAP() {
  if (!storageGetInt("s.net.wifi.ap.enable", 1)) { dbg("AP disabled\n"); return false; }
  if (apIdleMs() > 0 && rtcApWindowUsed) {
    dbg("AP window already used this boot\n");
    return false;
  }
  char ssid[33], pass[65], ip[16], mask[16];
  storageGetStr("s.net.wifi.ap.ssid", ssid, sizeof(ssid), WIFI_AP_SSID);
  storageGetStr("s.net.wifi.ap.pass", pass, sizeof(pass), WIFI_AP_PASS);
  storageGetStr("s.net.wifi.ap.ip",   ip,   sizeof(ip),   WIFI_AP_IP);
  storageGetStr("s.net.wifi.ap.mask", mask, sizeof(mask),  WIFI_AP_MASK);
  /* Configure AP IP before setting mode — prevents DHCP auto-start on default 192.168.4.1 */
  esp_netif_dhcps_stop(ap_netif);
  esp_netif_ip_info_t ip_info = {};
  ip_info.ip.addr      = ipaddr_addr(ip);
  ip_info.gw.addr      = ipaddr_addr(ip);
  ip_info.netmask.addr = ipaddr_addr(mask);
  esp_netif_set_ip_info(ap_netif, &ip_info);
  esp_wifi_set_mode(WIFI_MODE_AP);
  esp_netif_dhcps_start(ap_netif);
  wifi_config_t wifi_config = {};
  strncpy((char*)wifi_config.ap.ssid, ssid, sizeof(wifi_config.ap.ssid));
  wifi_config.ap.ssid_len = strlen(ssid);
  if (pass[0]) {
    strncpy((char*)wifi_config.ap.password, pass, sizeof(wifi_config.ap.password));
    wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
  } else {
    wifi_config.ap.authmode = WIFI_AUTH_OPEN;
  }
  wifi_config.ap.max_connection = 4;
  esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
  char ip_str[16]; esp_ip4addr_ntoa(&ip_info.ip, ip_str, sizeof(ip_str));
  info("AP ssid=%s ip=%s\n", ssid, ip_str);
  return true;
}

static void setDhcpHostname() {
  char hostname[32];
  storageGetStr("s.net.hostname", hostname, sizeof(hostname), "");
  esp_netif_set_hostname(sta_netif, hostname);
}

/* ---- multicast-RX hold ----
 * WIFI_PS_MAX_MODEM wakes only per listen interval and sleeps through the
 * DTIM beacons after which the access point transmits buffered multicast, so
 * a station in max power-save receives almost none of it (TCP survives on
 * retransmission; raw UDP multicast just vanishes). Services that need
 * multicast hold the modem at WIFI_PS_MIN_MODEM — wake every DTIM — for as
 * long as they run. */
static std::atomic<int> s_mcastRxHolds{0};

static void netApplyPs() {
  if (wifiState == ST_OFF) return;   /* doUp applies it on the next bring-up */
  esp_wifi_set_ps(s_mcastRxHolds.load() > 0 ? WIFI_PS_MIN_MODEM : WIFI_PS_MAX_MODEM);
}

void netMulticastRxAcquire() {
  s_mcastRxHolds.fetch_add(1);
  netApplyPs();
}

void netMulticastRxRelease() {
  /* Underflow guard: a stray release must not wedge the count below zero. */
  if (s_mcastRxHolds.fetch_sub(1) <= 0) { s_mcastRxHolds.fetch_add(1); return; }
  netApplyPs();
}

static void doUp(wifi_state_t newState) {
  /* Sync global so publishWifiStatus below sees the new state. The main loop
   * also assigns `wifiState = state` after we return, but subscribers to
   * wifi.{sta,ap}.up read the value published here. */
  wifiState = newState;
  netLinkUp = true;        /* set before fireEvent so late-replay is consistent */
  netApplyPs();
  connectTimeMs = millis();
  netTrafficInBytes = netTrafficOutBytes = 0;
  netLastActivityMs = millis();
  epOpenAll();
  info("net up\n");
  fireEvent(NET_EV_UP);
  setUpstream(newState == ST_STA_CONNECTED);
  publishWifiStatus();
}

static void doDown(wifi_state_t& state) {
  info("shutting down\n");
  netLinkUp = false;       /* clear before fireEvent so late-replay is consistent */
  setUpstream(false);
  fireEvent(NET_EV_DOWN);
  epCloseAll();
  delay(200);
  esp_wifi_disconnect();
  esp_wifi_stop();
  esp_wifi_deinit();
  state = ST_OFF;
  wifiState = ST_OFF;  /* sync before publishWifiStatus, so wifi.{sta,ap}.up=0 */
  pmLockRelease(netDeepLock);
  publishWifiStatus();
}

/* Stop + deinit the radio without the link-down ceremony — for leaving
 * ST_SCANNING (the link was never up, so no NET_EV_DOWN / endpoint close).
 * Previously these paths only released the PM lock and left the radio
 * initialized and drawing power in "OFF". */
static void radioOff() {
  info("Turning off wifi\n");
  esp_wifi_stop();
  esp_wifi_deinit();
  pmLockRelease(netDeepLock);
}

static volatile bool cmdDisconnect = false;
static volatile uint32_t lastBrowserScanMs = 0;

/* `net scan` asks for a scan and waits for the answer. The radio belongs to
 * this task — a scan started from the CLI task would land in the middle of
 * whatever the state machine was doing with it, and esp_wifi_scan_stop() at the
 * top of a scan is exactly the kind of thing two tasks must not both do. So the
 * request goes in and a generation comes back out: the CLI waits for the number
 * to move, then prints the cache the scan has just fed. */
static volatile bool     scanReq = false;
static volatile uint32_t scanGen = 0;

static void netCmdHandler(TaskHandle_t, const void* data, size_t len) {
  if (len < 1) return;
  uint8_t cmd = *(const uint8_t*)data;
  switch (cmd) {
    case NET_CMD_UP:         cmdUp = true; break;
    case NET_CMD_DOWN:       cmdDown = true; break;
    case NET_CMD_FORCE_DOWN: cmdForceDown = true; break;
    case NET_CMD_DISCONNECT: cmdDisconnect = true; break;
    case NET_CMD_SCAN:       scanReq = true; break;
    case NET_CMD_CONNECT:
      if (len >= 2) cmdConnectIdx = ((const uint8_t*)data)[1];
      break;
    case NET_CMD_WIFI_ADD: {
      /* payload after the cmd byte is "<ssid>\t<pass>" (not NUL-terminated). */
      size_t n = (len > 1) ? len - 1 : 0;
      if (n >= sizeof(cmdWifiAddBuf)) n = sizeof(cmdWifiAddBuf) - 1;
      memcpy(cmdWifiAddBuf, (const uint8_t*)data + 1, n);
      cmdWifiAddBuf[n] = '\0';
      cmdWifiAdd = true;
      break;
    }
    case NET_CMD_WIFI_DEL:
      if (len >= 2) { cmdWifiDelIdx = ((const uint8_t*)data)[1]; cmdWifiDel = true; }
      break;
  }
}


/* The relay waits on lwIP's select, which no task notification can end: ten
 * milliseconds at most, so ITS traffic toward a socket waits no longer. */
int netRelayWait(int maxFd, fd_set* rfds, fd_set* wfds, bool) {
  if (maxFd >= 0) {
    struct timeval tv = { 0, 10000 };
    return select(maxFd + 1, rfds, wfds, NULL, &tv);
  }
  vTaskDelay(pdMS_TO_TICKS(10));
  return 0;
}

static void netTaskFn(void* arg) {
  netRelayTaskInit();
  itsOnAux(NET_CMD_PORT, netCmdHandler);

  /* Pre-register the core CLI/log TCP endpoints. spangapInit() brought those
   * tasks up before this straddle's init hook ran, so xTaskGetHandle resolves
   * them now; epOpenAll() opens them (if configured) once net is up. */
  netRegisterCorePorts();

  /* Tell the rns boot barrier whether to wait for the network: 1 if any STA
   * network is configured (so net.up is expected), else 0 so a WiFi-less node
   * doesn't stall the barrier waiting for an IP that will never come. Ephemeral
   * key keeps rns decoupled from net (and net-less builds never set it). */
  storageSet("net.want", staNetCount() > 0 ? 1 : 0);

  /* Net's own config: re-open endpoints when ports/host change. */
  storageSubscribeChanges("s.net.", ON_CHANGE {
    if (!netIsUp()) return;
    fireEvent(NET_EV_CFG_CHANGED, key);
    epOpenAll();
  });

  /* Re-broadcast specific prefixes/keys via NET_EV_CFG_CHANGED for module
   * helpers that don't have their own task (wg, ntp). Narrow on purpose —
   * wildcarding "s." floods this task's inbox during burst writes (boot
   * script, firmware default install, etc.) and the broad scope buys
   * nothing: wgOnCfg/ntpOnCfg already filter by key prefix anyway. */
  static auto rebroadcast = [](const char* key, const char*) {
    if (!netIsUp()) return;
    fireEvent(NET_EV_CFG_CHANGED, key);
  };
  storageSubscribeChanges("s.wg.",        rebroadcast);
  storageSubscribeChanges("secrets.wg.",  rebroadcast);
  storageSubscribeChanges("s.ntp.",       rebroadcast);
  storageSubscribeChanges("wg.keygen",    rebroadcast);
  storageSubscribeChanges("sys.time.set", rebroadcast);

  /* Browser WiFi panel triggers */
  storageSubscribeChanges("wifi.scan", ON_CHANGE {
    if (strcmp(key, "wifi.scan") != 0) return;  /* don't match wifi.scanned */
    if (atoi(val) == 1) lastBrowserScanMs = 0;  /* trigger immediate scan */
  });
  storageSubscribeChanges("wifi.connect", ON_CHANGE {
    if (strcmp(key, "wifi.connect") != 0) return;  /* prefix also matches wifi.connecting etc */
    /* An EMPTY value is this sentinel being cleared, not a request to join
     * anything. The task loop deletes the key as it takes the command, and a
     * delete arrives here as val="" — which atoi() reads as 0, a perfectly
     * valid network index. Without this line, taking a connect command posts
     * another one for net 0, whose own clear posts another, forever: the device
     * associates, opens its ports, announces itself, and immediately tears the
     * association down to "join" the network it is already on.
     *
     * It only ever bit after a connect that CAME from the sentinel — the
     * scan path and the browser's array rewrite never touch it — which is why
     * it surfaced when onboarding started adding the first network through
     * `wifi.cmd.add` (which joins by writing this key). The other three
     * sentinels have carried this guard from the start; this one was the odd
     * one out, and 0 is exactly the index an empty string parses to. */
    if (!val || !*val) return;
    int idx = atoi(val);
    if (idx >= 0 && idx < MAX_STA_NETWORKS) {
      uint8_t buf[2] = { NET_CMD_CONNECT, (uint8_t)idx };
      itsSendAuxByTaskHandle(netHandle, NET_CMD_PORT, buf, 2, pdMS_TO_TICKS(100));
    }
  });
  storageSubscribeChanges("wifi.disconnect", ON_CHANGE {
    if (atoi(val) == 1) {
      uint8_t cmd = NET_CMD_DISCONNECT;
      itsSendAuxByTaskHandle(netHandle, NET_CMD_PORT, &cmd, 1, pdMS_TO_TICKS(100));
    }
  });
  /* On-device WiFi add/delete (LCD WiFi pane). Forward to the net task over ITS
   * (like connect) so the array writes happen there, not on the storage actor. */
  storageSubscribeChanges("wifi.cmd.add", ON_CHANGE {
    if (strcmp(key, "wifi.cmd.add") != 0 || !val || !*val) return;
    uint8_t buf[1 + sizeof(cmdWifiAddBuf)];
    buf[0] = NET_CMD_WIFI_ADD;
    size_t n = strlen(val);
    if (n > sizeof(cmdWifiAddBuf) - 1) n = sizeof(cmdWifiAddBuf) - 1;
    memcpy(buf + 1, val, n);
    itsSendAuxByTaskHandle(netHandle, NET_CMD_PORT, buf, 1 + n, pdMS_TO_TICKS(100));
  });
  storageSubscribeChanges("wifi.cmd.del", ON_CHANGE {
    if (strcmp(key, "wifi.cmd.del") != 0 || !val || !*val) return;
    uint8_t buf[2] = { NET_CMD_WIFI_DEL, (uint8_t)atoi(val) };
    itsSendAuxByTaskHandle(netHandle, NET_CMD_PORT, buf, 2, pdMS_TO_TICKS(100));
  });

  /* The settings collection: wifi.net.add / .set / .remove / .order / .connect.
   * The UI never writes s.net.wifi.nets — it writes these, and this task is the
   * array's only writer, which is what puts validation in one place and lets a
   * rejection come back as a sentence on wifi.net.error. */
  storageSubscribeChanges("wifi.net.add", ON_CHANGE {
    if (strcmp(key, "wifi.net.add") == 0 && val && *val) cmdNetAdd = true;
  });
  storageSubscribeChanges("wifi.net.set", ON_CHANGE {
    if (strcmp(key, "wifi.net.set") == 0 && val && *val) cmdNetSet = true;
  });
  storageSubscribeChanges("wifi.net.remove", ON_CHANGE {
    if (strcmp(key, "wifi.net.remove") == 0 && val && *val) cmdNetRemove = true;
  });
  storageSubscribeChanges("wifi.net.order", ON_CHANGE {
    if (strcmp(key, "wifi.net.order") == 0 && val && *val) cmdNetOrder = true;
  });
  storageSubscribeChanges("wifi.net.connect", ON_CHANGE {
    if (strcmp(key, "wifi.net.connect") == 0 && val && *val) cmdNetConnect = true;
  });

  /* Master WiFi switch. s.net.wifi.enable was a defined config key (default 1)
   * that nothing consumed — setting it 0 did nothing, the radio kept scanning.
   * Bring net down/up to match; cold boot also seeds rtcWantUp from it (netInit).
   * netDown/netUp here post a command to our own inbox, picked up next loop. */
  storageSubscribeChanges("s.net.wifi.enable", ON_CHANGE {
    if (strcmp(key, "s.net.wifi.enable") != 0) return;
    if (atoi(val) == 0) { info("wifi.enable=0 → bringing net down\n"); netDown(true); }
    else                { info("wifi.enable=1 → bringing net up\n");   netUp(); }
  });

  /* Boot-complete gate: spangapPostAppInit() sets sys.boot_complete after the
   * whole init walk + boot script, so this fires exactly once, after the last
   * boot-time flash writer. collectChanges notifies on every set (no old/new
   * diff), so it fires even when the key persisted as 1 from a prior boot. The
   * notify is delivered over ITS and only dispatched while this task is in
   * itsPoll — the gate below keeps polling so this callback can run. */
  storageSubscribeChanges("sys.boot_complete", ON_CHANGE {
    if (strcmp(key, "sys.boot_complete") != 0) return;
    s_bootComplete = true;
  });

  wifiNetifInit();

  /* Every known network needs the id the settings collection addresses it by,
   * including on a store written before ids existed. */
  staNetEnsureIds();

  xSemaphoreGive(readySem);  /* unblock netInit — task is running */

  wifi_state_t state = ST_OFF;
  /* Missed scans in the current ST_SCANNING window — WIFI_SCANS_PER_CYCLE
   * misses fall back to AP / radio-off. Reset on every window (re)start. */
  int scanMisses = 0;
  uint32_t lastApRetryMs = 0;
  uint32_t lastStatusMs = 0;
  uint32_t lastOffScanMs = millis();  /* last radio-down known-network rescan */
  /* Consecutive failed connects to a *seen* known network in ST_SCANNING.
   * Every exit from scanning (connected / AP fallback) resets it to 0. */
  int connectFails = 0;
  /* AP retry interval read from s.net.wifi.ap.retry (default 300s) */

  if (wantUp()) {
    /* Hold the radio down until the boot init storm is over. The boot-complete
     * notify arrives over ITS, so keep pumping itsPoll here for the callback to
     * fire — a plain blocking wait would deadlock against its own signal. The
     * config-change subs registered above all no-op while netIsUp() is false,
     * so pumping ITS now can't bring the radio up early. The notify lands before
     * the persist worker has flushed, so also storageSave()-drain: bring-up
     * then meets idle flash and WiFi's PSRAM structs are written with the cache
     * stable. Timeout backstops a wedged boot script so it can't strand WiFi. */
    uint32_t gateStart = millis();
    while (!s_bootComplete) {
      if (millis() - gateStart >= 15000) {
        info("boot-complete gate timed out — bringing WiFi up anyway\n");
        break;
      }
      itsPoll(pdMS_TO_TICKS(50));
    }
    storageSave();
    pmLockAcquire(netDeepLock);
    setDhcpHostname();
    info("Bringing up STA mode to scan for wifi networks\n");
    wifiHwStart(WIFI_MODE_STA);
    /* Scan even with no networks configured: the ST_SCANNING loop runs
     * WIFI_SCANS_PER_CYCLE scans and then falls back to AP, and those scans
     * fill the cache `net scan` reports — which is how setup tooling gets a
     * list of surrounding networks to offer the user without asking for a scan
     * of its own. */
    state = ST_SCANNING;
  }

  for (;;) {
    bool connected = (state == ST_STA_CONNECTED || state == ST_AP);
    /* Radio down but still want-up with networks configured (spent AP window,
     * or AP disabled): keep rescanning for a known network every ap.retry
     * seconds so walking back into range reconnects — radio off in between. */
    bool offRescan = (state == ST_OFF) && wantUp() && staNetCount() > 0;

    /* Sleep when off; drain ITS (non-blocking when connected — select handles
     * timing). With a rescan pending, sleep only until it is due. */
    { TickType_t t = 0;
      if (state == ST_OFF) {
        if (offRescan) {
          uint32_t retryMs = (uint32_t)storageGetInt("s.net.wifi.ap.retry", 300) * 1000;
          uint32_t since = millis() - lastOffScanMs;
          t = pdMS_TO_TICKS(since >= retryMs ? 1 : retryMs - since);
        } else {
          t = portMAX_DELAY;
        }
      }
      while (itsPoll(t)) { t = 0; } }

    /* Process command flags set by aux handler */
    if (cmdForceDown) {
      cmdForceDown = false;
      rtcWantUp = false;
      if (state != ST_OFF) { doDown(state); wifiState = state; }
      continue;
    }

    if (cmdUp) {
      cmdUp = false;
      rtcWantUp = true;
      if (state == ST_OFF) {
        info("Bringing up STA mode to scan for wifi networks\n");
        pmLockAcquire(netDeepLock);
        setDhcpHostname();
        wifiHwStart(WIFI_MODE_STA);
        state = ST_SCANNING;
        scanMisses = 0;
        wifiState = state;
      }
    }

    if (cmdDown) {
      cmdDown = false;
      rtcWantUp = false;
      if (connected) {
        info("going down (waiting for idle)\n");
      } else if (state == ST_SCANNING) {
        doDown(state); wifiState = state; continue;
      }
    }

    /* Browser-triggered disconnect */
    if (cmdDisconnect) {
      cmdDisconnect = false;
      storageDeleteTree("wifi.disconnect");
      if (state == ST_STA_CONNECTED) {
        info("browser disconnect\n");
        fireEvent(NET_EV_DOWN);
        epCloseAll();
        esp_wifi_disconnect();
        if (startAP()) { state = ST_AP; doUp(state); }
        else { state = ST_SCANNING; scanMisses = 0; }
        wifiState = state;
        publishWifiStatus();
        continue;
      }
    }

    /* Browser-triggered connect to specific known network */
    if (cmdConnectIdx >= 0) {
      int idx = cmdConnectIdx;
      cmdConnectIdx = -1;
      storageDeleteTree("wifi.connect");
      if (idx < staNetCount()) {
        info("browser connect to net %d\n", idx);
        if (state == ST_STA_CONNECTED || state == ST_AP) {
          fireEvent(NET_EV_DOWN);
          epCloseAll();
        }
        /* Use APSTA if we're in AP mode so browser stays connected */
        if (state == ST_AP)
          esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (connectSta(idx)) {
          state = ST_STA_CONNECTED;
          doUp(state);
        } else {
          /* Connect failed — stay in or start AP */
          if (state != ST_AP) {
            if (startAP()) { state = ST_AP; doUp(state); }
            else { state = ST_SCANNING; scanMisses = 0; }
          } else {
            esp_wifi_set_mode(WIFI_MODE_AP);
            doUp(state);
          }
        }
        wifiState = state;
        publishWifiStatus();
        continue;
      }
    }

    /* The compact add form: wifi.cmd.add="<ssid>\t<pass>", written by the
     * first-run wizard and the `net` CLI where a JSON object would be a
     * ceremony. The settings collection uses wifi.net.add instead, which
     * carries the full field set. Captured once by the subscriber, applied here
     * in the net task so the array writes run in a safe context. The join is
     * emitted as a SEPARATE wifi.connect op: storage ops commit in emit order,
     * so ssid/pass are committed before the connect handler reads them. */
    if (cmdWifiAdd) {
      cmdWifiAdd = false;
      storageDeleteTree("wifi.cmd.add");
      std::string add = cmdWifiAddBuf;
      size_t tab = add.find('\t');
      std::string ssid = add.substr(0, tab == std::string::npos ? add.size() : tab);
      std::string pass = tab == std::string::npos ? "" : add.substr(tab + 1);
      if (!ssid.empty()) {
        int idx = staNetFindBySsid(ssid.c_str());
        std::string id = (idx >= 0) ? staNetField(idx, "id") : staNetNextId();
        if (idx < 0) idx = staNetCount();
        char k[64], v[8];
        storageBegin();
        snprintf(k, sizeof(k), "s.net.wifi.nets.%d.id",   idx); storageSet(k, id.c_str());
        snprintf(k, sizeof(k), "s.net.wifi.nets.%d.ssid", idx); storageSet(k, ssid.c_str());
        snprintf(k, sizeof(k), "s.net.wifi.nets.%d.pass", idx); storageSet(k, pass.c_str());
        snprintf(v, sizeof(v), "%d", idx); storageSet("wifi.connect", v);
        storageEnd();
        info("on-device add '%s' at %d — joining\n", ssid.c_str(), idx);
      }
      continue;
    }
    /* On-device WiFi delete: wifi.cmd.del="<index>" → array-correct remove. */
    if (cmdWifiDel) {
      cmdWifiDel = false;
      storageDeleteTree("wifi.cmd.del");
      staNetDeleteIdx(cmdWifiDelIdx);
      continue;
    }

    /* The settings collection's sentinels. Each reads its own payload, applies
     * it, and clears the key — the clear is what lets the same request be made
     * twice, and the reason a rejected one leaves wifi.net.error set. */
    if (cmdNetAdd) {
      cmdNetAdd = false;
      std::string payload = storageGetStr("wifi.net.add", "");
      storageDeleteTree("wifi.net.add");
      staNetAddJson(payload.c_str());
      continue;
    }
    if (cmdNetSet) {
      cmdNetSet = false;
      std::string payload = storageGetStr("wifi.net.set", "");
      storageDeleteTree("wifi.net.set");
      staNetSetJson(payload.c_str());
      continue;
    }
    if (cmdNetRemove) {
      cmdNetRemove = false;
      std::string id = storageGetStr("wifi.net.remove", "");
      storageDeleteTree("wifi.net.remove");
      int idx = staNetFindById(id.c_str());
      if (idx < 0) staNetError("That network is no longer configured.");
      else { staNetDeleteIdx(idx); staNetError(""); staNetAck(); }
      continue;
    }
    if (cmdNetOrder) {
      cmdNetOrder = false;
      std::string csv = storageGetStr("wifi.net.order", "");
      storageDeleteTree("wifi.net.order");
      staNetOrder(csv.c_str());
      continue;
    }
    if (cmdNetConnect) {
      cmdNetConnect = false;
      std::string id = storageGetStr("wifi.net.connect", "");
      storageDeleteTree("wifi.net.connect");
      int idx = staNetFindById(id.c_str());
      /* wifi.connect addresses a network by index, which is what every other
       * caller has; the collection knows only ids, so the translation is here. */
      if (idx >= 0) { char v[12]; snprintf(v, sizeof(v), "%d", idx); storageSet("wifi.connect", v); }
      continue;
    }

    /* `net scan` asked for one, wherever the radio happens to be. ST_OFF brings
     * it up for the scan and puts it straight back; ST_AP goes APSTA so the AP
     * keeps serving its clients while the scan runs, exactly as the browser's
     * beat does. A hit is NOT acted on — asking what is in earshot is not
     * asking to be moved onto it, and the state machine's own search is the one
     * thing allowed to change which network this node is on.
     *
     * The generation moves on every path, a failed scan included: the CLI is
     * parked on it and must never be left there. */
    if (scanReq) {
      scanReq = false;
      if (state == ST_OFF) {
        pmLockAcquire(netDeepLock);
        wifiHwStart(WIFI_MODE_STA);
        wifiScanRun(false);
        radioOff();
      } else {
        if (state == ST_AP) esp_wifi_set_mode(WIFI_MODE_APSTA);
        wifiScanRun(false);
        if (state == ST_AP) esp_wifi_set_mode(WIFI_MODE_AP);
      }
      scanGen = scanGen + 1;
      continue;
    }

    /* Periodic radio-down rescan (see offRescan above). One pass: radio up,
     * WIFI_SCANS_PER_CYCLE scans, connect on a hit, radio straight back off
     * on a miss. */
    if (offRescan && state == ST_OFF &&
        millis() - lastOffScanMs >= (uint32_t)storageGetInt("s.net.wifi.ap.retry", 300) * 1000) {
      lastOffScanMs = millis();
      info("Bringing up STA mode to scan for wifi networks\n");
      pmLockAcquire(netDeepLock);
      setDhcpHostname();
      wifiHwStart(WIFI_MODE_STA);
      int idx = wifiScanRun(true);
      for (int s = 1; idx < 0 && s < WIFI_SCANS_PER_CYCLE; s++) idx = wifiScanRun(true);
      if (idx >= 0 && connectSta(idx)) {
        state = ST_STA_CONNECTED;
        doUp(state);
      } else {
        radioOff();
      }
      wifiState = state;
      continue;
    }

    /* Graceful shutdown: want_up=0 + connected → wait for idle */
    if (!wantUp() && connected) {
      if (millis() - netLastActivityMs >= WIFI_IDLE_TIMEOUT_MS) {
        info("idle, shutting down\n");
        doDown(state); wifiState = state; continue;
      }
    }

    /* A new IPv6 address reached VALID — publish it now, not on the 30s tick. */
    if (connected && ip6Dirty) {
      ip6Dirty = false;
      publishWifiStatus();
    }

    /* Periodic status publishing (~30s) */
    if (connected && millis() - lastStatusMs >= 30000) {
      lastStatusMs = millis();
      uint32_t t0 = millis();
      publishWifiStatus();
      uint32_t dt = millis() - t0;
      if (dt > 200) verb("publishWifiStatus took %ums\n", (unsigned)dt);
    }

    /* Browser-triggered WiFi scan (every 20s while wifi.scan=1) */
    if (connected && storageGetInt("wifi.scan") == 1 &&
        millis() - lastBrowserScanMs >= 20000) {
      lastBrowserScanMs = millis();
      if (state == ST_AP)
        esp_wifi_set_mode(WIFI_MODE_APSTA);
      wifiScanRun(false);
      if (state == ST_AP)
        esp_wifi_set_mode(WIFI_MODE_AP);
    }

    switch (state) {
      case ST_OFF: break;
      case ST_SCANNING: {
        int idx = wifiScanRun(true);
        if (idx >= 0) {
          scanMisses = 0;
          /* A known network is in range. Give it WIFI_CONNECT_RETRIES attempts
           * before falling back to AP — a flaky AP or slow DHCP shouldn't bump
           * us off the network the user actually wants on the first miss. */
          if (connectSta(idx)) {
            connectFails = 0;
            state = ST_STA_CONNECTED;
            doUp(state);
          } else if (++connectFails >= WIFI_CONNECT_RETRIES) {
            info("connect to known network failed %dx, falling back to AP\n", connectFails);
            connectFails = 0;
            if (startAP()) { state = ST_AP; doUp(state); }
            else { state = ST_OFF; radioOff(); lastOffScanMs = millis(); }
          } else {
            info("connect failed (attempt %d/%d), retrying\n", connectFails, WIFI_CONNECT_RETRIES);
            delay(2000);
          }
        } else if (++scanMisses >= WIFI_SCANS_PER_CYCLE) {
          /* No known network visible after WIFI_SCANS_PER_CYCLE scans → AP. */
          scanMisses = 0;
          connectFails = 0;
          if (startAP()) { state = ST_AP; doUp(state); }
          else { state = ST_OFF; radioOff(); lastOffScanMs = millis(); }
        } else {
          delay(2000);
        }
        break;
      }
      case ST_STA_CONNECTED:
        netPollOnce();
        if (!staConnected) {
          info("disconnected, scanning...\n");
          fireEvent(NET_EV_DOWN);
          epCloseAll();
          esp_wifi_disconnect();
          /* Clear the residual STA config. When the AP we were on vanishes
           * (e.g. A20 goes out of range) its SSID stays loaded in the driver,
           * which keeps churning on it and wedges the rescan below — so
           * wifiScanRun() never matches an alternate visible network and we
           * time out into AP mode until a reboot. Nulling the target restores a
           * clean scan; WIFI_STORAGE_RAM keeps it RAM-only, so connectSta()
           * rewrites it on the next real connect. */
          { wifi_config_t clear = {};
            esp_wifi_set_config(WIFI_IF_STA, &clear); }
          /* Keep PM lock — still want_up, will reconnect */
          state = ST_SCANNING;
          scanMisses = 0;
        }
        break;
      case ST_AP:
        netPollOnce();
        /* Timed AP window (ap.timeout > 0): once the link has seen no traffic
         * for that many minutes, spend the window and drop the radio.
         * doUp() stamps netLastActivityMs, so an untouched AP lives exactly the
         * timeout; any TCP traffic restarts the timer, so an in-progress
         * browser session keeps it alive instead of being cut off mid-config.
         * rtcWantUp stays set: the radio-down rescan above keeps looking for
         * known networks — only the AP is spent until the next reboot. Note
         * the up-time in the log: the last "mode: softAP" driver line may be
         * the ap.retry APSTA scan flipping back, not the AP start, which makes
         * the window look shorter than it was. */
        { uint32_t idleMs = apIdleMs();
          /* Disabled while live (settings toggle): drop the AP on the spot.
           * The window isn't "spent" — rtcApWindowUsed stays clear, so
           * re-enabling later can start it again. The OFF-state rescan keeps
           * looking for known networks; startAP() itself refuses while the
           * switch is off, so the AP won't come back until re-enabled. */
          if (!storageGetInt("s.net.wifi.ap.enable", 1)) {
            info("AP disabled, dropping (up %u s)\n",
                 (unsigned)((millis() - connectTimeMs) / 1000));
            lastOffScanMs = millis();
            doDown(state);
            wifiState = state;
            continue;
          }
          if (idleMs > 0 && millis() - netLastActivityMs >= idleMs) {
            info("AP idle for %u min (up %u s), AP off until reboot\n",
                 (unsigned)(idleMs / 60000),
                 (unsigned)((millis() - connectTimeMs) / 1000));
            rtcApWindowUsed = true;
            lastOffScanMs = millis();
            doDown(state);
            wifiState = state;
            continue;
          }
        }
        { uint32_t apRetryMs = (uint32_t)storageGetInt("s.net.wifi.ap.retry", 300) * 1000;
          if (staNetCount() > 0 && wantUp() && millis() - lastApRetryMs >= apRetryMs) {
            lastApRetryMs = millis();
            /* Non-disruptive scan: APSTA keeps AP running for connected clients */
            esp_wifi_set_mode(WIFI_MODE_APSTA);
            delay(100);
            int idx = wifiScanRun(true);
            if (idx >= 0) {
              /* Found a known network — tear down AP and connect */
              fireEvent(NET_EV_DOWN);
              epCloseAll();
              if (connectSta(idx)) {
                state = ST_STA_CONNECTED;
                doUp(state);
              } else {
                startAP();
                doUp(state);
              }
            } else {
              /* Nothing found — back to pure AP, no disruption */
              esp_wifi_set_mode(WIFI_MODE_AP);
            }
          }
        }
        break;
    }
    wifiState = state;

    /* Sync upstream marker against the loop's resolved state. setUpstream is
     * idempotent, so this runs once per real transition and is a no-op
     * otherwise — saves having to instrument every state-change site. */
    setUpstream(state == ST_STA_CONNECTED);
  }
}

/* ---- ICMP ping (CLI) — raw socket on caller task; avoids esp_ping extra task (often ESP_ERR_NO_MEM / 257) ---- */

static int pingRecvEchoReply(int sock, uint16_t wantId, uint16_t wantSeq, uint8_t* ttlOut, uint32_t* payloadOut) {
    char buf[128];
    for (;;) {
        struct sockaddr_storage from{};
        socklen_t fromlen = sizeof(from);
        int len = recvfrom(sock, buf, sizeof(buf), 0, reinterpret_cast<struct sockaddr*>(&from), &fromlen);
        if (len <= 0) return len;
        if (from.ss_family != AF_INET) continue;
        if (len < (int)sizeof(struct ip_hdr)) continue;
        struct ip_hdr* iphdr = reinterpret_cast<struct ip_hdr*>(buf);
        int iphLen = IPH_HL_BYTES(iphdr);
        if (len < iphLen + (int)sizeof(struct icmp_echo_hdr)) continue;
        auto* iecho = reinterpret_cast<struct icmp_echo_hdr*>(buf + iphLen);
        if (iecho->type != ICMP_ER) continue;
        if (iecho->id != wantId || iecho->seqno != wantSeq) continue;
        *ttlOut = IPH_TTL(iphdr);
        *payloadOut = (uint32_t)(lwip_ntohs(IPH_LEN(iphdr)) - (uint16_t)iphLen - sizeof(struct icmp_echo_hdr));
        return len;
    }
}

static void pingCliCmd(const char* args) {
    if (cliWantsHelp(args)) {
        cliPrintf("%-*s ICMP echo (default target=router, count=4)\n", CLI_HELP_COL, "ping [ip] [count]");
        return;
    }
    if (!netIsUp()) {
        cliPrintf("ping: no network\n");
        return;
    }

    esp_netif_t* nif = (wifiState == ST_AP) ? ap_netif : sta_netif;
    if (!nif) {
        cliPrintf("ping: no interface\n");
        return;
    }

    ip_addr_t target{};
    uint32_t count = 4;

    while (*args == ' ' || *args == '\t') args++;
    if (!*args) {
        esp_netif_ip_info_t ipi{};
        if (esp_netif_get_ip_info(nif, &ipi) != ESP_OK || ipi.gw.addr == 0) {
            cliPrintf("ping: no gateway\n");
            return;
        }
        ip_addr_set_ip4_u32_val(target, ipi.gw.addr);
    } else {
        char host[48];
        const char* p = args;
        const char* q = p;
        while (*q && !std::isspace((unsigned char)*q)) q++;
        size_t len = (size_t)(q - p);
        if (len >= sizeof(host)) {
            cliPrintf("ping: address too long\n");
            return;
        }
        memcpy(host, p, len);
        host[len] = '\0';
        while (*q == ' ' || *q == '\t') q++;
        if (*q) {
            char* end = nullptr;
            unsigned long c = std::strtoul(q, &end, 10);
            if (end != q && c > 0 && c <= 32) count = (uint32_t)c;
            else cliPrintf("ping: bad count, using 4\n");
        }
        uint32_t raw = ipaddr_addr(host);
        if (raw == IPADDR_NONE || raw == 0) {
            cliPrintf("ping: invalid IPv4 address\n");
            return;
        }
        ip_addr_set_ip4_u32_val(target, raw);
    }

    const char* tstr = ipaddr_ntoa(&target);
    cliPrintf("PING %s (%s): 56 data bytes\n", tstr ? tstr : "?", tstr ? tstr : "?");

    constexpr uint32_t kDataSize = 56;
    constexpr uint32_t kIntervalMs = 1000;
    constexpr uint32_t kTimeoutMs = 2000;
    const size_t icmpTotal = sizeof(struct icmp_echo_hdr) + kDataSize;
    std::unique_ptr<uint8_t[]> pkt(new (std::nothrow) uint8_t[icmpTotal]);
    if (!pkt) {
        cliPrintf("ping: out of memory\n");
        return;
    }

    int sock = socket(AF_INET, SOCK_RAW, IP_PROTO_ICMP);
    if (sock < 0) {
        cliPrintf("ping: socket(AF_INET, SOCK_RAW) failed errno=%d\n", errno);
        return;
    }
    struct timeval tv;
    tv.tv_sec = kTimeoutMs / 1000;
    tv.tv_usec = (kTimeoutMs % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in to{};
    to.sin_family = AF_INET;
    inet_addr_from_ip4addr(&to.sin_addr, ip_2_ip4(&target));

    auto* echo = reinterpret_cast<struct icmp_echo_hdr*>(pkt.get());
    echo->type = ICMP_ECHO;
    echo->code = 0;
    echo->id = (uint16_t)(esp_random() & 0xFFFF);
    echo->seqno = 0;
    char* data = reinterpret_cast<char*>(pkt.get()) + sizeof(struct icmp_echo_hdr);
    for (uint32_t i = 0; i < kDataSize; i++) data[i] = static_cast<char>('A' + (i % 26));

    uint32_t tx = 0, rx = 0;
    struct timeval t0{}, t1{};
    gettimeofday(&t0, nullptr);

    for (uint32_t n = 0; n < count; n++) {
        echo->seqno = static_cast<uint16_t>(echo->seqno + 1);
        echo->chksum = 0;
        echo->chksum = inet_chksum(echo, icmpTotal);

        struct timeval ts{};
        gettimeofday(&ts, nullptr);
        ssize_t sent = sendto(sock, pkt.get(), icmpTotal, 0, reinterpret_cast<struct sockaddr*>(&to), sizeof(to));
        if (sent != (ssize_t)icmpTotal) {
            cliPrintf("ping: sendto failed errno=%d\n", errno);
            break;
        }
        tx++;

        uint8_t ttl = 0;
        uint32_t pay = 0;
        int r = pingRecvEchoReply(sock, echo->id, echo->seqno, &ttl, &pay);
        struct timeval te{};
        gettimeofday(&te, nullptr);
        uint32_t ms = (uint32_t)((te.tv_sec - ts.tv_sec) * 1000 + (te.tv_usec - ts.tv_usec) / 1000);

        if (r > 0) {
            rx++;
            cliPrintf("%u bytes from %s: icmp_seq=%u ttl=%u time=%ums\n",
                      (unsigned)(sizeof(struct icmp_echo_hdr) + pay), tstr ? tstr : "?",
                      (unsigned)echo->seqno, (unsigned)ttl, (unsigned)ms);
        } else {
            cliPrintf("Request timeout for icmp_seq %u\n", (unsigned)echo->seqno);
        }
        if (n + 1 < count) delay(kIntervalMs);
    }

    gettimeofday(&t1, nullptr);
    uint32_t dur = (uint32_t)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_usec - t0.tv_usec) / 1000);
    unsigned lossPct = tx ? (unsigned)((100U * (tx - rx)) / tx) : 0;
    cliPrintf("--- %u packets transmitted, %u received, %u%% packet loss, time %ums\n",
              (unsigned)tx, (unsigned)rx, lossPct, (unsigned)dur);
    close(sock);
}

/* ---- Public API ---- */

/* Find the index of a known STA network by SSID, or -1. */
static int staNetFindBySsid(const char* ssid) {
    int n = staNetCount();
    for (int i = 0; i < n; i++) {
        char s[33]; staNetGet(i, "ssid", s, sizeof(s));
        if (strcmp(s, ssid) == 0) return i;
    }
    return -1;
}

/* Tokenise `in` into outv[], in-place into `scratch` (NUL-separated). Tokens
 * are whitespace-delimited; "..."-quoted runs include literal whitespace and
 * are stored without the surrounding quotes. No escapes inside quotes.
 *
 * Returns:
 *    >= 0  number of tokens parsed (≤ maxOut)
 *      -1  bad quoting / scratch overflow
 *      -2  more than maxOut tokens present */
/* netParseArgs lives with the relay — the `hostname` command shares it, and
 * that command is not WiFi's. */
static inline int parseArgs(const char* in, char* scratch, size_t scratchLen,
                            char* outv[], int maxOut) {
    return netParseArgs(in, scratch, scratchLen, outv, maxOut);
}

/* `hostname` — show or set s.net.hostname. The new value is applied to the
 * STA netif and mDNS the next time the station reconnects (e.g. `net join`,
 * `net add`, or a reboot). */
/* ── waiting on the net task, from the CLI ─────────────────────────────────
 *
 * Three verbs hand work to the net task and have nothing worth saying until it
 * lands: `net scan` waits for the scan, `net add` and `net join` wait for the
 * connect attempt to reach a conclusion. Both print a dot a second, so a wait
 * reads as a wait rather than as a hang.
 *
 * **Ctrl-C abandons the WAIT, never the work.** The scan and the connect belong
 * to the net task and run to their own conclusion either way — which is what
 * makes the abort free to take, and why the message says so. `cliReadRaw`
 * doubles as the sleep and the keystroke check; a session with nothing to read
 * from (a script, cron) gets a plain sleep and no dots to abort.
 *
 * Returns false when the user asked to stop waiting. */
static bool cliWaitTick(uint32_t* lastDot) {
  char c;
  int n = cliReadRaw(&c, 1, 100);
  if (n > 0 && (c == 0x03 || c == 0x04)) return false;   /* Ctrl-C / Ctrl-D */
  if (n < 0) delay(100);                                 /* no reader; just wait */
  if (millis() - *lastDot >= 1000) { *lastDot = millis(); cliPrintf("."); }
  return true;
}

/* An unanswered request is a bug somewhere, not a state to sit in: every path
 * through the task's scan block moves the generation, and a connect attempt
 * ends in connected / AP / off. These are the backstops for the day one of
 * those stops being true. */
#define NET_CLI_SCAN_WAIT_MS     20000
#define NET_CLI_CONNECT_WAIT_MS  90000

/* Scan now, so what gets printed is the neighbourhood as it is rather than as
 * it was at boot. The CACHE is still what `net scan` prints — it is every
 * sighting since boot, and one scan is one look — this only makes sure the
 * newest look is in it first. */
static void netScanNow(void) {
  if (!netHandle) return;
  uint32_t gen = scanGen;
  uint8_t cmd = NET_CMD_SCAN;
  itsSendAuxByTaskHandle(netHandle, NET_CMD_PORT, &cmd, 1, pdMS_TO_TICKS(100));
  cliPrintf("scanning ");
  uint32_t start = millis(), lastDot = start;
  while (scanGen == gen) {
    if (millis() - start >= NET_CLI_SCAN_WAIT_MS) { cliPrintf(" no answer\n\n"); return; }
    if (!cliWaitTick(&lastDot)) { cliPrintf(" (the scan itself is still running)\n\n"); return; }
  }
  cliPrintf("\n\n");
}

/* Wait out the connect attempt netUp() has just asked for. Two phases, because
 * netUp() is a request and not the thing itself: the task has to pick it up —
 * until it does, the state still reads as whatever it was, and a naive wait
 * would return before anything had happened — and then the search has to end,
 * which it does by connecting, by falling back to this node's own AP, or by
 * turning the radio off. */
static void netWaitForConnect(void) {
  cliPrintf("connecting ");
  uint32_t start = millis(), lastDot = start;
  bool started = false;
  for (;;) {
    if (wifiState == ST_SCANNING) started = true;
    else if (started || millis() - start >= 3000) break;
    if (millis() - start >= NET_CLI_CONNECT_WAIT_MS) {
      cliPrintf(" still trying\n\n");
      return;
    }
    if (!cliWaitTick(&lastDot)) {
      cliPrintf(" (still connecting in the background)\n\n");
      return;
    }
  }
  cliPrintf("\n\n");
}

/* `net` with no verb: what this node's WiFi is doing right now. Its own
 * function because the verbs that change that state print it when they are
 * done — the answer to "did it work" is the same answer as "what is it doing",
 * and there should be one place that gives it. */
static void netCliStatus(void) {
  /* Four states: down, connecting, up, going down */
  if (wifiState == ST_OFF) { cliPrintf("wifi: down\n"); return; }
  if (wifiState == ST_SCANNING) { cliPrintf("wifi: connecting\n"); return; }
  bool goingDown = !wantUp();

  uint32_t upSecs = (millis() - connectTimeMs) / 1000;
  char elapsed[32];
  fmtElapsed(upSecs, elapsed, sizeof(elapsed));

  if (wifiState == ST_AP) {
    cliPrintf("wifi: %s (AP) - %s\n\n", goingDown ? "going down" : "up", elapsed);
    char ssid[33];
    storageGetStr("s.net.wifi.ap.ssid", ssid, sizeof(ssid), WIFI_AP_SSID);
    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(ap_netif, &ip_info);
    char ip[16], mask[16];
    esp_ip4addr_ntoa(&ip_info.ip, ip, sizeof(ip));
    esp_ip4addr_ntoa(&ip_info.netmask, mask, sizeof(mask));
    cliPrintf("SSID:    %s\n", ssid);
    cliPrintf("IP:      %s\n", ip);
    cliPrintf("netmask: %s\n", mask);
  } else {
    cliPrintf("wifi: %s - %s\n\n", goingDown ? "going down" : "up", elapsed);
    wifi_ap_record_t ap_info;
    const char* ssid = "?";
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) ssid = (const char*)ap_info.ssid;
    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(sta_netif, &ip_info);
    char ip[16], gw[16], mask[16];
    esp_ip4addr_ntoa(&ip_info.ip, ip, sizeof(ip));
    esp_ip4addr_ntoa(&ip_info.gw, gw, sizeof(gw));
    esp_ip4addr_ntoa(&ip_info.netmask, mask, sizeof(mask));
    esp_netif_dns_info_t dns1 = {}, dns2 = {};
    char dns1s[16], dns2s[16];
    esp_netif_get_dns_info(sta_netif, ESP_NETIF_DNS_MAIN, &dns1);
    esp_ip4addr_ntoa(&dns1.ip.u_addr.ip4, dns1s, sizeof(dns1s));
    esp_netif_get_dns_info(sta_netif, ESP_NETIF_DNS_BACKUP, &dns2);
    esp_ip4addr_ntoa(&dns2.ip.u_addr.ip4, dns2s, sizeof(dns2s));
    cliPrintf("SSID:    %s\n", ssid);
    cliPrintf("IP:      %s\n", ip);
    cliPrintf("router:  %s\n", gw);
    cliPrintf("netmask: %s\n", mask);
    if (strcmp(dns2s, "0.0.0.0") != 0)
      cliPrintf("DNS:     %s, %s\n", dns1s, dns2s);
    else
      cliPrintf("DNS:     %s\n", dns1s);
    char ip6[INET6_ADDRSTRLEN], ip6ll[INET6_ADDRSTRLEN];
    staIp6Strings(ip6, sizeof(ip6), ip6ll, sizeof(ip6ll));
    if (ip6[0])   cliPrintf("IPv6:    %s\n", ip6);
    if (ip6ll[0]) cliPrintf("IPv6 LL: %s\n", ip6ll);
  }
  char inBuf[16], outBuf[16];
  fmtSize(netTrafficInBytes, inBuf, sizeof(inBuf));
  fmtSize(netTrafficOutBytes, outBuf, sizeof(outBuf));
  cliPrintf("traffic: in %s, out %s\n", inBuf, outBuf);
}

static void netCliCmd(const char* args) {
    if (strcmp(args, "help") == 0) { cliPrintf("%-*s WiFi status; list/scan/up/down/add/join/delete\n", CLI_HELP_COL, "net [...]"); return; }
    if (cliWantsHelp(args)) {
        cliPrintf("%-*s WiFi control / status\n", CLI_HELP_COL, "net [up|down|down!]");
        cliPrintf("%-*s list stored WiFi networks\n", CLI_HELP_COL, "net list");
        cliPrintf("%-*s save a WiFi network (quote spaces)\n", CLI_HELP_COL, "net add <ssid> [pass]");
        cliPrintf("%-*s force-join a known network\n", CLI_HELP_COL, "net join <ssid>");
        cliPrintf("%-*s remove + disconnect\n",     CLI_HELP_COL, "net delete <ssid>");
        cliPrintf("%-*s scan now, then list every AP seen this boot\n", CLI_HELP_COL, "net scan");
        cliPrintf("%-*s add/join/scan wait for the result; Ctrl-C stops waiting,\n", CLI_HELP_COL, "");
        cliPrintf("%-*s not the scan or the connect itself\n", CLI_HELP_COL, "");
        cliPrintf("%-*s onboarding output: state/ssid/ip/hostname\n", CLI_HELP_COL, "net -O");
        cliPrintf("%-*s onboarding output: count + one ap= per network\n", CLI_HELP_COL, "net scan -O");
        return;
    }

    /* Onboarding output — the machine-readable contract, `key=value` lines and
     * nothing else. flashmon reads these to drive provisioning; the human
     * status display below is free to change without breaking it. */
    if (strcmp(args, "-O") == 0) {
        cliPrintf("state=%s\n", wifiState == ST_SCANNING      ? "connecting"
                              : wifiState == ST_AP            ? "ap"
                              : wifiState == ST_STA_CONNECTED ? "sta"
                                                              : "down");
        char ssid[33] = "", ip[16] = "";
        esp_netif_ip_info_t ip_info = {};
        if (wifiState == ST_AP) {
            storageGetStr("s.net.wifi.ap.ssid", ssid, sizeof(ssid), WIFI_AP_SSID);
            if (ap_netif) esp_netif_get_ip_info(ap_netif, &ip_info);
        } else if (wifiState == ST_STA_CONNECTED) {
            wifi_ap_record_t ap_info;
            if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK)
                safeStrncpy(ssid, (const char*)ap_info.ssid, sizeof(ssid));
            if (sta_netif) esp_netif_get_ip_info(sta_netif, &ip_info);
        }
        esp_ip4addr_ntoa(&ip_info.ip, ip, sizeof(ip));
        /* A key that isn't known is omitted, not emitted empty — the reader
         * treats missing and unknown as the same thing either way. */
        if (ssid[0]) cliPrintf("ssid=%s\n", ssid);
        if (ip[0] && strcmp(ip, "0.0.0.0") != 0) cliPrintf("ip=%s\n", ip);
        char host[48];
        storageGetStr("s.net.hostname", host, sizeof(host), "");
        if (host[0]) cliPrintf("hostname=%s\n", host);
        return;
    }

    if (strcmp(args, "scan") == 0 || strcmp(args, "scan -O") == 0) {
        const bool onboarding = (args[4] != '\0');
        /* The human form scans first, so what it prints is the neighbourhood as
         * it is. `-O` deliberately does not: it is the onboarding contract, read
         * over the framed channel by a caller holding a two-second timeout that
         * a full sweep of the band does not fit inside. It answers from the
         * cache, free and instantly, which is what its readers ask it for. */
        if (!onboarding) netScanNow();
        int order[SCAN_CACHE_MAX];
        int n = 0;
        for (int i = 0; i < scanSeenCount; i++) {
            /* An SSID that isn't representable on one line would corrupt a
             * key=value stream, so it is skipped rather than emitted — and
             * `count` is computed after this filter, so it always equals the
             * number of `ap=` lines that follow. */
            if (onboarding && strpbrk(scanSeen[i].ssid, "\r\n")) continue;
            order[n++] = i;
        }
        /* Sorting <= 64 entries at print time is free, and keeps the cache in
         * sighting order so nothing depends on when a network first appeared. */
        for (int i = 0; i < n - 1; i++)
            for (int j = i + 1; j < n; j++)
                if (scanSeen[order[j]].rssi > scanSeen[order[i]].rssi) {
                    int t = order[i]; order[i] = order[j]; order[j] = t;
                }
        if (onboarding) {
            cliPrintf("count=%d\n", n);
            /* SSID last, so a space in it needs no quoting. */
            for (int i = 0; i < n; i++)
                cliPrintf("ap=%d %s %s\n", scanSeen[order[i]].rssi,
                          scanSeen[order[i]].open ? "open" : "closed",
                          scanSeen[order[i]].ssid);
        } else if (n == 0) {
            cliPrintf("no access points seen yet\n");
        } else {
            for (int i = 0; i < n; i++)
                cliPrintf("%4ddBm  %-6s %s\n", scanSeen[order[i]].rssi,
                          scanSeen[order[i]].open ? "open" : "closed",
                          scanSeen[order[i]].ssid);
        }
        return;
    }

    if (strcmp(args, "up") == 0) { netUp(); return; }
    if (strcmp(args, "down!") == 0) { netDown(true); return; }
    if (strcmp(args, "down") == 0) { netDown(); return; }

    if (strcmp(args, "list") == 0) {
        int n = staNetCount();
        if (n == 0) { cliPrintf("no stored networks\n"); return; }
        /* Mark the one we're currently associated to, if any. */
        char cur[33] = "";
        if (wifiState == ST_STA_CONNECTED) {
            wifi_ap_record_t ap_info;
            if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK)
                safeStrncpy(cur, (const char*)ap_info.ssid, sizeof(cur));
        }
        for (int i = 0; i < n; i++) {
            char ssid[33], pass[65];
            staNetGet(i, "ssid", ssid, sizeof(ssid));
            staNetGet(i, "pass", pass, sizeof(pass));
            cliPrintf("%s[%d] %s%s\n", (cur[0] && strcmp(cur, ssid) == 0) ? "* " : "  ",
                      i, ssid, pass[0] ? "" : " (open)");
        }
        return;
    }

    if (strncmp(args, "add ", 4) == 0 || strcmp(args, "add") == 0) {
        char scratch[160]; char* argv[2];
        int n = parseArgs(args + 3, scratch, sizeof(scratch), argv, 2);
        if (n == -1) { cliPrintf("bad quoting (use \"...\" for spaces)\n"); return; }
        if (n == -2) { cliPrintf("too many arguments — quote spaces with \"...\"\n"); return; }
        if (n < 1)   { cliPrintf("usage: net add <ssid> [<password>]\n"); return; }
        const char* ssid = argv[0];
        const char* pass = (n == 2) ? argv[1] : "";
        if (!*ssid) { cliPrintf("usage: net add <ssid> [<password>]\n"); return; }
        int idx = staNetFindBySsid(ssid);
        if (idx < 0) idx = staNetCount();
        char k[64];
        storageBegin();
        snprintf(k, sizeof(k), "s.net.wifi.nets.%d.ssid", idx); storageSet(k, ssid);
        snprintf(k, sizeof(k), "s.net.wifi.nets.%d.pass", idx); storageSet(k, pass);
        storageEnd();
        cliPrintf("saved '%s' at index %d%s\n", ssid, idx, *pass ? "" : " (open)");
        /* Join immediately when we're not already on a real station — i.e.
         * sitting on the built-in AP, scanning, or off. A freshly added
         * network should come up without a separate `net join`. An existing
         * STA connection is left undisturbed. */
        if (wifiState != ST_STA_CONNECTED) {
            cliPrintf("joining '%s'…\n", ssid);
            netDown(true);
            netUp();
            netWaitForConnect();
            netCliStatus();
        }
        return;
    }

    if (strncmp(args, "join ", 5) == 0 || strcmp(args, "join") == 0) {
        char scratch[80]; char* argv[1];
        int n = parseArgs(args + 4, scratch, sizeof(scratch), argv, 1);
        if (n == -1) { cliPrintf("bad quoting (use \"...\" for spaces)\n"); return; }
        if (n == -2) { cliPrintf("too many arguments — quote spaces with \"...\"\n"); return; }
        if (n != 1)  { cliPrintf("usage: net join <ssid>\n"); return; }
        const char* ssid = argv[0];
        int idx = staNetFindBySsid(ssid);
        if (idx < 0) {
            cliPrintf("unknown network '%s' — add it first with `net add`\n", ssid);
            return;
        }
        cliPrintf("joining '%s'…\n", ssid);
        netDown(true);
        netUp();
        netWaitForConnect();
        netCliStatus();
        return;
    }

    if (strncmp(args, "delete ", 7) == 0 || strcmp(args, "delete") == 0) {
        char scratch[80]; char* argv[1];
        int n = parseArgs(args + 6, scratch, sizeof(scratch), argv, 1);
        if (n == -1) { cliPrintf("bad quoting (use \"...\" for spaces)\n"); return; }
        if (n == -2) { cliPrintf("too many arguments — quote spaces with \"...\"\n"); return; }
        if (n != 1)  { cliPrintf("usage: net delete <ssid>\n"); return; }
        const char* ssid = argv[0];
        int idx = staNetFindBySsid(ssid);
        if (idx < 0) { cliPrintf("no such network: '%s'\n", ssid); return; }
        int total = staNetCount();
        /* Are we currently associated to it? If so, disconnect after removal. */
        bool wasJoined = false;
        if (wifiState != ST_OFF && wifiState != ST_AP) {
            wifi_ap_record_t ap_info;
            if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK &&
                strcmp((const char*)ap_info.ssid, ssid) == 0) wasJoined = true;
        }
        staNetDeleteIdx(idx);   /* array-correct shift+drop (shared with wifi.cmd.del) */
        cliPrintf("removed '%s' (%d remain)\n", ssid, total - 1);
        if (wasJoined) {
            cliPrintf("disconnecting (was the current STA)…\n");
            netDown(true);
            netUp();
        }
        return;
    }

    if (*args) { cliPrintf("usage: net [up|down|down!|list|scan|add|join|delete]\n"); return; }

    netCliStatus();
}

/* ================= Wi-Fi traffic ring (activity monitor) ================= */

static NetTrafSample*    s_trafRing = nullptr;
static int               s_trafCap = 0, s_trafHead = 0, s_trafCount = 0;
static SemaphoreHandle_t s_trafMux = nullptr;
static bool              s_trafPrimed = false;
static uint32_t          s_prevBIn = 0, s_prevBOut = 0, s_prevPIn = 0, s_prevPOut = 0;

/* Per-window Wi-Fi averages for the web pill (tx/rx utilisation ×10 %, current
 * ×10 mA). Windows match pm.cpp's AVG_WINDOWS. */
static const int NET_WINDOWS[] = { 30, 60, 120, 180, 240, 300 };
static constexpr int N_NET_WINDOWS = (int)(sizeof(NET_WINDOWS) / sizeof(NET_WINDOWS[0]));
struct NetAvg { int txPctX10, rxPctX10, mA10; };
static void netTrafficAvgSet(NetAvg* out);   /* one-pass multi-window; defined below */

/* esp_netif's Wi-Fi RX path calls netif->input directly, bypassing lwIP's MIB2
 * netif accounting — so we tally frames ourselves by wrapping the STA netif's
 * input + linkoutput function pointers and chaining to the originals. ABI-safe
 * (no struct change) and re-armed each second in case the netif is recreated on
 * reconnect. Volatile counters: written on the tcpip/output threads, read by the
 * sampler — a torn read only skews one second's rate harmlessly. */
static volatile uint32_t   s_rxB = 0, s_rxP = 0, s_txB = 0, s_txP = 0;
static netif_input_fn      s_origInput   = nullptr;
static netif_linkoutput_fn s_origLinkout = nullptr;

static err_t trafInputHook(struct pbuf* p, struct netif* inp) {
  if (p) { s_rxB = s_rxB + p->tot_len; s_rxP = s_rxP + 1; }
  return s_origInput ? s_origInput(p, inp) : (err_t)ERR_IF;
}
static err_t trafLinkoutHook(struct netif* nif, struct pbuf* p) {
  if (p) { s_txB = s_txB + p->tot_len; s_txP = s_txP + 1; }
  return s_origLinkout ? s_origLinkout(nif, p) : (err_t)ERR_IF;
}
static void trafArmHooks(void) {
  if (!sta_netif) return;
  struct netif* n = (struct netif*)esp_netif_get_netif_impl(sta_netif);
  if (!n) return;
  if (n->input != trafInputHook)          { s_origInput   = n->input;      n->input      = trafInputHook; }
  if (n->linkoutput != trafLinkoutHook)   { s_origLinkout = n->linkoutput; n->linkoutput = trafLinkoutHook; }
}

/* One per-second sample: diff the cumulative counters, push the delta, and — if
 * a monitor is watching — publish the latest second. Runs on the CPU sampler's
 * beat (core 0). Unsigned subtraction is wrap-safe. */
static void netTrafficTick(void) {
  if (!s_trafMux) s_trafMux = xSemaphoreCreateMutex();   /* persists across restarts */
  if (!s_trafRing) {                             /* fresh zeroed ring each time watching resumes */
    int cap = storageGetInt("s.sys.cpu_sample_buf", 320);
    if (cap < 1) cap = 1;
    if (cap > 3600) cap = 3600;
    s_trafRing = (NetTrafSample*)gp_alloc((size_t)cap * sizeof(NetTrafSample));
    if (s_trafRing) memset(s_trafRing, 0, (size_t)cap * sizeof(NetTrafSample));
    s_trafCap  = s_trafRing ? cap : 0;
    s_trafHead = 0; s_trafCount = 0;
  }
  trafArmHooks();                                /* (re)install on the live netif */
  NetTrafSample d = { 0, 0, 0, 0 };
  uint32_t bIn = s_rxB, bOut = s_txB, pIn = s_rxP, pOut = s_txP;
  if (s_trafPrimed) {
    d.bytesIn  = bIn  - s_prevBIn;
    d.bytesOut = bOut - s_prevBOut;
    d.pktsIn   = pIn  - s_prevPIn;
    d.pktsOut  = pOut - s_prevPOut;
  }
  s_prevBIn = bIn; s_prevBOut = bOut; s_prevPIn = pIn; s_prevPOut = pOut;
  s_trafPrimed = true;

  if (s_trafRing && s_trafCap > 0 && s_trafMux) {
    xSemaphoreTake(s_trafMux, portMAX_DELAY);
    s_trafRing[s_trafHead] = d;
    s_trafHead = (s_trafHead + 1) % s_trafCap;
    if (s_trafCount < s_trafCap) s_trafCount++;
    xSemaphoreGive(s_trafMux);
  }

  if (storageGetInt("sys.stats.web_actmon", 0) || storageGetInt("sys.stats.lcd_actmon", 0)) {
    storageBegin();
    storageSet("sys.stats.net.bytes_in",  (int)d.bytesIn);
    storageSet("sys.stats.net.bytes_out", (int)d.bytesOut);
    storageSet("sys.stats.net.pkts_in",   (int)d.pktsIn);
    storageSet("sys.stats.net.pkts_out",  (int)d.pktsOut);
    /* Per-window Wi-Fi averages for the draggable pill:
       sys.stats.avg.net.w<secs>.{tx_x10,rx_x10,ma_x10}. */
    NetAvg na[N_NET_WINDOWS];
    netTrafficAvgSet(na);
    for (int i = 0; i < N_NET_WINDOWS; i++) {
      char key[52]; int w = NET_WINDOWS[i];
      snprintf(key, sizeof key, "sys.stats.avg.net.w%d.tx_x10", w); storageSet(key, na[i].txPctX10);
      snprintf(key, sizeof key, "sys.stats.avg.net.w%d.rx_x10", w); storageSet(key, na[i].rxPctX10);
      snprintf(key, sizeof key, "sys.stats.avg.net.w%d.ma_x10", w); storageSet(key, na[i].mA10);
    }
    storageEnd();
  }
}

int netTrafficHistory(NetTrafSample* out, int max) {
  if (!out || max <= 0 || !s_trafMux || !s_trafRing || s_trafCap <= 0) return 0;
  xSemaphoreTake(s_trafMux, portMAX_DELAY);
  int n = s_trafCount < max ? s_trafCount : max;
  int start = ((s_trafHead - n) % s_trafCap + s_trafCap) % s_trafCap;
  for (int i = 0; i < n; i++) out[i] = s_trafRing[(start + i) % s_trafCap];
  xSemaphoreGive(s_trafMux);
  return n;
}

/* Placeholder Wi-Fi power model — tune against a real measurement. A radio idle
 * floor plus a linear term in average throughput. */
static const int WIFI_IDLE_MA10     = 20;      /* ~2.0 mA radio idle / DTIM wake */
static const int WIFI_MA10_PER_MBIT = 30;      /* ~3.0 mA per Mbit/s of throughput */

int netTrafficAvgMa10(int secs) {
  if (secs <= 0) secs = 300;
  uint64_t sumBytes = 0;
  int n = 0;
  if (s_trafMux && s_trafRing && s_trafCap > 0) {
    xSemaphoreTake(s_trafMux, portMAX_DELAY);
    n = s_trafCount < secs ? s_trafCount : secs;
    int start = ((s_trafHead - n) % s_trafCap + s_trafCap) % s_trafCap;
    for (int i = 0; i < n; i++) {
      const NetTrafSample& e = s_trafRing[(start + i) % s_trafCap];
      sumBytes += (uint64_t)e.bytesIn + e.bytesOut;
    }
    xSemaphoreGive(s_trafMux);
  }
  if (n <= 0) return WIFI_IDLE_MA10;
  uint64_t avgBps = sumBytes / (uint32_t)n;                        /* avg total bytes/s */
  int64_t ma10 = WIFI_IDLE_MA10 + (int64_t)(avgBps * 8 * WIFI_MA10_PER_MBIT) / 1000000;
  return (int)ma10;
}

/* Utilisation is throughput as a fraction of a nominal link capacity — placeholder,
 * to be calibrated. tx = transmitted, rx = received. */
static const uint32_t WIFI_NOMINAL_BPS = 2000000;   /* 2 MB/s ≈ 100% (placeholder) */

static void netAvgFromSums(NetAvg* o, uint64_t txSum, uint64_t rxSum, int n) {
  if (n <= 0) { o->txPctX10 = o->rxPctX10 = 0; o->mA10 = WIFI_IDLE_MA10; return; }
  uint32_t txBps = (uint32_t)(txSum / (uint32_t)n), rxBps = (uint32_t)(rxSum / (uint32_t)n);
  o->txPctX10 = (int)((uint64_t)txBps * 1000 / WIFI_NOMINAL_BPS);   /* tenths of a percent */
  o->rxPctX10 = (int)((uint64_t)rxBps * 1000 / WIFI_NOMINAL_BPS);
  uint64_t tot = (uint64_t)txBps + rxBps;
  o->mA10 = WIFI_IDLE_MA10 + (int)((tot * 8 * WIFI_MA10_PER_MBIT) / 1000000);
}

/* All NET_WINDOWS in one backward pass (nested, like pmStatsAvgSet). */
static void netTrafficAvgSet(NetAvg* out) {
  for (int i = 0; i < N_NET_WINDOWS; i++) netAvgFromSums(&out[i], 0, 0, 0);
  uint64_t txSum = 0, rxSum = 0;
  int cnt = 0, wi = 0;
  if (s_trafMux && s_trafRing && s_trafCap > 0) {
    xSemaphoreTake(s_trafMux, portMAX_DELAY);
    int total = s_trafCount < NET_WINDOWS[N_NET_WINDOWS - 1] ? s_trafCount : NET_WINDOWS[N_NET_WINDOWS - 1];
    for (int k = 0; k < total; k++) {
      int idx = ((s_trafHead - 1 - k) % s_trafCap + s_trafCap) % s_trafCap;
      const NetTrafSample& e = s_trafRing[idx];
      txSum += e.bytesOut; rxSum += e.bytesIn; cnt++;
      while (wi < N_NET_WINDOWS && cnt == NET_WINDOWS[wi]) { netAvgFromSums(&out[wi], txSum, rxSum, cnt); wi++; }
    }
    xSemaphoreGive(s_trafMux);
  }
  for (; wi < N_NET_WINDOWS; wi++) netAvgFromSums(&out[wi], txSum, rxSum, cnt);
}

/* Freed when the last Activity monitor stops watching (the shared sampler is
   tearing down). The mutex persists; drop the ring and re-prime so a later
   resume reallocates fresh and its first delta isn't a giant stale jump. */
static void netTrafficStop(void) {
  if (s_trafMux) xSemaphoreTake(s_trafMux, portMAX_DELAY);
  free(s_trafRing); s_trafRing = nullptr; s_trafCap = 0; s_trafHead = 0; s_trafCount = 0;
  if (s_trafMux) xSemaphoreGive(s_trafMux);
  s_trafPrimed = false;
}

void netTrafficInit(void) {
  pmStatsAddSampler(netTrafficTick, nullptr, netTrafficStop);
}

void netInit() {
  netTrafficInit();
  netInitCommon();

  /* Suppress noisy WiFi driver block-ack renegotiation logs */
  esp_log_level_set("wifi", ESP_LOG_WARN);

  pmLockCreate(PM_NO_DEEP_SLEEP, "net", &netDeepLock);
  cliRegisterCmd("net", netCliCmd);
  cliRegisterCmd("ping", pingCliCmd);

  void wgetRegister();   /* wget.cpp — download CLI verb */
  wgetRegister();

  /* Master switch s.net.wifi.enable (default 1). Cold boot: seed rtcWantUp from
   * it. Warm boot / deep-sleep wake: preserve the runtime state (rtcWantUp lives
   * in RTC RAM) EXCEPT enable=0 always forces down — an explicit "off" must
   * survive a reset, and the change handler may not have run this boot (e.g. the
   * value was set in a config/boot script before net subscribed). Previously
   * this only seeded on cold boot, so a warm boot kept the preserved rtcWantUp=1
   * and scanned + brought up AP despite enable=0. */
  bool wifiEnabled = storageGetInt("s.net.wifi.enable", 1) != 0;
  if (!rtcRamValid()) {
    rtcWantUp = wifiEnabled;
    rtcApWindowUsed = false;   /* real reboot re-arms the timed AP window */
  }
  else if (!wifiEnabled) rtcWantUp = false;
  /* A factory-reset boot has nowhere to be. It exists to erase the store and
   * restart, and everything a radio would be for — joining the network this
   * device is configured for, standing up its AP, answering for a hostname —
   * describes a device that is about to stop existing. The stack still comes up
   * (the console, the log and the socket relay ride it); only the radio stays
   * down. Backup and restore are the other way round: those safe modes are
   * reached over the network and need it. */
  if (spangapSafeMode() == SAFE_MODE_FACTORY_RESET) rtcWantUp = false;

  readySem = xSemaphoreCreateBinary();
  wifiConnectedSem = xSemaphoreCreateBinary();
  netHandle = spawnTask(netTaskFn, "net", 8192, nullptr, 2, 0);
  xSemaphoreTake(readySem, portMAX_DELAY);  /* wait until task is running */
}

void netUp() {
  /* Master switch chokepoint: enable=0 means the radio stays off, period — every
   * caller (the wifi.enable change handler, `net up`, add-network helpers) is a
   * no-op while disabled. This is what stops a spurious/duplicate bring-up (the
   * cmdUp path doesn't consult wantUp()) from overriding an explicit disable. */
  if (storageGetInt("s.net.wifi.enable", 1) == 0) {
    info("netUp() ignored — wifi.enable=0\n");
    return;
  }
  rtcWantUp = true;
  if (!netHandle) return;  /* net task will pick up rtcWantUp on start */
  uint8_t cmd = NET_CMD_UP;
  itsSendAuxByTaskHandle(netHandle, NET_CMD_PORT, &cmd, 1, pdMS_TO_TICKS(100));
}

void netDown(bool force) {
  rtcWantUp = false;
  if (!netHandle) return;
  uint8_t cmd = force ? NET_CMD_FORCE_DOWN : NET_CMD_DOWN;
  itsSendAuxByTaskHandle(netHandle, NET_CMD_PORT, &cmd, 1, pdMS_TO_TICKS(100));
}

bool netIsUp() {
  return wifiState == ST_STA_CONNECTED || wifiState == ST_AP;
}

bool netIsStaConnected() {
  return wifiState == ST_STA_CONNECTED;
}

void netGetLocalIp(char* out, size_t len) {
  if (!sta_netif || !staConnected) { out[0] = '\0'; return; }
  esp_netif_ip_info_t ip_info;
  esp_netif_get_ip_info(sta_netif, &ip_info);
  esp_ip4addr_ntoa(&ip_info.ip, out, len);
}

