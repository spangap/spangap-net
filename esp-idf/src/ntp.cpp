/**
 * NTP — non-blocking time sync via esp_sntp + date CLI commands.
 * Registers NET_EV_UP/DOWN/CFG_CHANGED callbacks with net.
 * Publishes sys.time.valid (ephemeral) when time becomes valid.
 * Browser can push epoch seconds via sys.time.set when NTP is unavailable.
 */
#include "ntp.h"
#include "storage.h"
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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const time_t VALID_EPOCH = 1735689600;  /* 2025-01-01 00:00:00 UTC */

static bool timeValid() { return time(nullptr) >= VALID_EPOCH; }

static void updateTimeValid() {
  storageSet("sys.time.valid", timeValid() ? 1 : 0);
}

/* SNTP sync notification: lwIP calls this after a successful poll sets the
 * clock. Flip sys.time.valid so subscribers (e.g. the lcd status-bar clock)
 * react without polling. Runs on the tcpip task context. */
static void ntpSyncNotify(struct timeval*) {
  updateTimeValid();
}

/* ---- NTP start/stop ---- */

static void ntpStop(const char*) {
  esp_sntp_stop();
}

void ntpApplyTimezone() {
  char iana[48];
  storageGetStr("s.ntp.tz", iana, sizeof(iana));
  if (!iana[0]) return;

  /* Primary: use cached POSIX string (set by browser alongside s.ntp.tz) */
  char posix[64];
  storageGetStr("s.ntp.posix", posix, sizeof(posix));

  /* Fallback: look up from s.time.zones (flash-only, transparent via storageGetStr) */
  if (!posix[0]) {
    std::string key = "s.time.zones.";
    for (const char* p = iana; *p; p++)
      key += (*p == '/') ? '.' : *p;
    storageGetStr(key.c_str(), posix, sizeof(posix));
    if (posix[0]) storageSet("s.ntp.posix", posix);  /* cache for next boot */
  }

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

static void ntpStart(const char*) {
  static char server[64];  /* esp_sntp_setservername stores pointer, not copy */
  storageGetStr("s.ntp.server", server, sizeof(server));
  ntpApplyTimezone();

  esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, server);
  esp_sntp_init();

  char tz[64];
  storageGetStr("s.ntp.tz", tz, sizeof(tz));
  info("ntp: %s TZ=%s\n", server, tz);
}

/* ---- NET_EV_CFG_CHANGED: timezone + time set ---- */

static void ntpOnCfg(const char* key) {
  if (strcmp(key, "s.ntp.tz") == 0) {
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
  if (strcmp(a, "help") == 0) { cliPrintf("  %-*s wait for valid date/time\n", CLI_HELP_COL, "date wait [timeout_secs]"); return; }
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
  if (strcmp(a, "help") == 0) { cliPrintf("  %-*s show or set date/time\n", CLI_HELP_COL, "date [wait] [yyyymmddhhmmss]"); return; }
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
#define NTP_VERSION 1

void ntpInit() {
  int v = storageGetInt("s.ntp.version", 0);
  if (v < NTP_VERSION) {
    storageDefaultTree("s.ntp", R"({
      "server": "pool.ntp.org",
      "tz":     "",
      "posix":  ""
    })");
    storageSet("s.ntp.version", NTP_VERSION);
  }

  /* Fire updateTimeValid() on every successful background SNTP sync — the
   * automatic poll calls settimeofday() inside lwIP, which we'd otherwise
   * never hear about. */
  esp_sntp_set_time_sync_notification_cb(ntpSyncNotify);

  netRegister(NET_EV_UPSTREAM_UP,   ntpStart);
  netRegister(NET_EV_UPSTREAM_DOWN, ntpStop);
  netRegister(NET_EV_CFG_CHANGED,   ntpOnCfg);

  cliRegisterCmd("date wait", cmdDateWait);
  cliRegisterCmd("date", cmdDate);

  /* Publish initial time validity (timezone applied early in app_main) */
  updateTimeValid();
}
