# Theater Sound Manager configuration

`tsm_config.json` controls the default playlists, music files, wedding assets,
announcements, announcement schedules, and the LUFS normalization target.
Source-tree builds use the populated development configuration. Reproducible
packages omit local media by default and install `tsm_config.empty.json` under
the `tsm_config.json` name; enable `TSM_INSTALL_ASSETS` only for an explicitly
licensed deployment that supplies the referenced media.

- `loudnessTargetLufs`: perceived loudness target, normally between `-24` and `-10`.
- `playlists[].options.segmentDuration`: default segment duration in seconds.
- `playlists[].tracks`: music IDs and paths loaded into the playlist.
- `wedding`: entrance, ceremony, exit, and transition SFX paths.
- `announcements`: announcement paths and optional `hour` / `minute` schedule.
- `cinema.schedules`: deterministic date/period-to-playlist calendars.
- `cinema.resume`: atomic crash-checkpoint policy and maximum checkpoint age.
- `cinema.safety`: protected evacuation preset and optional external interlock.

Relative paths are resolved from the application root (the parent of this
`config` directory), independently of the caller's working directory. Missing
optional files are reported. A missing track used by a cinema schedule or a
missing evacuation preset is a startup error and keeps the deployment
fail-closed.

`cinema.schedules[].period.type` accepts `always`, `date`, `date-range`,
`annual-date`, `annual-range`, and `easter-range`. Optional local `window`
endpoints use `HH:MM`; optional `weekdays` use ISO 1 (Monday) through 7 (Sunday).
Higher `priority` wins, then the lexicographically smallest stable schedule ID.
See [the cinema deployment guide](../docs/CINEMA.md) for period boundaries,
overnight/DST behavior, recovery, and examples.

When `cinema.safety.enabled` is true, `evacuationAnnouncementId` is mandatory,
must reference an unscheduled announcement, and is loaded as protected emergency
audio. When `cinema.safety.interlock.enabled` is true,
`expectedHeartbeatSource` is also mandatory. It must exactly match the
`source_id` submitted to `safety.heartbeat`.

By default, LUFS results and cinema state are stored in the current user's local
application-data directory (`TheaterSoundManager`). `--state-dir PATH` selects a
dedicated deployment directory for `loudness_cache.json`, `playback_state.json`,
and `safety_state.json`. The operating-system temporary directory is only a
fallback when local application data is unavailable; the caller's working
directory is never used. Cache entries are automatically invalidated when a
file's size or modification time changes.
