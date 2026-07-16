# Cinema operation and safety integration

The cinema mode adds deterministic seasonal calendars, crash recovery, and a
fail-closed audio gate to Theater Sound Manager (TSM). It is designed to be
supervised through the persistent CLI host and integrated with an external
cinema control system.

TSM is not a fire-alarm control panel, a certified sounder, an emergency-lighting
controller, or a substitute for the venue's SSI. Treat its evacuation preset as
an auxiliary function behind the certified safety chain.

## Calendar model

Cinema schedules select a configured playlist. The winning active entry is the
one with the highest numeric `priority`; equal priorities are resolved by the
lexicographically smallest schedule `id`. IDs must therefore be stable and
unique.

Supported periods are:

| `period.type` | Required fields | Meaning |
| --- | --- | --- |
| `always` | none | Every civil date |
| `date` | `date: YYYY-MM-DD` | One absolute date |
| `date-range` | `start`, `end: YYYY-MM-DD` | Inclusive absolute range |
| `annual-date` | `date: MM-DD` | Recurring annual date |
| `annual-range` | `start`, `end: MM-DD` | Inclusive recurring range; may cross New Year |
| `easter-range` | `startOffsetDays`, `endOffsetDays` | Inclusive offsets around Gregorian Easter Sunday |

`window.start` is inclusive and `window.end` is exclusive. Equal endpoints mean
a full local day. A window such as `18:00` to `02:00` is anchored to its starting
civil date, including its period and weekday filters. `weekdays` uses ISO values
1 (Monday) through 7 (Sunday).

Calendar evaluation intentionally uses local wall-clock time. The missing hour
at the spring DST transition is never invented. Both occurrences of a repeated
autumn hour resolve to the same schedule ID, so the playlist is not restarted
during the fold.

The shipped [cinema configuration example](../config/tsm_config.cinema.example.json)
contains Halloween, December/Christmas, and Easter-week schedules. Its media
paths are deployment placeholders and must be replaced with licensed, validated
files before use.

Preview a calendar without changing it:

```powershell
TheaterSoundManager.exe --cli cinema preview 2026-10-31T19:00 `
  --config C:\Cinema\TSM\config\tsm_config.json --no-sound --pretty
```

## Restart recovery

Use a dedicated durable directory on a local disk:

```powershell
TheaterSoundManager.exe --cli serve `
  --config C:\Cinema\TSM\config\tsm_config.json `
  --state-dir C:\ProgramData\Cinema\TSM
```

TSM atomically replaces strictly versioned `playback_state.json` and
`safety_state.json` files in this directory. The playback checkpoint records the
playlist, track identity/index, position, segment/random-order state, generation,
configuration fingerprint, timestamp, and whether the previous shutdown was
clean.

Automatic crash recovery occurs only when every condition is true:

- the previous checkpoint was left dirty by an unexpected stop;
- it is younger than `resume.maxAgeMinutes`;
- the exact configuration file fingerprint is unchanged;
- the currently winning calendar still selects the saved playlist;
- the safety/interlock state is Ready;
- `resume.enabled` and `resume.automatic` are true.

A clean shutdown starts the current calendar normally instead of restoring an
old channel. Stale, future-dated, mismatched, malformed, or unknown-version state
is never resumed. Corrupt or unreadable state inhibits programme audio until an
operator handles it. `session.discard` replaces a bad playback checkpoint but
does not restart music; `session.resume` is the explicit playback/re-arm action.

## Fail-closed interlock

When safety is enabled, the configured evacuation announcement is loaded into a
dedicated protected FMOD group. Generic sound and announcement commands cannot
play, alter, stop, unload, or replace it.

An enabled interlock requires an exact `expectedHeartbeatSource`. Each heartbeat
also carries a panel boot/session ID, a strictly increasing sequence, and one of
`safe`, `alarm`, or `fault`:

```json
{"id":"hb-1","command":"safety.heartbeat","params":{"source_id":"fire-panel","session_id":"panel-boot-20260716","sequence":1,"state":"safe"}}
```

The source must match the configuration allow-list exactly. This identifier is
not cryptographic authentication: the parent process, OS account, stdin handle,
and adapter transport form the trust boundary and must be protected accordingly.
Replayed sequences and retired sessions cannot extend freshness. A new session
must establish a new stable SAFE interval. Programme audio remains blocked from
process start until the first accepted SAFE signal has remained stable for
`safeStableMilliseconds`.

Safety reactions are deliberately asymmetric:

| Event | Programme audio | Evacuation preset | Recovery |
| --- | --- | --- | --- |
| `safe` and stable | Allowed | Stopped | Normal operation |
| `alarm` | Immediate hard stop | Repeated | Latched |
| `fault` | Immediate hard stop | Silent | Latched |
| heartbeat timeout | Immediate hard stop | Silent | Latched |
| startup heartbeat absent past grace | Blocked | Silent | Latched |
| manual trip with `play_evacuation=true` | Immediate hard stop | Repeated | Latched |

The normal-audio gate is applied before manager cleanup and at the FMOD channel
groups, so queued playlist, announcement, wedding, SFX, and Bluetooth work
cannot leak through it. Bluetooth is opt-in, unauthenticated, has no safety
commands, and its pending commands are discarded while playback is inhibited.

SAFE after an alarm never clears a latch. Reset requires all of the following:

- the exact active incident ID (compare-and-set protection);
- an authorized fresh, stable SAFE heartbeat when the interlock is enabled;
- a non-empty operator identifier and reset reason for the persisted audit;
- a separate `session.resume` after reset if programme audio should return.

Example operator sequence:

```json
{"id":"state","command":"safety.status","params":{}}
{"id":"reset","command":"safety.reset","params":{"incident_id":"INCIDENT_ID_FROM_STATUS","operator":"operator-17","reason":"Fire panel and auditorium verified safe"}}
{"id":"resume","command":"session.resume","params":{}}
```

`config.reload` is refused while an incident is latched so a reload cannot stop
or replace the active emergency preset.

## Certified safety boundary

French ERP Type L rules include automatic programme stopping and restoration of
normal lighting before the general alarm, a clear prerecorded evacuation
message for relevant equipped venues, and safety power for applicable alarm
equipment. Consult the current official text, your approved fire-safety design,
and the competent authority: [Légifrance, ERP Type L, article L 16 and related
provisions](https://www.legifrance.gouv.fr/codes/section_lc/JORFTEXT000000290033/LEGISCTA000020334570/2024-05-19).

TSM only controls audio that passes through its own FMOD process. A frozen
process, operating-system failure, powered-off PC, driver failure, amplifier
fault, disconnected cable, or compromised host can prevent a software-only
action. A production installation should therefore include, as specified and
approved for the venue:

- the certified SSI/general-alarm and emergency-lighting systems;
- a normally-safe external relay or certified priority input that physically
  mutes/overrides programme audio independently of TSM;
- an external watchdog/supervisor that treats process or heartbeat loss as a
  fault;
- the required safety power, monitored audio path, priority routing, and
  loudspeaker coverage;
- commissioning and periodic tests with the SSI maintainer and safety officer.

Do not connect TSM directly to a fire panel without the panel manufacturer,
qualified integrator, venue safety officer, and applicable approval process.

## Deployment checklist

1. Put the Release package, configuration, media, and state on controlled local
   paths; grant write access only to the service identity for the state/log
   directory.
2. Run `config validate --check-files`; reject any missing cinema or evacuation
   asset. Calibrate and verify the evacuation recording and downstream priority
   path independently.
3. Supervise `--cli serve`; parse `system.health`, `safety.status`, and stderr.
   `system.health.status=critical` or `safeToPlay=false` must create an operator
   alert.
4. Generate a unique heartbeat session on every panel-adapter boot and persist a
   monotonically increasing sequence within that session.
5. Test startup without heartbeat, wrong source, replay, SAFE stabilization,
   alarm, fault, timeout, process kill, power loss, corrupt state, reset CAS, and
   explicit resume.
6. Test Halloween, December/New Year, Easter, overnight windows, leap day, and
   both DST transitions using `cinema.preview`.
7. Keep Bluetooth disabled unless its non-safety legacy control is explicitly
   accepted on an isolated host.
8. Re-run the full commissioning test after every configuration, media,
   executable, audio-driver, amplifier, SSI-interface, or operating-system
   change.
