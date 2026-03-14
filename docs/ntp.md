# NTP Time Sync

Sets system time from NTP. Include `ntp.h`.

## Usage

```cpp
ntpBegin();  // call after WiFi is connected (blocks up to 30s for sync)
ntpStop();   // stop SNTP (call before wifi down)
```

`ntpBegin()` configures `esp_sntp`, then polls `gettimeofday()` every 500ms for up to 30s. Logs the synced time via `info()` on success, or "no time after 30s" on timeout. Runs on the wifi task.

**Important**: `esp_sntp_setservername()` stores the pointer, not a copy. The server buffer is `static` to avoid dangling pointer.

## NVS configuration

| Key | Default | Description |
|-----|---------|-------------|
| `ntp_server` | `pool.ntp.org` | NTP server hostname |
| `ntp_tz` | `GMT0` | POSIX timezone string |

Timezone uses POSIX TZ format, e.g.:
- `GMT0` — UTC
- `CET-1CEST,M3.5.0,M10.5.0/3` — Central European Time
- `EST5EDT,M3.2.0,M11.1.0` — US Eastern
- `PST8PDT,M3.2.0,M11.1.0` — US Pacific

DNS must be reachable for NTP to work.

## WiFi lifecycle

`wifi_task.cpp` calls `ntpBegin()` on network up and `ntpStop()` on wifi down. On wifi down→up cycle, `ntpBegin()` is called again (re-inits SNTP with fresh config from NVS).

## CLI

`date` — show current date/time
`date yyyymmddhhmmss` — set date/time manually (e.g. `date 20260315143000`)
