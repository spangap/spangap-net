# mdns — internals

Maintainer reference for the mDNS wrapper. The [operator guide](mdns.md) covers
config. Source: [src/spangap_mdns.cpp](../esp-idf/src/spangap_mdns.cpp),
[include/spangap_mdns.h](../esp-idf/include/spangap_mdns.h).

## 1. What this function provides

A thin, storage-driven wrapper over the ESP-IDF `mdns` component:

- **Lifecycle bound to net events** — `mdnsInit()` registers `mdnsStart` on
  `NET_EV_UP` and `mdnsStop` on `NET_EV_DOWN`; there is no other entry point.
- **Config-tree advertisement** — `storageForEach("s.net.mdns", …)` walks the
  tree and advertises each entry, resolving a config-key reference to a live
  port at advertise time.
- **The AP-responder enable fix** for the factory-reset boot race.

The header is named `spangap_mdns.h` (not `mdns.h`) to avoid colliding with the
IDF component's own `mdns.h`.

## 2. Advertise path

`mdnsStart`:

1. Returns immediately if `s.net.mdns_enable` is 0, or if `s.net.hostname` is
   empty.
2. `mdns_init()`, `mdns_hostname_set()`, `mdns_instance_name_set()`.
3. `storageForEach("s.net.mdns", mdnsAdvertiseEntry)` — each callback takes the
   last key segment as the service name (`s.net.mdns.http` → `_http._tcp`) and
   resolves the value: a leading digit means a literal port, otherwise it's a
   config key resolved with `storageGetInt(val, 0)`. Port `≤ 0` advertises
   nothing.
4. Enables the AP responder by hand when on the built-in AP (§3).

`mdnsStop` is `mdns_free()`. Re-advertising on every up edge is deliberate — it
keeps the published ports in step with the services' configured ports.

## 3. The AP-responder init-order trap

The IDF `mdns` component enables its per-interface responder only when *its own*
event handler catches the interface coming up — for the SoftAP that's
`WIFI_EVENT_AP_START` — and that handler is registered only inside `mdns_init()`.
On a factory-reset boot the net task has no stored networks, so it switches
straight to the built-in AP within milliseconds — long before this init runs, so
`AP_START` has already fired and been missed. The result is mDNS initialised but
the AP responder never enabled: `<hostname>.local` is dead for anyone joined to
the device's AP. (STA usually escapes this — the scan/associate delay leaves
mdns time to register before `GOT_IP`.)

The fix: when `netIsUp() && !netIsStaConnected()`, resolve the predefined
`WIFI_AP_DEF` interface and call
`mdns_netif_action(ap, MDNS_EVENT_ENABLE_IP4)` directly — exactly what mdns's own
`AP_START` handler does (`post_enable_pcb(MDNS_IF_AP, IPv4)`), but driving the
public API. Enabling an already-enabled pcb is idempotent.

## 4. Pitfalls

- **Never re-post the global `WIFI_EVENT_AP_START`** to wake the responder.
  `esp_netif_create_default_wifi_ap()` installs a default handler that treats it
  as imperative — `esp_netif_action_start()` → `esp_netif_start()` →
  `netif_add()` on the already-started AP netif — tripping lwIP's "netif already
  added" assert. Always drive the consumer through its own API
  (`mdns_netif_action`), never the shared `WIFI_EVENT`. (This is the general net
  rule; see [net-internals.md](net-internals.md).)
- **Service entries belong to their servers, not here.** `net` owns only the
  mechanism and `s.net.mdns_enable`. Don't seed a service's port default in this
  file — web/sshd/etc. seed their own `s.net.mdns.<name>` entries.
