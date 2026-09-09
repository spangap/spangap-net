# ntp — internals

Maintainer reference for time sync. The [operator guide](ntp.md) covers the
config surface. Source: [src/ntp.cpp](../esp-idf/src/ntp.cpp),
[include/ntp.h](../esp-idf/include/ntp.h).

## 1. What this function provides

- **A reconciled SNTP engine** — `esp_sntp` started/stopped from one place,
  driven by upstream state AND an inhibit flag.
- **The IANA→POSIX timezone resolver** over the firmware's built-in zone
  table.
- **The GPS-inhibit hand-off** over a storage key, with no compile-time coupling.
- **Browser time-push** (`sys.time.set`) and the `sys.time.valid` telemetry.
- **The `date` / `date wait` CLI.**

## 2. Engine reconcile

Three flags govern the engine: `s_up` (upstream internet, net task), `s_running`
(`esp_sntp_init` in effect, net task), and `volatile s_inhibited` (set by
`ntpInhibit()` from any task). `ntpEngineApply()` is the **single** place that
calls `esp_sntp_init` / `esp_sntp_stop`, computing `want = s_up && !s_inhibited`
and acting only on a change. It runs only in net-task context — the
`NET_EV_UPSTREAM_UP/DOWN` and `NET_EV_POLL` callbacks — so the start/stop calls
never race across tasks. `ntpInhibit()` just writes the flag; `ntpOnPoll`
reconciles it on the next net poll (≤ ~10 ms), so an external time source needs
no event of its own.

`esp_sntp_setservername` stores the **pointer, not a copy** — the server buffer
in `ntpEngineApply` is `static` to avoid a dangling pointer.

## 3. The self-deletion trap (why the subscription is lazy)

`ntpInit()` runs on the auto-init dispatcher (main_task), which **self-deletes
when `app_main` returns**. A `storageSubscribeChanges` registered there would be
orphaned — the callback never fires, and storage logs a "notify drop" into the
freed TCB. So every subscription this module owns — `sys.time.ext`, the
`ntp.tz.set` form command key, the `ntp.sync.now` button command key — is registered
**lazily in `ntpOnPoll`**, on the net task (which lives and polls), guarded by a
`static bool subDone`. That also puts the handlers in the one context allowed
to touch the SNTP engine and to run the file-parsing zone resolve. The
`sys.time.ext` value is applied once at registration, in case a clock authority
claimed it before net came up. Anything that must outlive `app_main` goes here,
not in `ntpInit()`.

## 4. Timezone resolver

`applyTz()` is the only writer of the `TZ` env var, and it is only ever fed a
**resolved POSIX string** — never a raw IANA name, which newlib cannot parse
and which would therefore mean silent UTC.

`ntpApplyTimezone()` reads `s.ntp.tz` (IANA). If empty it early-exits, leaving
`TZ` unset → UTC. It resolves via `tzLookup()` (spangap-core `timezones.h`): a
binary search over two strcmp-sorted rodata arrays generated at release time —
no file, no parse, no RAM, and therefore no cache to invalidate. On a hit it
calls `applyTz()`; on a miss the currently applied `TZ` is kept and a warning
logged — a name the table lacks must never tear down a working timezone, and a
later firmware whose table gained the zone resolves it on that boot.

`ntpOnCfg` on `s.ntp.tz` just calls `ntpApplyTimezone()` — the browser writes
`s.ntp.tz` directly on first connect; the settings form goes through the
validating `ntp.tz.set` command key instead, which rejects unresolvable names
outright.

`ntpInit()` ends by calling `updateTimeValid()` then `ntpApplyTimezone()`, so the
auto-init dispatcher runs NTP end-to-end with no consumer call site — log lines
switch from UTC to local from that point.

## 5. Time validity & browser push

`VALID_EPOCH` is 2025-01-01 00:00:00 UTC; `timeValid()` is `time(nullptr) >=
VALID_EPOCH`. `updateTimeValid()` publishes `sys.time.valid`. It's called on:
the registered SNTP sync-notification callback (`ntpSyncNotify`, which runs on
the tcpip task after a background poll sets the clock), the `date` set path, and
init. `ntpSyncNotify` additionally publishes `ntp.last_sync`, a finished
local-time string for the settings pane's "Last NTP sync" row — the visible
effect of the sync-now button.

`ntpOnCfg` handles `sys.time.set`: it accepts a browser-pushed epoch only if the
clock isn't already valid (NTP wins when present), calls `settimeofday`, and
clears the key.

## 6. Pitfalls

- **All SNTP start/stop must stay on the net task.** `ntpEngineApply` is only
  ever reached from net-task callbacks; calling `esp_sntp_init`/`stop` from
  another task races the engine. `ntpInhibit` is the cross-task entry — it sets a
  flag, nothing more. The `ntp.sync.now` command key (`esp_sntp_restart` is a
  stop+init) is safe only because its subscription is registered from the net
  task.
- **Never setenv an unresolved zone name.** `TZ` takes POSIX strings; newlib
  quietly falls back to UTC on anything else. All writes go through
  `applyTz()`, which is only fed `tzLookup()` output.
- **The zone table has exactly one refresh channel: `make timezones` +
  release.** There is no on-device zone file and no runtime upload — do not
  reintroduce one; stale zone data is fixed by shipping firmware, which
  happens far more often than IANA rule changes.
- **The timezone map must not enter the config tree.** It's a loose file by
  design; keep it parsed transiently and freed. Holding it resident defeats the
  whole reason it lives outside `cfgRoot`.
- **Don't move the `sys.time.ext` subscription into `ntpInit()`.** It would be
  orphaned on main_task's self-delete (§3).
- **`s.ntp.version` is a config-version gate**, not a feature — it seeds defaults
  and, on the v1→v2 step, evicts the legacy in-config `s.time.zones` blob so the
  map is truly out of RAM after an upgrade. Under the no-config-migrations policy
  this is a candidate for code removal; don't add new migration branches.
