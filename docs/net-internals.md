# net — internals

Maintainer reference for the `net` task. The [operator guide](net.md) covers the
config surface and the integration points; this document is for changing the
code without breaking it. Source: [src/net.cpp](../esp-idf/src/net.cpp),
[include/net.h](../esp-idf/include/net.h), [src/wget.cpp](../esp-idf/src/wget.cpp),
and the LCD pane [conditional/spangap-lcd/src/net_lcd.cpp](../esp-idf/conditional/spangap-lcd/src/net_lcd.cpp).

## 1. What this function provides

All of `net` is new platform code (no upstream baseline to diff against). It
adds:

- **A single WiFi task** owning the STA/AP state machine, the TCP socket relay,
  and the event bus — a 4-state machine (`ST_OFF` / `ST_SCANNING` /
  `ST_STA_CONNECTED` / `ST_AP`) driven from one loop.
- **The `NET_EV_*` event bus** with level-replay for the two UP edges, so
  registration order versus bring-up never matters.
- **An inbound TCP endpoint registry** (`NET_PORT_REG_PORT`): tasks register a
  port by `net_port_msg_t`; net binds, listens, accepts, and bridges each
  connection to the owner over ITS, optionally terminating TLS first.
- **Outbound dial-on-behalf-of** (`NET_PORT_TCP_DIAL`): DNS + non-blocking
  connect (AF_UNSPEC, so v6-only hosts are reachable), the accepted ITS handle
  becomes the byte stream.
- **Core-service exposure** — net registers the `cli` and `log` tasks' TCP
  endpoints on their behalf (they know nothing about TCP), both off by default.
- **The `/state/net_up` boot hook** on every upstream-up.
- **Per-device AP SSID** seeding and the WiFi-enable master switch.
- **The `net` / `ping` CLI**, raw-socket ICMP echo, and the `wget` download
  command.
- **Two front ends:** the browser WiFi panel (`browser/`) and the LVGL WiFi pane
  (`conditional/spangap-lcd/`).

## 2. The net task

One task, **`"net"`, core 0, prio 2, 8 KB stack**, spawned by `netInit()`, which
blocks on a ready semaphore until the task is running (not until WiFi connects).
The task owns:

- **WiFi state** — `wifiState` (`volatile`, also read by the CLI and ping). The
  loop's local `state` is the authority; `wifiState` is synced after each
  transition so observers and `publishWifiStatus()` see the new value.
- **The endpoint table** (`netEps`, ≤8) and the **client table** (`netClients`,
  ≤8) — every active relayed connection, inbound or dialed.
- **The 4 KB PSRAM proxy buffer** (`netProxyBuf`), allocated in task context so
  heap accounting attributes it to net.

`netInit()` runs on the auto-init dispatcher (main_task). Anything that must
outlive `app_main` — the storage subscriptions, the core-port registration — is
set up inside the task, not in `netInit()`. (The same self-deletion trap bites
ntp; see [ntp-internals.md](ntp-internals.md).)

### The single loop

`netTaskFn` blocks in exactly one place per state: `portMAX_DELAY` ITS poll when
`ST_OFF`, otherwise the `select()`/`vTaskDelay(10ms)` inside `netPollOnce()`.
That select is **timeout-driven, not notify-driven** — it wakes on socket
activity or the 10 ms tick, never on a task notify, so `itsPoll`'s auto-boost
count is never silently dropped. `pmBoostAuto(false)` is released right before
the block so steady relaying rides the DFS floor instead of pinning 240 MHz.

Command flags (`cmdUp` / `cmdDown` / `cmdConnect…`) are set by the aux handler
`netCmdHandler` on `NET_CMD_PORT` and consumed at the top of the loop; the public
`netUp()` / `netDown()` post those commands to net's own inbox rather than
touching state directly.

## 3. The TCP socket relay

`netPollOnce()` does one unified `select()` over all server fds and all client
fds, then:

1. **Accept** on any readable server fd. For a TLS endpoint, if `tlsReady()` is
   false the connection is refused with a zero-linger RST so the browser backs
   off fast; otherwise `tlsAccept()` runs the handshake. `TCP_NODELAY` is set by
   default; `SO_KEEPALIVE` (10 s idle / 5 s × 3 probes) when the endpoint asked
   for it. A `net_connect_t` carries `ws=0, tls, clientAddr` to the owner.
2. **Relay** each client both ways through `netProxyBuf`: socket→ITS
   (`itsSend(..,0)` — drop on a full inbox) and ITS→socket (up to 4 rounds per
   poll). Dead-peer detection is asymmetric and load-bearing — see pitfalls.

`netRegisterCorePorts()` registers `cli`/`log` by resolving their tasks with
`xTaskGetHandle` (ITS names them `"cli"` / `"log"`); the dependency runs
net → core, never the reverse. Their port keys (`s.net.cli_port` /
`s.net.log_port`) default to 0.

`epOpenPort()` reads `s.net.<nvsKey>` live; the `s.net.` change subscription
calls `epOpenAll()` so editing a port key re-binds the socket without a reboot.
A service that wants a port simply ships `nvsKey` + `defaultPort` in its
`net_port_msg_t` — net owns the `s.net.*` key, the service owns the listener.

**Outbound dial** (`netOnDialConnect`) runs `netDialSync` on the net task:
`getaddrinfo` with `AF_UNSPEC`, then a bounded non-blocking connect over each
candidate (8 s) — a slow DNS lookup briefly stalls the select loop, acceptable
at the RNS-reconnect cadence. The dial fd reuses a `netClients` slot with
`epIdx = -1` so the relay path treats it like any other client.

## 4. WiFi state machine

`ST_SCANNING` calls `scanForKnown()` (active scan, match against
`s.net.wifi.nets[]`); a match gets `WIFI_CONNECT_RETRIES` (3) attempts before AP
fallback, so one slow DHCP doesn't bump the user off their network. `ST_AP`
re-scans every `s.net.wifi.ap.retry` seconds via APSTA (keeping the AP up for
joined clients) and switches to STA on a hit. `doUp()` sets `s_linkUp` before
firing `NET_EV_UP` and `setUpstream()`; `doDown()` clears it before firing
`NET_EV_DOWN` — late-replay consistency depends on that ordering.

With `s.net.wifi.ap.active_for` > 0 the AP is a one-shot idle window: the
`ST_AP` loop tears the radio down (`doDown` → `ST_OFF`) once `lastActivityMs`
is `active_for` seconds stale. `doUp()` stamps `lastActivityMs`, so an
untouched AP lives exactly `active_for` seconds, and any TCP traffic (the
relay's `netActivity()`) restarts the timer — an active browser session keeps
the AP alive for as long as it is used. The spent flag (`rtcApWindowUsed`)
lives in RTC RAM: preserved across deep sleep (cron wakes don't re-arm the
AP), reloaded to false by any real reset, and `startAP()` refuses while it is
set. `rtcWantUp` is deliberately left set, and `ST_OFF` with want-up + stored
networks runs the radio-down rescan: every `ap.retry` seconds one pass of
radio-up → `scanForKnown()` → connect on a hit / `radioOff()` on a miss, so
walking back into range reconnects without a reboot — only the AP is spent.
The same rescan covers `active_for = -1` (AP disabled) after a fruitless
`ST_SCANNING` window. `radioOff()` (stop + deinit + PM-lock release) is also
what the `ST_SCANNING` → `ST_OFF` fallbacks use — they previously left the
radio initialized and drawing power in "OFF".

`setUpstream(bool)` is idempotent (fires only on real transitions), writes the
ephemeral `net.up` storage key the rns boot barrier waits on, and fires
`NET_EV_UPSTREAM_{UP,DOWN}`. It's called both at each transition site and once
per loop against the resolved state, so no state-change site needs to remember to
instrument it.

**IPv6 is dual-stack on STA.** On `WIFI_EVENT_STA_CONNECTED` the task calls
`esp_netif_create_ip6_linklocal(sta_netif)`, and with SLAAC
(`CONFIG_LWIP_IPV6_AUTOCONFIG`) the station picks up a global v6 address from
router RAs; `IP_EVENT_GOT_IP6` just logs it. This is what makes the AF_UNSPEC
outbound dial (§3) able to reach v6-only hosts. No v6 address is published to the
`wifi.sta.*` telemetry — only the v4 lease is.

**The WiFi-enable master switch** (`s.net.wifi.enable`) gates *every* "should we
be up?" check. `rtcWantUp` (RTC RAM, survives deep sleep) is only a runtime
cache; `wantUp()` ANDs it with the persistent enable so a stale `rtcWantUp=1`
can't override an explicit `enable=0`. `netUp()` is a no-op while disabled.
`netInit()` re-seeds `rtcWantUp` from enable on cold boot, and forces it false on
any boot where enable is 0 (the change handler may not have run yet this boot).

## 5. Config / sentinel subscriptions

Set up inside the task. `s.net.` → re-open endpoints + `NET_EV_CFG_CHANGED`.
Specific prefixes are **re-broadcast** as `NET_EV_CFG_CHANGED` for module helpers
that have no task of their own — `s.wg.`, `secrets.wg.`, `wg.keygen`, `s.ntp.`,
`sys.time.set`. This is narrow on purpose: wildcarding `s.` floods the inbox
during burst writes (boot script, default install) and buys nothing, since
`wgOnCfg`/`ntpOnCfg` filter by key anyway. The `wifi.*` triggers
(`wifi.scan` / `wifi.connect` / `wifi.disconnect` / `wifi.cmd.add` /
`wifi.cmd.del`) forward to the net task over ITS so the array writes happen on a
safe context, not on the storage actor.

`net.want` is published once (1 iff `staNetCount() > 0`) so the rns barrier
doesn't wait for an IP that will never come on a WiFi-less node.

## 6. wget

`wgetWorker` runs on a temporary 24 KB **PSRAM-stack** task (`STACK_PSRAM`).
`onEvent` streams only the final 2xx body to disk (`is2xx` skips redirect-hop
bodies). The command blocks on a binary semaphore until the worker signals —
the worker always completes because `esp_http_client`'s 30 s timeout bounds it,
so the wait can't hang and `WgetJob` is never freed under the worker (no UAF).
A non-2xx status or a short write removes the partial file. There is no shared
HTTP-client wrapper on the platform; acme/duckdns/ota/viewer each call
`esp_http_client` the same way.

## 7. Maintainer pitfalls

- **Never re-post a WiFi lifecycle event** (`WIFI_EVENT_STA_START` /
  `AP_START` / `*_STOP`) as a notification. They look passive but
  `esp_netif_create_default_wifi_{sta,ap}()` installs default handlers that treat
  them as imperative: `*_START` → `esp_netif_action_start()` → `netif_add()`.
  Re-posting `AP_START` on an already-up AP double-adds the lwIP netif and trips
  the `netif_add: "netif already added"` assert (netif.c:420), even though net's
  own handler ignores the event. A consumer that missed a real start (registered
  late) must be driven through its own API instead — e.g. mdns via
  `mdns_netif_action(esp_netif_get_handle_from_ifkey("WIFI_AP_DEF"), …)`.
- **Dead-peer detection is asymmetric — keep it.** `tlsRead` maps real error/EOF
  to `<0` and no-data to `0`; raw `recv` is the opposite (`0` = EOF, `<0` =
  EAGAIN *or* a real error like ECONNRESET/ETIMEDOUT). The relay treats any
  non-EAGAIN negative as dead. Collapse the two and an errored fd stays
  select-readable forever, spinning the net task at ~100% CPU with no log.
- **`SO_SNDBUF` is silently ignored** — the TCP send buffer is compile-time
  (`CONFIG_LWIP_TCP_SND_BUF_DEFAULT`, 16384). Streaming throughput needs
  `TCP_NODELAY`, which the relay sets by default.
- **lwIP `O_NONBLOCK` / `MSG_DONTWAIT` can still briefly block** — the relay
  uses `select()` with a zero timeout before `accept`/`recv` rather than relying
  on the flag alone.
- **ISR contexts must not call `itsSend*`** (the ITS queues are PSRAM-backed).
  Use `vTaskNotifyGiveFromISR` plus a heap flag picked up by the target task's
  `itsPoll`.
- **The `net_up` script runs on its own spawned task**, because `cliRunFile`
  blocks until the script's commands drain — running it inline would stall the
  net loop.
- **AP SSID compile fallback vs seeded value.** `WIFI_AP_SSID` (in net.h)
  defaults to `CONFIG_SPANGAP_PROJECT_NAME` and is only the fallback used when
  `s.net.wifi.ap.ssid` is unset. The real per-device value is seeded once in
  `netInit()` as `<hostname>_<MAC[4]><MAC[5]>`, where `s.net.hostname` itself
  defaults to `CONFIG_SPANGAP_FW_HOSTNAME` — not the project name.
- **`s.net.version` is a config-version gate, not a feature.** `netInit()` only
  seeds defaults when the stored version is older. With zero users this gate is a
  candidate for code removal under the no-config-migrations policy; don't grow
  new migration branches in it.

## 8. Front ends

- **Browser** (`browser/src/`): `modules/net.ts` → `registerNet()` builds the
  Settings → Internet menu and registers `NetworkPanel.vue` (which embeds
  `WifiScanDialog.vue`). All known-network edits go through the array
  (`device.sendJson({s:{net:{wifi:{nets}}}})`); connect/disconnect/scan use the
  `wifi.*` ephemeral keys.
- **LCD** (`conditional/spangap-lcd/src/net_lcd.cpp`): `netLcdRegister()` →
  `lcdRegisterSettings("Internet/WiFi", …)`. Compiled only when spangap-lcd is
  staged; invoked via the `when:`-gated `netLcdRegister` init hook. It drives the
  same `wifi.connect` / `wifi.cmd.add` / `wifi.cmd.del` paths as the browser, and
  nulls its LVGL pointers on pane delete so a late storage callback can't touch
  freed objects.
