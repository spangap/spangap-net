# NTP Time Sync

Sets system time from NTP. Include `ntp.h`.

## Usage

```cpp
ntpBegin();  // call after WiFi is connected
```

Non-blocking. Configures `esp_sntp` and returns immediately. A one-shot FreeRTOS timer fires 30 seconds later to log the synced time (workaround: `esp_sntp` sync notification callback never fires on ESP32S3 despite time being set).

## NVS configuration

| Key | Default | Description |
|-----|---------|-------------|
| `ntp_server` | `pool.ntp.org` | NTP server hostname |
| `ntp_tz` | `GMT0` | POSIX timezone string |

Timezone uses POSIX TZ format, e.g.:
- `GMT0` — UTC
- `CET-1CEST,M3.5.0,M10.5.0/3` — Central European Time
- `EST5EDT,M3.2.0,M11.1.0` — US Eastern

DNS must be reachable for NTP to work.

## CLI

`date` — show current date/time
`date yyyymmddhhmmss` — set date/time manually (e.g. `date 20260315143000`)
