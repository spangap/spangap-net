#ifndef SECCAM_NTP_H
#define SECCAM_NTP_H

/** Register NTP net event callbacks. Call from main after netInit(). */
void ntpInit();

/** Apply s.ntp.tz / s.ntp.posix → TZ env. Safe to call as soon as
 *  storageLoad() has run; subsequent localtime() calls show local time. */
void ntpApplyTimezone();

/** Suspend/resume background SNTP polling. Used by an authoritative local time
 *  source (e.g. GPS) to stop fighting NTP once it owns the clock, and to hand
 *  control back when it goes stale. Thread-safe: sets a flag that the net task
 *  reconciles on its next poll, so the actual esp_sntp start/stop always runs
 *  in the net-task context. Idempotent. */
void ntpInhibit(bool inhibit);

#endif
