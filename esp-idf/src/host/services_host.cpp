/**
 * services_host — the net services a station process does not run, and the one
 * piece of one of them that still means something.
 *
 * The clock is the host's, already disciplined by whatever the machine runs,
 * so there is nothing for SNTP to correct; and a station is reached by its own
 * loopback address, which needs no name advertised on a local link. Both boot
 * hooks stay, because the generated boot dispatch calls them. The timezone is
 * a station setting either way, so it is still applied.
 */
#include "ntp.h"
#include "spangap_mdns.h"

#include "compat.h"
#include "log.h"
#include "spangap.h"
#include "storage.h"
#include "timezones.h"

#include "esp_timer.h"

#include <cstdlib>
#include <ctime>

/* The host's clock is already right, so the answer to "is the time valid yet?"
 * is yes from the first instant — and it has to be published, because the
 * boot tasks that wait for a clock wait on this and nothing else sets it. */
void ntpInit() {
  ntpApplyTimezone();
  storageBegin();
  storageSet("sys.time.valid", 1);
  signalTimeValid();
  storageSet("sys.boot_time",
             (int)(time(nullptr) - (time_t)(esp_timer_get_time() / 1000000)));
  storageEnd();
}

void ntpInhibit(bool) {}
void mdnsInit() {}

void ntpApplyTimezone() {
  char iana[48];
  storageGetStr("s.ntp.tz", iana, sizeof(iana));
  if (!iana[0]) return;
  const char* posix = tzLookup(iana);
  if (!posix) {
    warn("timezone: %s not in the built-in zone table — keeping current TZ\n", iana);
    return;
  }
  setenv("TZ", posix, 1);
  tzset();
  info("timezone: %s → %s\n", iana, posix);
}
