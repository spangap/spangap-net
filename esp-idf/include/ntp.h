#ifndef SECCAM_NTP_H
#define SECCAM_NTP_H

/** Register NTP net event callbacks. Call from main after netInit(). */
void ntpInit();

/** Apply s.ntp.tz / s.ntp.posix → TZ env. Safe to call as soon as
 *  storageLoad() has run; subsequent localtime() calls show local time. */
void ntpApplyTimezone();

#endif
