# NTP Time Sync

Sets system time from NTP. Include `ntp.h`.

## Usage

```cpp
ntpBegin();  // call after WiFi is connected (non-blocking, just calls esp_sntp_init)
ntpStop();   // stop SNTP (call before network down)
```

`ntpBegin()` configures and starts `esp_sntp` (non-blocking). The blocking wait for time sync is in `main.cpp`'s `waitForTime()`, which blocks the main task until valid time (>Jan 1 2026), then releases the boot deep sleep lock. A 30s one-shot FreeRTOS timer logs the synced time. Runs on the network task.

**Important**: `esp_sntp_setservername()` stores the pointer, not a copy. The server buffer is `static` to avoid dangling pointer.

## Configuration

| Config key | Default | Description |
|------------|---------|-------------|
| `s.ntp.server` | `pool.ntp.org` | NTP server hostname |
| `s.ntp.tz` | `GMT0` | POSIX timezone string |

Timezone uses POSIX TZ format, e.g.:
- `GMT0` — UTC
- `CET-1CEST,M3.5.0,M10.5.0/3` — Central European Time
- `EST5EDT,M3.2.0,M11.1.0` — US Eastern
- `PST8PDT,M3.2.0,M11.1.0` — US Pacific

DNS must be reachable for NTP to work.

## WiFi lifecycle

`net.cpp` calls `ntpBegin()` on network up and `ntpStop()` on network down. On network down→up cycle, `ntpBegin()` is called again (re-inits SNTP with fresh config from storage).

## CLI

`date` — show current date/time
`date yyyymmddhhmmss` — set date/time manually (e.g. `date 20260315143000`)
