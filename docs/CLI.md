# Theater Sound Manager CLI

The CLI is a public, versioned interface for humans, scripts, and parent
applications. Launching the executable without arguments still starts the GUI.

## Integration model

There are two execution modes:

1. **One-shot commands** initialize the requested context, return one JSON
   response, and exit. They are appropriate for configuration validation,
   inspection, export/save, health checks, cache maintenance, and short playback
   with `--wait`.
2. **Persistent host mode** keeps FMOD and the application state alive. A parent
   application starts `--cli serve`, sends NDJSON requests on stdin, and reads
   responses from stdout. This is the recommended mode for live control.

Live playback commands intentionally fail in one-shot mode unless `--wait` is
provided. Otherwise the process would immediately close FMOD and stop the sound.
Commands that only mutate in-memory process state (for example `mixer.set`,
`playlist.create`, or `schedule.add`) require host mode and return
`persistent_host_required` in one-shot mode. This prevents a successful-looking
change from disappearing as soon as the process exits.
The read-only forms of `playlist.options` and `loudness.target` remain valid
one-shot inspections; adding an option/target value turns them into mutations
that require `serve`.

## Global invocation

```text
TheaterSoundManager.exe --cli <resource> <action> [arguments] [options]
TheaterSoundManager.exe --cli serve [global options]
TheaterSoundManager.exe --cli --request JSON [global options]
```

Global options:

| Option | Meaning |
| --- | --- |
| `--config PATH` | Configuration file. Auto-detected when omitted. |
| `--no-config` | Start with an empty in-memory session. |
| `--state-dir PATH` | Durable LUFS, playback-recovery, and safety-state directory. |
| `--no-sound` | Use FMOD's no-sound backend for tests and automation. |
| `--bluetooth` | Attempt to start the Bluetooth server in this process. |
| `--wait SECONDS` | Keep a one-shot runtime alive for 0 to 86400 seconds. |
| `--tick-ms N` | Engine tick period from 1 to 1000 ms; default is 10 ms. |
| `--pretty` | Indent one-shot JSON. Not allowed for NDJSON host mode. |
| `--quiet` | Disable console diagnostics. stdout remains JSON-only either way. |
| `--log-level LEVEL` | `trace`, `debug`, `info`, `warn`, `error`, `critical`, or `off`. |
| `--log-file PATH` | Also persist diagnostic logs to the requested file. |
| `--schema` | Print the versioned protocol and command catalog. |
| `--version` | Print application and CLI versions. |

CLI diagnostics go to stderr. stdout contains only the documented text help,
one-shot JSON, or NDJSON protocol records. ANSI log formatting never contaminates
machine responses.

## One-shot examples

```powershell
# Validate both the JSON structure and every referenced asset.
TheaterSoundManager.exe --cli config validate --check-files --pretty

# Inspect the current configuration through the same JSON contract.
TheaterSoundManager.exe --cli status --no-sound --pretty

# Play for thirty seconds, then shut down cleanly.
TheaterSoundManager.exe --cli playlist play playlist_PreShow --wait 30

# The raw request form avoids shell-specific positional parsing and echoes its ID.
TheaterSoundManager.exe --cli --request `
  '{"id":"probe","command":"system.ping","params":{}}'
```

## Persistent NDJSON protocol

Start the host:

```powershell
TheaterSoundManager.exe --cli serve --config config/tsm_config.json `
  --state-dir C:\ProgramData\Cinema\TSM
```

The host writes exactly one `ready` event when initialization has completed:

```json
{"schemaVersion":1,"apiVersion":"1.0","event":"ready","data":{"runtime":{},"mixer":{}}}
```

Then send one UTF-8 JSON object per line:

```json
{"id":"play","command":"playlist.play","params":{"name":"playlist_PreShow"}}
{"id":"volume","command":"mixer.set","params":{"master":0.75}}
{"id":"state","command":"system.status","params":{}}
{"id":"stop","command":"system.shutdown","params":{}}
```

Every request produces exactly one response in input order:

```json
{
  "schemaVersion": 1,
  "apiVersion": "1.0",
  "id": "volume",
  "command": "mixer.set",
  "ok": true,
  "data": {"mixer": {"master": 0.75}},
  "error": null
}
```

Errors use the same envelope:

```json
{
  "schemaVersion": 1,
  "apiVersion": "1.0",
  "id": "play",
  "command": "playlist.play",
  "ok": false,
  "data": null,
  "error": {
    "code": "playlist_not_found",
    "message": "Playlist 'missing' was not found.",
    "details": {}
  }
}
```

The ready event's `data` is the same complete snapshot returned by
`system.status`; the shortened object above only illustrates its envelope.

Request IDs may be strings, numbers, booleans, or null. Responses echo them
unchanged. Requests and ticks are serialized on the engine lock because FMOD and
the managers are not concurrently mutated; a long reload or import can therefore
delay a tick. Each input line is read with a hard 1 MiB memory bound. EOF,
`system.shutdown`, Ctrl+C, and Ctrl+Break all trigger orderly cleanup.

## Exit codes

| Code | Meaning |
| ---: | --- |
| `0` | Success |
| `2` | Invalid command syntax or unknown parameter |
| `3` | Invalid value or out-of-range argument |
| `4` | Resource not found |
| `5` | Conflict or invalid runtime state |
| `6` | Invalid configuration |
| `7` | FMOD or audio runtime unavailable |
| `8` | File or transport I/O failure |
| `9` | Timeout |
| `10` | Internal error |
| `11` | Partial success |
| `130` | Interrupted by the user |

The JSON error code is more specific and should be used for program logic. The
process exit code is intended for shells and supervisors.

## Command catalog

Positional syntax and JSON request names are shown together. Named CLI options
use kebab-case; JSON `params` use snake_case.

Boolean CLI flags use `--flag`, `--no-flag`, or `--flag=false`. The space form
`--flag false` is intentionally not used, so a following positional argument is
never consumed as a boolean value.

### Machine-readable parameter contract

Every entry returned by `--schema` and `system.capabilities` contains both its
human CLI positionals and a complete `paramsSchema` for NDJSON/`--request`
integration. `parameterSchemaDialect` identifies JSON Schema Draft 2020-12.
For example, `sound.play` is described in this form (abridged only for display):

```json
{
  "name": "sound.play",
  "positionals": ["id"],
  "paramsSchema": {
    "type": "object",
    "additionalProperties": false,
    "properties": {
      "id": {"type": "string", "minLength": 1},
      "loop": {"type": "boolean", "default": false},
      "volume": {"type": "number", "minimum": 0.0, "maximum": 3.0, "default": 1.0},
      "pitch": {"type": "number", "minimum": 0.25, "maximum": 4.0, "default": 1.0},
      "fade_in": {"type": "boolean", "default": false}
    },
    "required": ["id"]
  }
}
```

Machine clients should send the canonical JSON types declared by
`paramsSchema`. The positional command-line frontend also accepts textual
representations and validates them before execution. Standard JSON Schema
keywords carry their usual meaning:

- `required` identifies mandatory parameters; every other property is optional.
- `default`, `minimum`, `maximum`, `enum`, `minLength`, and `pattern` define
  defaults and accepted values.
- `anyOf`, `oneOf`, and `not` express cross-parameter rules such as “at least
  one mixer value”, “exactly one playlist track selector”, and mutually
  exclusive schedule time forms.
- `additionalProperties: false` means unknown parameters are rejected.

Names beginning with `x-tsm-` are stable TSM annotations layered on JSON
Schema. `x-tsm-format` describes paths or local times,
`x-tsm-defaultSource` and `x-tsm-defaultBy` describe runtime-dependent defaults,
`x-tsm-omitted` describes an inherited/read-only omission behavior, and
`x-tsm-appliesWhen` limits when an otherwise valid option has an effect.
Consumers must ignore unknown annotations and unknown future commands.

### System and configuration

| CLI | JSON command | Parameters |
| --- | --- | --- |
| `status` | `system.status` | none |
| `health` | `system.health` | none |
| `system ping` | `system.ping` | none |
| `system capabilities` | `system.capabilities` | none |
| `system shutdown` | `system.shutdown` | none |
| `config validate [PATH]` | `config.validate` | `path`, `check_files` |
| `config show [PATH]` | `config.show` | `path` |
| `config reload [PATH]` | `config.reload` | `path` |

`system.status` returns the runtime, mixer, load report, active playlist,
announcement sequence, schedules, wedding sequence, cinema calendar, recovery,
and safety state in one snapshot. The
runtime object exposes `bluetooth`, `bluetoothState` (`disabled`, `starting`,
`running`, `failed`, or `stopped`), and a nullable `bluetoothError`; startup is
never reported as successful before RFCOMM is actually listening.

`system.health` returns `critical` and `safeToPlay=false` whenever the cinema
safety or operational gate inhibits normal audio. Integrations should alarm on
that state rather than treating a responsive process as healthy.

### Cinema, safety, and restart recovery

| CLI | JSON command | Parameters |
| --- | --- | --- |
| `cinema status` | `cinema.status` | none |
| `cinema schedules` | `cinema.schedules` | none |
| `cinema preview [AT]` | `cinema.preview` | optional local `at` as `YYYY-MM-DDTHH:MM` |
| `safety status` | `safety.status` | none |
| `safety heartbeat SOURCE SESSION SEQUENCE STATE` | `safety.heartbeat` | `source_id`, `session_id`, `sequence`, `state` |
| `safety trip CAUSE [SOURCE]` | `safety.trip` | `cause`, optional `source`, `play_evacuation` |
| `safety reset INCIDENT OPERATOR REASON` | `safety.reset` | `incident_id`, `operator`, `reason` |
| `session status` | `session.status` | none |
| `session resume` | `session.resume` | none |
| `session discard` | `session.discard` | none |

Safety and session mutations require persistent host mode. `state` is exactly
`safe`, `alarm`, or `fault`; the configured heartbeat source must match exactly,
the session identifies one interlock-adapter boot, and sequence must increase
strictly within that session. Replays never refresh the timeout.

An alarm hard-stops normal audio and repeats the protected evacuation preset. A
fault, missing startup heartbeat, heartbeat timeout, corrupt state, or
persistence failure hard-stops normal audio without playing that preset. Every
incident latches. SAFE does not clear it, `safety.reset` uses the exact incident
ID and an operator audit, and reset intentionally leaves playback suspended
until `session.resume`. `session.discard` removes recovery state without starting
music. `config.reload` is refused while an incident is latched.

The evacuation preset is not exposed as a normal announcement: generic
play/stop/set/seek/unload commands reject it. Bluetooth has no safety commands
and pending Bluetooth work is discarded while the gate is inhibited.

See [CINEMA.md](CINEMA.md) for calendar period semantics, DST/overnight rules,
atomic power-loss recovery, the certified safety boundary, and commissioning
requirements.

### Sounds

| CLI | JSON command | Parameters |
| --- | --- | --- |
| `sound list` | `sound.list` | optional `kind` |
| `sound show ID` | `sound.show` | `id` |
| `sound load ID PATH` | `sound.load` | `id`, `path`, `kind`, `stream` |
| `sound unload ID` | `sound.unload` | `id` |
| `sound play ID` | `sound.play` | `id`, `loop`, `volume`, `pitch`, `fade_in` |
| `sound stop ID` | `sound.stop` | `id`, `fade` |
| `sound stop-all` | `sound.stop-all` | optional `fade` |
| `sound set ID` | `sound.set` | `id`, optional `volume`, `pitch` |
| `sound pause ID` | `sound.pause` | `id` |
| `sound resume ID` | `sound.resume` | `id` |
| `sound seek ID POSITION_MS` | `sound.seek` | `id`, `position_ms` |

Valid sound kinds are `sfx`, `music`, `announcement`, and `wedding`. Music and
wedding sounds use normalization and ducking. Announcements and SFX use their
dedicated mixer buses. `sound.play volume` and `sound.set volume` are local
per-channel gains; master/category/duck gains are applied independently through
FMOD channel groups, so later mixer changes do not erase a channel override.
`sound-effect` is accepted as a compatibility alias for `sfx`; new integrations
should emit the canonical `sfx` value.

`sound.unload` refuses active or referenced resources with `sound_in_use`.
Remove playlist/schedule references and stop playback first. Wedding assets are
replaced with `wedding.asset`, while the sequence is stopped.

### Playlists

| CLI | JSON command | Parameters |
| --- | --- | --- |
| `playlist list` | `playlist.list` | none |
| `playlist show NAME` | `playlist.show` | `name` |
| `playlist create NAME` | `playlist.create` | `name` |
| `playlist delete NAME` | `playlist.delete` | `name` |
| `playlist rename NAME NEW_NAME` | `playlist.rename` | `name`, `new_name` |
| `playlist duplicate NAME NEW_NAME` | `playlist.duplicate` | `name`, `new_name` |
| `playlist add NAME ID` | `playlist.add` | `name`, `id` |
| `playlist remove NAME ID` | `playlist.remove` | `name` and exactly one of `id`, `index` |
| `playlist clear NAME` | `playlist.clear` | `name` |
| `playlist move NAME FROM TO` | `playlist.move` | `name`, `from`, `to` |
| `playlist options NAME` | `playlist.options` | `name`, optional playback values below |
| `playlist import PATH [NAME]` | `playlist.import` | `path`, optional `name` |
| `playlist export NAME PATH` | `playlist.export` | `name`, `path` |
| `playlist save PATH` | `playlist.save` | `path` |
| `playlist load PATH` | `playlist.load` | `path` |
| `playlist play NAME` | `playlist.play` | `name`, optional playback values below |
| `playlist play-index NAME INDEX` | `playlist.play-index` | `name`, `index` |
| `playlist stop [NAME]` | `playlist.stop` | optional `name`; omitted stops all |
| `playlist next NAME` | `playlist.next` | `name` |
| `playlist status [NAME]` | `playlist.status` | optional `name` |

Playback options are `random_order`, `random_segment`, `segment_duration`,
`loop`, and `crossfade`. Only sounds classified as `music` can be added to a
playlist.

### Announcements and daily schedules

| CLI | JSON command | Parameters |
| --- | --- | --- |
| `announcement list` | `announcement.list` | none |
| `announcement load ID PATH` | `announcement.load` | `id`, `path` |
| `announcement unload ID` | `announcement.unload` | `id` |
| `announcement play ID` | `announcement.play` | `id`, `duck`, `sfx_before`, `sfx_after` |
| `announcement stop` | `announcement.stop` | none |
| `announcement status` | `announcement.status` | none |
| `schedule list` | `schedule.list` | none |
| `schedule add ANNOUNCEMENT` | `schedule.add` | `announcement`, either `at` or `hour` + `minute` |
| `schedule update SCHEDULE_ID` | `schedule.update` | `schedule_id`, optional announcement/time values |
| `schedule remove SCHEDULE_ID` | `schedule.remove` | `schedule_id` |
| `schedule reset` | `schedule.reset` | none |

`at` uses local 24-hour `HH:MM`. Schedules have stable numeric IDs during the
session and automatically become eligible again on a new local calendar day.
Provide either `at` or numeric fields, never both. `schedule.update` may change
only `hour` or only `minute`; the other value is preserved.

### Mixer and wedding sequence

| CLI | JSON command | Parameters |
| --- | --- | --- |
| `mixer get` | `mixer.get` | none |
| `mixer set` | `mixer.set` | one or more of `master`, `music`, `announcement`, `sfx`, `duck` |
| `wedding status` | `wedding.status` | none |
| `wedding asset PHASE PATH` | `wedding.asset` | `phase`, `path` |
| `wedding phase PHASE` | `wedding.phase` | `phase`, `transition_to_normal`, `post_playlist` |
| `wedding next` | `wedding.next` | none |
| `wedding stop` | `wedding.stop` | none |

`master`, `music`, and `duck` range from 0 to 1. Announcement and SFX buses
range from 0 to 3. Wedding phases are 1 (entrance), 2 (ceremony), and 3 (exit).
Mixer responses also expose `effectiveDuck`, the product of independent user,
announcement, and wedding duck layers.

### Loudness normalization

| CLI | JSON command | Parameters |
| --- | --- | --- |
| `loudness status [ID]` | `loudness.status` | optional `id` |
| `loudness analyze [ID]` | `loudness.analyze` | optional `id`; omitted queues all music |
| `loudness target` | `loudness.target` | optional `value` from -30 to -8 LUFS |
| `loudness clear-cache` | `loudness.clear-cache` | none |

Analysis is asynchronous. Poll `loudness.status` until each requested item is
`ready` or `failed`. `loudness.clear-cache` cancels and joins pending analysis,
removes cached data, resets normalization gains/statuses, and leaves tracks
eligible for a new `loudness.analyze` request.

## Paths and deployment

The default configuration is searched in this order, based on the actual module
path rather than the host-provided `argv[0]`:

1. `config/tsm_config.json` beside the installed executable;
2. the repository config derived from `build/bin/<Configuration>`.

Relative asset paths inside the configuration resolve from the application root
(the parent of the `config` directory), so callers may use any working directory.
Use `--config PATH` for every other location; the host application's working
directory is never searched implicitly.
Paths passed to import/export/save/load resolve from the caller's working
directory. Windows command-line arguments are read as Unicode and converted to
UTF-8 before parsing.

Reproducible installs omit local media by default and ship a valid empty
configuration. `TSM_INSTALL_ASSETS=ON` is an explicit deployment choice that
installs the source-tree media and populated configuration; callers are
responsible for the corresponding FMOD and media redistribution rights.

The LUFS cache, playback checkpoint, and safety latch are runtime state, not
installation artifacts. They are stored below `--state-dir` when provided;
otherwise TSM uses the current Windows user's local application-data directory
under `TheaterSoundManager`. The operating-system temporary directory is used
only if local application data is unavailable. Runtime state never falls back to
the caller's working directory.

## Compatibility policy

- `schemaVersion` changes only when the JSON envelope becomes incompatible.
- `apiVersion` changes when commands or command semantics evolve.
- Existing fields and commands remain compatible within API major version 1.
- Consumers must ignore unknown response fields and use `system.capabilities`
  rather than assuming optional commands.
- stderr is diagnostic-only and must not be parsed as protocol data.

`--schema` and `system.capabilities` provide the versioned envelope, limits,
exit codes, command catalog, positional mapping, and authoritative machine
parameter schemas. This document adds the behavioral contract for API version 1.

The bundled [Python client](../examples/cli_client.py) demonstrates the complete
subprocess lifecycle without third-party packages. It serializes concurrent
requests, enforces the 1 MiB/non-finite-JSON limits locally, preserves startup
and command failures as `TheaterSoundManagerCommandError` (`code`, `message`,
`details`), and keeps the host alive after ordinary command errors. Its
`state_directory=` constructor argument maps directly to `--state-dir`.
