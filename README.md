# spangap-net

## What is this?

**spangap-net** is the IP-networking half of the [spangap](../spangap)
platform: WiFi, TCP/UDP, TLS, NTP, and mDNS. Any straddle that needs an
IP stack (`spangap-web`, `acme`, `ota`, `duckdns`, `upnp`, `wg`, and the
TCP / AutoInterface RNS transports) pulls this in. A device that talks
only over LoRa or ESP-NOW skips it entirely.

Firmware-only — no browser half, not a UI activator.

## What this straddle owns

| Module       | Header              | Source                | What it does                                              |
| ------------ | ------------------- | --------------------- | --------------------------------------------------------- |
| `net`        | `include/net.h`     | `src/net.cpp`         | WiFi + TCP/UDP call-centre + event dispatch               |
| `tls`        | `include/tls.h`     | `src/tls.cpp`         | mbedTLS server, EC P-256 cert, hot reload                 |
| `ntp`        | `include/ntp.h`     | `src/ntp.cpp`         | non-blocking NTP via `esp_sntp`, POSIX-TZ cache, `date` CLI |
| `mdns`       | `include/spangap_mdns.h` | `src/spangap_mdns.cpp` | wrapper around ESP-IDF `mdns` (renamed to avoid collision) |

## How others use it

In your `app_main()` after `spangapInit()`:

```cpp
netInit();
tlsInit();
ntpInit();
```

Other straddles register with `net` for TCP listen ports and net events:

```cpp
netRegister(NET_EV_UPSTREAM_UP, [](const char* data) { info("upstream up\n"); });
// Events: NET_EV_UP, NET_EV_DOWN, NET_EV_UPSTREAM_UP, NET_EV_UPSTREAM_DOWN,
//         NET_EV_CFG_CHANGED, NET_EV_POLL
```

Register a TCP listen port (the connection is handed back as an ITS
handle):

```cpp
net_port_msg_t reg = { .port = MY_PORT, .taskName = "mytask" };
itsSendAux("net", NET_PORT_REG_PORT, &reg, sizeof(reg), pdMS_TO_TICKS(500));
```

`net` also exposes `NET_PORT_TCP_DIAL` for outbound TCP dials. The
reticulous TCP transport uses this for its outbound peers.

## Behaviours worth knowing

- **UP edges are level-replayed.** A handler registered for
  `NET_EV_UP` / `NET_EV_UPSTREAM_UP` *after* the link is already up
  fires immediately. UP handlers must be idempotent and must tolerate
  running on the registering task, not just the net task. DOWN / CFG /
  POLL remain edge-only.
- **Per-device AP SSID.** First-boot defaults compute
  `s.net.wifi.ap.ssid = "<s.net.hostname>_<last 2 MAC bytes hex>"`
  (e.g. `reticulous_dcbc`) so a fleet doesn't present identical APs.
  `s.net.hostname` defaults to `CONFIG_SPANGAP_PROJECT_NAME`.
- **TLS uses ChaCha20-Poly1305**, not AES-GCM (ESP32-S3 AES-GCM hardware
  DMA bug: espressif/esp-idf#12689).
- **lwIP `O_NONBLOCK` / `MSG_DONTWAIT` can still briefly block.** Use
  `select()` with zero timeout before `accept()` / `recv()`.
- **`SO_SNDBUF` is silently ignored.** TCP send buffer is compile-time
  (`CONFIG_LWIP_TCP_SND_BUF_DEFAULT=16384`). Streaming requires
  `TCP_NODELAY`.

## What it does NOT own

- HTTPS server, REST, WebDAV, WebSocket — in
  [spangap-web](../spangap-web).
- WireGuard tunnel — in [wg](../wg).
- ACME / DuckDNS / UPnP — in [acme](../acme) / [duckdns](../duckdns) /
  [upnp](../upnp).
- OTA — in [ota](../ota).

## Read next

- [INTERNALS.md](INTERNALS.md) — implementation notes.
- Platform-wide [spangap/INTERNALS.md](../spangap/INTERNALS.md) for ITS
  patterns, gotchas, ESP-IDF specifics.
- Subsystem deep dives: see the `docs/` tree in
  [spangap-core](../spangap-core) (these files have not yet migrated).
