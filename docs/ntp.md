# NTP Time Sync

Sets system time from NTP. Include `ntp.h`.

## Usage

```cpp
ntpBegin();  // call after WiFi is connected
```

Reads `ntp.server` and `ntp.timezone` from config (settings.yaml). Defaults to `pool.ntp.org` and `GMT0`.

Timezone uses POSIX TZ format, e.g.:
- `GMT0` — UTC
- `CET-1CEST,M3.5.0,M10.5.0/3` — Central European Time
- `EST5EDT,M3.2.0,M11.1.0` — US Eastern

DNS must be reachable for NTP to work. If using static IP, configure DNS in settings or it falls back to the gateway.
