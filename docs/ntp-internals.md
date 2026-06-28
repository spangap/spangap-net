# ntp — internals

Maintainer reference for time sync. The [operator guide](ntp.md) covers the
config surface. Source: [src/ntp.cpp](../esp-idf/src/ntp.cpp),
[include/ntp.h](../esp-idf/include/ntp.h).

## 1. What this function provides

- **A reconciled SNTP engine** — `esp_sntp` started/stopped from one place,
  driven by upstream state AND an inhibit flag.
- **The IANA→POSIX timezone resolver** with a transient parse of an on-disk DB
  and a cached result.
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
freed TCB. So the `sys.time.ext` subscription is registered **lazily in
`ntpOnPoll`**, on the net task (which lives and polls), guarded by a `static
bool subDone`. It also applies the current `sys.time.ext` value once at that
point, in case a clock authority claimed it before net came up. Anything that
must outlive `app_main` goes here, not in `ntpInit()`.

## 4. Timezone resolver

`ntpApplyTimezone()` reads `s.ntp.tz` (IANA). If empty it early-exits, leaving
`TZ` unset → UTC. It prefers the cached `s.ntp.posix`; on a miss it calls
`zoneLookup()`, which reads `<stateDir>/timezones.json` into a PSRAM buffer
(falling back to `gp_alloc`), parses it with cJSON, walks the `/`-separated IANA
path down the nested objects (so `America/Argentina/Buenos_Aires` descends three
levels), pulls the one string, and **frees the whole cJSON tree before
returning** — nothing of the ~15 KB map survives the call. The result is cached
back into `s.ntp.posix`. `ntpOnCfg` clears `s.ntp.posix` whenever `s.ntp.tz`
changes so the new zone re-resolves.

`ntpInit()` ends by calling `updateTimeValid()` then `ntpApplyTimezone()`, so the
auto-init dispatcher runs NTP end-to-end with no consumer call site — log lines
switch from UTC to local from that point.

## 5. Time validity & browser push

`VALID_EPOCH` is 2025-01-01 00:00:00 UTC; `timeValid()` is `time(nullptr) >=
VALID_EPOCH`. `updateTimeValid()` publishes `sys.time.valid`. It's called on:
the registered SNTP sync-notification callback (`ntpSyncNotify`, which runs on
the tcpip task after a background poll sets the clock), the `date` set path, and
init.

`ntpOnCfg` handles `sys.time.set`: it accepts a browser-pushed epoch only if the
clock isn't already valid (NTP wins when present), calls `settimeofday`, and
clears the key.

## 6. Pitfalls

- **All SNTP start/stop must stay on the net task.** `ntpEngineApply` is only
  ever reached from net-task callbacks; calling `esp_sntp_init`/`stop` from
  another task races the engine. `ntpInhibit` is the cross-task entry — it sets a
  flag, nothing more.
- **The timezone map must not enter the config tree.** It's a loose file by
  design; keep it parsed transiently and freed. Holding it resident defeats the
  whole reason it lives outside `cfgRoot`.
- **Don't move the `sys.time.ext` subscription into `ntpInit()`.** It would be
  orphaned on main_task's self-delete (§3).
- **`s.ntp.version` is a config-version gate**, not a feature — it seeds defaults
  and, on the v1→v2 step, evicts the legacy in-config `s.time.zones` blob so the
  map is truly out of RAM after an upgrade. Under the no-config-migrations policy
  this is a candidate for code removal; don't add new migration branches.
