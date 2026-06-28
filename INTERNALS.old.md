# spangap-net — internals

## Layout

```
esp-idf/
├── idf_component.yml
├── CMakeLists.txt
├── include/
│   ├── net.h
│   ├── tls.h
│   ├── ntp.h
│   └── spangap_mdns.h
└── src/
    ├── net.cpp
    ├── tls.cpp
    ├── ntp.cpp
    └── spangap_mdns.cpp
```

## `net.cpp` — the call centre

`net.cpp` is one task that owns:

- WiFi STA + AP state machine.
- Outbound TCP dial (clients hand it a target, get back an ITS handle).
- TCP listen ports registered by other tasks (`NET_PORT_REG_PORT`).
- Event dispatch (`NET_EV_*`) to subscribers.
- DHCP / hostname / mDNS interaction.

Why event dispatch is centralized: any number of straddles want to know
"the link came up" or "configuration changed", and asking each to wire
into raw `esp_event` plus track its own debounce is fragile. Subscribers
call `netRegister(event, cb)` and forget about it.

UP events level-replay (see README). DOWN / CFG / POLL are edge-only.

## `tls.cpp` — TLS server

mbedTLS server, EC P-256 self-signed cert (or ACME-issued cert once
`acme` is in the graph). Hot reload: when the ACME flow lands a new
cert pair, `tls` swaps it in without dropping active sessions.

Cipher suite forces ChaCha20-Poly1305 — the ESP32-S3's AES-GCM hardware
DMA bug (espressif/esp-idf#12689) corrupts payloads otherwise.

## `ntp.cpp` — non-blocking NTP

Built on `esp_sntp`. The IANA → POSIX timezone map ships as a plain
user-state file at `<stateDir>/timezones.json` (NOT a config subtree —
this keeps the ~15 KB map out of `cfgRoot`/RAM). `ntpApplyTimezone()`
parses it transiently on a timezone change, frees it immediately, and
caches the one resolved POSIX string in `s.ntp.posix`. `update-zones.py`
(in `spangap-core/scripts/`) regenerates the file; the browser refreshes
it via HTTPS PUT to `/state/timezones.json` when GitHub's copy is newer.
Exposes the `date` CLI.

## `spangap_mdns.cpp` — mDNS wrapper

Wrapper around ESP-IDF's `mdns` component. The header is named
`spangap_mdns.h` to avoid collision with IDF's own `mdns.h`.

## Why this is its own straddle

Phase-2 split from `spangap-core`. Two reasons:

1. **The IP stack is an optional capability, not part of the foundation.**
   An image that doesn't enable net shouldn't carry it. Keeping net out of
   `spangap-core` lets such a build skip `~150 KB` of lwIP code and the WiFi
   blob's RAM footprint — `--no-net` drops net and everything that
   hard-requires it.
2. **Several add-ons (acme, duckdns, upnp, wg, ota) want net but not
   the rest of `spangap-core`'s storage / log / fs heaviness.** They
   express that with `requires: [spangap/spangap-net]`.

`spangap-net` itself transitively requires `spangap-core` — there is no
configuration of spangap that runs without it.

## Conventions

- ISR contexts may NOT call `itsSend*` (the queues are PSRAM-backed —
  see [spangap/INTERNALS.md](../spangap/INTERNALS.md) ITS section).
  Use `vTaskNotifyGiveFromISR` plus a heap flag, picked up by the
  target task's `itsPoll`.
- UP-event handlers must be idempotent — level-replay can fire them
  out of the net task's context.
- **Never re-post a Wi-Fi lifecycle event (`WIFI_EVENT_STA_START` /
  `WIFI_EVENT_AP_START` / `*_STOP`) as a notification.** They look passive,
  but `esp_netif_create_default_wifi_{sta,ap}()` (in `wifiNetifInit`) installs
  default handlers that treat them as *imperative* state transitions:
  `*_START` → `esp_netif_action_start()` → `esp_netif_start()` →
  `netif_add()`, and `*_STOP` the reverse. Re-posting `AP_START` while the AP
  is already up therefore double-adds the lwIP netif and trips the
  `netif_add: "netif already added"` assert (`netif.c:420`), even though net's
  own `wifi_event_handler` ignores the event. If a *consumer* (e.g. mdns)
  missed a real start because it registered late, drive that consumer directly
  through its own API — `mdns_netif_action(esp_netif_get_handle_from_ifkey(
  "WIFI_AP_DEF"), MDNS_EVENT_ENABLE_IP4)` — never via the shared `WIFI_EVENT`.
