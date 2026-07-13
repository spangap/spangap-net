/**
 * NTP — non-blocking time sync via esp_sntp + date CLI commands.
 * Registers NET_EV_UP/DOWN/CFG_CHANGED callbacks with net.
 * Publishes sys.time.valid (ephemeral) when time becomes valid.
 * Browser can push epoch seconds via sys.time.set when NTP is unavailable.
 */
#include "ntp.h"
#include "mem.h"
#include "storage.h"
#include "fs.h"
#include "net.h"
#include "cli.h"
#include "pm.h"
#include "log.h"
#include "compat.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <time.h>
#include <sys/time.h>
#include "esp_sntp.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const time_t VALID_EPOCH = 1735689600;  /* 2025-01-01 00:00:00 UTC */

static bool timeValid() { return time(nullptr) >= VALID_EPOCH; }

static void updateTimeValid() {
  bool valid = timeValid();
  storageSet("sys.time.valid", valid ? 1 : 0);
  /* Publish the wall-clock instant this device booted, so consumers can turn a
   * monotonic (since-boot) timestamp into real Unix time:
   *     unix_of_event = sys.boot_time + monotonic_seconds_of_event
   * The browser uses this to age the lxmf announce catalogue, whose stamps are
   * esp_timer seconds-since-boot (not wall time — an offline device has none).
   * Only meaningful once the clock is valid; recomputed on every clock step
   * (SNTP sync, browser sys.time.set, CLI date) so it tracks adjustments. */
  if (valid)
    storageSet("sys.boot_time",
               (int)(time(nullptr) - (time_t)(esp_timer_get_time() / 1000000)));
}

/* SNTP sync notification: lwIP calls this after a successful poll sets the
 * clock. Flip sys.time.valid so subscribers (e.g. the lcd status-bar clock)
 * react without polling. Runs on the tcpip task context. */
static void ntpSyncNotify(struct timeval*) {
  updateTimeValid();
}

/* ---- NTP start/stop ----
 *
 * The SNTP engine should run iff upstream internet is up AND no local time
 * authority has inhibited it (e.g. GPS, see ntpInhibit). s_up and s_running are
 * owned by the net task; s_inhibited is written from any task and read by the
 * net task. ntpEngineApply() is the single place that calls esp_sntp_init/stop
 * and runs only in net-task context (UP/DOWN/POLL callbacks), so those calls
 * never race across tasks. */
static bool s_up        = false;   /* upstream internet up (net task) */
static bool s_running   = false;   /* esp_sntp_init() in effect (net task) */
static volatile bool s_inhibited = false;  /* set by ntpInhibit() from any task */

static void ntpEngineApply() {
  bool want = s_up && !s_inhibited;
  if (want == s_running) return;
  if (want) {
    static char server[64];  /* esp_sntp_setservername stores pointer, not copy */
    storageGetStr("s.ntp.server", server, sizeof(server));
    ntpApplyTimezone();
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, server);
    esp_sntp_init();
    s_running = true;
    char tz[64];
    storageGetStr("s.ntp.tz", tz, sizeof(tz));
    info("ntp: %s TZ=%s\n", server, tz);
  } else {
    esp_sntp_stop();
    s_running = false;
    info("ntp: stopped (%s)\n", s_inhibited ? "GPS time" : "upstream down");
  }
}

void ntpInhibit(bool inhibit) {
  s_inhibited = inhibit;   /* net task reconciles on its next poll (≤~10 ms) */
}

/* Resolve the POSIX TZ string for an IANA name from the on-disk timezone DB.
 *
 * The IANA→POSIX map is large (~15 KB JSON, hundreds of zones) and is needed
 * only on the rare timezone change, so it deliberately does NOT live in the
 * config tree (cfgRoot) — that would keep it resident in RAM for the whole
 * runtime. It's a plain file at <stateDir>/timezones.json (the browser
 * refreshes it via an HTTPS PUT when GitHub's copy is newer). We parse it
 * transiently here, pull the one string we need, and free the cJSON tree
 * before returning — so nothing of it survives the call.
 *
 * Returns true and fills `out` on success; false if the file or zone is
 * missing. `iana` is split on '/' (e.g. "America/Argentina/Buenos_Aires"
 * descends three object levels). */
static bool zoneLookup(const char* iana, char* out, size_t outLen) {
  out[0] = '\0';
  std::string path = fsStatePath("/timezones.json");
  int fd = fs_open(path.c_str(), "rb");
  if (fd < 0) return false;
  fs_seek(fd, 0, SEEK_END);
  long sz = fs_tell(fd);
  fs_seek(fd, 0, SEEK_SET);
  if (sz <= 0 || sz > 256 * 1024) { fs_close(fd); return false; }
  char* buf = (char*)heap_caps_malloc(sz + 1, MALLOC_CAP_SPIRAM);
  if (!buf) buf = (char*)gp_alloc(sz + 1);
  if (!buf) { fs_close(fd); return false; }
  size_t rd = fs_read(buf, 1, sz, fd);
  fs_close(fd);
  buf[rd] = '\0';

  cJSON* root = cJSON_Parse(buf);
  free(buf);
  if (!root) return false;

  /* Walk the '/'-separated IANA path down the nested objects. */
  cJSON* node = root;
  std::string part;
  for (const char* p = iana; node; p++) {
    if (*p == '/' || *p == '\0') {
      node = cJSON_GetObjectItemCaseSensitive(node, part.c_str());
      part.clear();
      if (*p == '\0') break;
    } else {
      part += *p;
    }
  }
  if (cJSON_IsString(node) && node->valuestring)
    snprintf(out, outLen, "%s", node->valuestring);
  cJSON_Delete(root);
  return out[0] != '\0';
}

void ntpApplyTimezone() {
  char iana[48];
  storageGetStr("s.ntp.tz", iana, sizeof(iana));
  if (!iana[0]) return;

  /* Primary: cached POSIX string from a previous resolve (tiny, lives in
   * config). Invalidated by ntpOnCfg() whenever s.ntp.tz changes. */
  char posix[64];
  storageGetStr("s.ntp.posix", posix, sizeof(posix));

  /* Fallback: resolve from the on-disk timezone DB, then cache the result so
   * subsequent boots skip the file parse. */
  if (!posix[0] && zoneLookup(iana, posix, sizeof(posix)))
    storageSet("s.ntp.posix", posix);

  if (posix[0]) {
    setenv("TZ", posix, 1);
    tzset();
    info("timezone: %s → %s\n", iana, posix);
  } else {
    setenv("TZ", iana, 1);
    tzset();
    info("timezone: %s (no POSIX mapping)\n", iana);
  }
}

static void ntpOnUp(const char*)   { s_up = true;  ntpEngineApply(); }
static void ntpOnDown(const char*) { s_up = false; ntpEngineApply(); }
/* Cheap reconcile (two bool compares) on each net poll, so an ntpInhibit()
 * flip from another task takes effect without its own event. */
static void ntpOnPoll(const char*) {
  /* Register the sys.time.ext subscription HERE — on the net task, which lives
   * and polls — not in ntpInit(), which runs on the auto-init dispatcher
   * (main_task) and self-deletes when app_main returns, orphaning the
   * subscription (callback never fires; storage logs a "notify drop" into the
   * freed TCB). Once, on first poll; apply the current value too, in case a local
   * clock authority claimed it before net came up. */
  static bool subDone = false;
  if (!subDone) {
    subDone = true;
    storageSubscribeChanges("sys.time.ext", ON_CHANGE { ntpInhibit(atoi(val) != 0); });
    ntpInhibit(storageGetInt("sys.time.ext", 0) != 0);
  }
  ntpEngineApply();
}

/* ---- NET_EV_CFG_CHANGED: timezone + time set ---- */

static void ntpOnCfg(const char* key) {
  if (strcmp(key, "s.ntp.tz") == 0) {
    /* Drop the stale POSIX cache so ntpApplyTimezone() re-resolves the new
     * zone from the on-disk DB. The browser now sends only s.ntp.tz; the
     * device owns the IANA→POSIX lookup. */
    storageSet("s.ntp.posix", "");
    ntpApplyTimezone();
  } else if (strcmp(key, "sys.time.set") == 0) {
    char buf[16];
    storageGetStr("sys.time.set", buf, sizeof(buf));
    time_t epoch = (time_t)atoll(buf);
    if (epoch < VALID_EPOCH) return;
    if (timeValid()) return;
    struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    info("time set by browser: %lld\n", (long long)epoch);
    updateTimeValid();
    storageSet("sys.time.set", 0);
  }
}

/* ---- CLI: date, date wait ---- */

static void cmdDateWait(const char* a) {
  if (cliWantsHelp(a)) { cliPrintf("%-*s wait for valid date/time\n", CLI_HELP_COL, "date wait [timeout_secs]"); return; }
  if (timeValid()) return;
  int timeout = *a ? atoi(a) : 60;
  pm_lock_handle_t lock = nullptr;
  pmLockCreate(PM_NO_DEEP_SLEEP, "datewait", &lock);
  pmLockAcquire(lock);
  uint32_t start = millis();
  while (!timeValid()) {
    if ((int)(millis() - start) >= timeout * 1000) {
      info("date wait: timed out after %ds\n", timeout);
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
  if (timeValid()) {
    info("valid date received\n");
    updateTimeValid();
  }
  pmLockRelease(lock);
}

static void cmdDate(const char* a) {
  if (cliWantsHelp(a)) { cliPrintf("%-*s show or set date/time\n", CLI_HELP_COL, "date [wait] [yyyymmddhhmmss]"); return; }
  if (!*a) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm;
    localtime_r(&tv.tv_sec, &tm);
    cliPrintf("%04d-%02d-%02d %02d:%02d:%02d\n",
      tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
      tm.tm_hour, tm.tm_min, tm.tm_sec);
  } else {
    struct tm tm = {};
    if (sscanf(a, "%4d%2d%2d%2d%2d%2d",
      &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
      &tm.tm_hour, &tm.tm_min, &tm.tm_sec) == 6) {
      tm.tm_year -= 1900; tm.tm_mon -= 1;
      time_t t = mktime(&tm);
      struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
      settimeofday(&tv, NULL);
      updateTimeValid();
      cliPrintf("date set\n");
    } else {
      cliPrintf("usage: date [yyyymmddhhmmss]\n");
    }
  }
}

/* ---- Init ---- */

/* Module config version. Bump when adding/changing defaults. See duckdns.cpp. */
#define NTP_VERSION 2

void ntpInit() {
  int v = storageGetInt("s.ntp.version", 0);
  if (v < NTP_VERSION) {
    storageDefaultTree("s.ntp", R"({
      "server":     "pool.ntp.org",
      "tz":         "",
      "posix":      "",
      "zones_etag": ""
    })");
    /* v1→v2: the IANA→POSIX map moved out of config storage to the loose
     * file <stateDir>/timezones.json. On an OTA upgrade the legacy external
     * blob is still on disk and scanExternals() would keep it resident in
     * cfgRoot forever — evict it so the map is truly out of RAM. The new
     * loose file ships via the factory image / browser PUT. No-op when the
     * key is absent (e.g. fresh devices). The cached s.ntp.posix from v1
     * survives, so TZ stays applied even before the new file arrives. */
    if (v < 2) storageDeleteTree("s.time.zones");
    storageSet("s.ntp.version", NTP_VERSION);
  }

  /* Fire updateTimeValid() on every successful background SNTP sync — the
   * automatic poll calls settimeofday() inside lwIP, which we'd otherwise
   * never hear about. */
  esp_sntp_set_time_sync_notification_cb(ntpSyncNotify);

  netRegister(NET_EV_UPSTREAM_UP,   ntpOnUp);
  netRegister(NET_EV_UPSTREAM_DOWN, ntpOnDown);
  netRegister(NET_EV_POLL,          ntpOnPoll);
  netRegister(NET_EV_CFG_CHANGED,   ntpOnCfg);

  /* A local time authority (e.g. a GPS receiver) parks SNTP by writing
   * sys.time.ext=1 on the storage state bus — and releases it with 0. Driving it
   * through storage instead of a direct ntpInhibit() call means the time source
   * needs no compile-time dependency on net: a net-less image just has no
   * subscriber, and the source owns the clock outright. The subscription is
   * registered lazily in ntpOnPoll (on the net task) — NOT here — because ntpInit
   * runs on main_task, which self-deletes and would orphan it. */

  cliRegisterCmd("date wait", cmdDateWait);
  cliRegisterCmd("date", cmdDate);

  /* Publish initial time validity, then switch to the persisted timezone so
   * every subsequent log line is timestamped local. ntpApplyTimezone() used
   * to be a separate call the consumer made in app_main right after ntpInit();
   * folding it here lets the auto-init dispatcher run NTP end-to-end with no
   * consumer call site. */
  updateTimeValid();
  ntpApplyTimezone();
}
