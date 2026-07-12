# Theater Sound Manager configuration

`tsm_config.json` controls the default playlists, music files, wedding assets,
announcements, announcement schedules, and the LUFS normalization target.

- `loudnessTargetLufs`: perceived loudness target, normally between `-24` and `-10`.
- `playlists[].options.segmentDuration`: default segment duration in seconds.
- `playlists[].tracks`: music IDs and paths loaded into the playlist.
- `wedding`: entrance, ceremony, exit, and transition SFX paths.
- `announcements`: announcement paths and optional `hour` / `minute` schedule.

Paths are resolved from the executable working directory. Missing files are logged
and skipped without preventing the application from starting.

LUFS results are stored in `loudness_cache.json`. The cache entry is automatically
invalidated when a file's size or modification time changes.
