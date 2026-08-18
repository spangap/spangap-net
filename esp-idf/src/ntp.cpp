/**
 * NTP — non-blocking time sync via esp_sntp + date CLI commands.
 * Registers NET_EV_UP/DOWN/CFG_CHANGED callbacks with net.
 * Publishes sys.time.valid (ephemeral) when time becomes valid.
 * Browser can push epoch seconds via sys.time.set when NTP is unavailable.
 */
#include "ntp.h"
#include "spangap.h"
#include "storage.h"
#include "timezones.h"
#include "net.h"
#include "cli.h"
#include "pm.h"
#include "log.h"
#include "compat.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <string>
#include <time.h>
#include <sys/time.h>
#include "esp_sntp.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const time_t VALID_EPOCH = 1735689600;  /* 2025-01-01 00:00:00 UTC */

static void applyTz(const char* iana, const char* posix);

static bool timeValid() { return time(nullptr) >= VALID_EPOCH; }

static void updateTimeValid() {
  bool valid = timeValid();
  storageBegin();
  storageSet("sys.time.valid", valid ? 1 : 0);
  /* Wake any boot task blocked in waitForTime() the instant the clock lands. */
  if (valid) signalTimeValid();
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
  storageEnd();
}

/* SNTP sync notification: lwIP calls this after a successful poll sets the
 * clock. Flip sys.time.valid so subscribers (e.g. the lcd status-bar clock)
 * react without polling. Runs on the tcpip task context. */
static void ntpSyncNotify(struct timeval*) {
  updateTimeValid();
  /* Finished local-time string for the settings "Last NTP sync" row (and the
   * visible effect of the sync-now button). Ephemeral: absent until the first
   * sync of a boot. */
  time_t now = time(nullptr);
  struct tm tm;
  localtime_r(&now, &tm);
  char buf[24];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
  storageSet("ntp.last_sync", buf);
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

/* The timezone as a form over a sentinel.
 *
 * The zone list is the firmware's built-in table (timezones.h) — hundreds of
 * entries, so it cannot be a static option list in a settings descriptor, and
 * a bare text field would take "Europe/Athens " or "CET" without a word. The
 * form submits the name here, this checks it against the table, and a miss
 * comes back as a sentence on ntp.tz.set.error. That is the same
 * submit-and-error shape every other validated setting uses, and it is why a
 * zone the device cannot resolve can no longer be stored. */
static void ntpTzSentinel(const char* key, const char* val) {
  if (strcmp(key, "ntp.tz.set") != 0 || !val || !*val) return;
  cJSON* o = cJSON_Parse(val);
  cJSON* m = o ? cJSON_GetObjectItem(o, "tz") : nullptr;
  std::string want = (cJSON_IsString(m) && m->valuestring) ? m->valuestring : "";
  if (o) cJSON_Delete(o);
  storageUnset(key);

  while (!want.empty() && isspace((unsigned char)want.front())) want.erase(0, 1);
  while (!want.empty() && isspace((unsigned char)want.back()))  want.pop_back();
  if (want.empty()) { storageSet("ntp.tz.set.error", "A timezone name is required."); return; }

  const char* posix = tzLookup(want.c_str());
  if (!posix) {
    storageSet("ntp.tz.set.error",
               ("No such timezone: \"" + want + "\". Use an IANA name, e.g. Europe/Berlin.").c_str());
    return;
  }
  storageSet("s.ntp.tz", want.c_str());
  /* Accepted: the form closes on the bump. A monotonic per-boot counter, not a
   * read-increment — reads see the committed tree, and the actor may not have
   * applied the previous bump yet. */
  static int ack = 0;
  storageSet("ntp.tz.set.done", ++ack);
  applyTz(want.c_str(), posix);
}

/* The one place TZ is written. Only ever fed a resolved POSIX string — never
 * a raw IANA name, which newlib can't parse and would silently mean UTC. */
static void applyTz(const char* iana, const char* posix) {
  setenv("TZ", posix, 1);
  tzset();
  info("timezone: %s → %s\n", iana, posix);
}

void ntpApplyTimezone() {
  char iana[48];
  storageGetStr("s.ntp.tz", iana, sizeof(iana));
  if (!iana[0]) return;
  const char* posix = tzLookup(iana);
  if (posix)
    applyTz(iana, posix);
  else
    /* Keep whatever TZ is currently applied; a table that gains the zone in
     * a later firmware resolves it on that boot. */
    warn("timezone: %s not in the built-in zone table — keeping current TZ\n", iana);
}

/* "Sync time now" button (settings System pane) writes this sentinel. Runs on
 * the net task — registered in ntpOnPoll — which is the only context allowed
 * to poke the SNTP engine, so esp_sntp_restart() (a stop+init) is safe here. */
static void ntpSyncNowSentinel(const char* key, const char* val) {
  /* The button writes with edge semantics (0 first, then 1) — only the 1 is
   * the press; the 0 and our own storageUnset echo must not retrigger. */
  if (strcmp(key, "ntp.sync.now") != 0 || !val || atoi(val) == 0) return;
  storageUnset(key);
  if (s_running) {
    esp_sntp_restart();
    info("ntp: immediate sync requested\n");
  } else {
    warn("ntp: sync requested but engine is stopped (%s)\n",
         s_inhibited ? "GPS owns time" : "no upstream");
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
    /* The settings sentinels live here too, and for the same reason: a
     * subscription from ntpInit() dies with main_task. Registering on the net
     * task also puts both handlers in the one context that may touch the SNTP
     * engine and do the (file-parsing) zone resolve. */
    storageSubscribeChanges("ntp.tz.set", ntpTzSentinel);
    storageSubscribeChanges("ntp.sync.now", ntpSyncNowSentinel);
  }
  ntpEngineApply();
}

/* ---- NET_EV_CFG_CHANGED: timezone + time set ---- */

static void ntpOnCfg(const char* key) {
  if (strcmp(key, "s.ntp.tz") == 0) {
    /* The browser sends only s.ntp.tz (its first-connect auto-config); the
     * settings form goes through the validating ntp.tz.set sentinel instead.
     * ntpApplyTimezone() resolves against the built-in table and touches TZ
     * only on success, so a name the table lacks never tears down a working
     * timezone. */
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
    delay(1000);
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
      "server": "pool.ntp.org",
      "tz":     ""
    })");
    /* v1→v2: the IANA→POSIX map is compiled into the firmware (timezones.h),
     * never config storage. On an OTA upgrade from v1 the legacy in-config
     * blob is still on disk and scanExternals() would keep it resident in
     * cfgRoot forever — evict it. No-op when the key is absent. */
    storageBegin();
    if (v < 2) storageDeleteTree("s.time.zones");
    storageSet("s.ntp.version", NTP_VERSION);
    storageEnd();
  }

  /* The ntp.tz.set / ntp.sync.now sentinels are registered lazily in
   * ntpOnPoll, on the net task — a subscription from here would be orphaned
   * when main_task self-deletes (see §3 of ntp-internals). The sentinels are
   * ephemeral keys BESIDE their values, never children of them — a dot-path
   * write under a scalar key replaces the scalar with an object. */

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
