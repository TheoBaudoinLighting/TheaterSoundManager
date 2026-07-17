# Smart music selection and exploration

Theater Sound Manager can choose a musically plausible excerpt instead of a
uniform random cut. The feature is opt-in through `automaticSegmentDuration` in
configuration, `automatic_segment_duration` in the CLI, or **Let Theater Sound
Manager choose the segment** in the GUI. The fixed-duration mode remains
available and is the default for compatibility.

## Offline analysis

Each music file is decoded once on FMOD's private no-output analysis system. A
frame is sampled at the actual decoder position approximately every two seconds
and stores:

- local short-term LUFS and linear RMS;
- local true peak and normalized energy;
- spectral centroid and positive spectral flux;
- onset strength, silence probability, and energy slope.

Entry candidates favour stable, audible material without a brutal initial
attack. Exit candidates favour falling energy, low attack density, stability,
breathing space, and phrase-like boundaries. Version 1 intentionally treats
tempo and key as secondary: ambiguous orchestral or themed-area material is
usually matched more reliably by loudness, energy, texture, and onset density.

Results are stored in `music_analysis.sqlite3` below the runtime state directory.
The database is versioned, transactional, integrity-checked, and keyed by the
canonical file path, size, and modification time. A changed file is reanalyzed;
an unavailable database degrades to asynchronous reanalysis rather than
blocking programme playback.

## Segment decision

For automatic segments, the configured values mean:

```text
minimum: hard lower bound, normally 45 s
preferred: Gaussian duration target, normally 150 s
maximum: hard upper bound, normally 240 s
```

The engine first performs a bounded coarse pass over the library, then performs
the detailed entry/exit comparison on at most 32 tracks. Each detailed decision
uses at most 128 entry and 128 exit candidates (with an absolute 512-candidate
safety ceiling), favours the preferred duration, and compares the previous
segment's ending profile with the next entry. These budgets keep the control
thread predictable even with a large cinema library. It draws from the best
candidates with a weighted temperature instead of always choosing the single
maximum. A track shorter than the minimum is played in full. If its offline
analysis is not ready, selection uses a bounded random fallback and reports that
mode explicitly.

The crossfade uses a smoothed equal-power envelope. Automatic overlaps are
capped relative to a short segment, manual skips use at most two seconds, and a
failed incoming or outgoing channel is recovered without deliberately fading
the surviving programme channel into silence.

## Segment fatigue

Every track is divided into ten-second cells. Listening updates a cell's fatigue
within `[0, 1]`; a fully heard cell reaches `1`. Between updates, fatigue decays
continuously as:

```text
fatigue(now) = fatigue(previous) * 0.95 ^ elapsed_days
```

Candidate windows use their overlap-weighted mean fatigue. Low-fatigue windows
receive an exploration bonus, but entry/exit quality remains a guardrail: the
engine will not choose a destructive cut merely because it is novel. This lets a
three-hour area loop expose all 1,080 cells progressively, while a region heard
months ago naturally becomes eligible again.

Track-pair memory applies the same idea to directed transitions. `A -> B` and
`B -> A` are distinct. The store keeps both a lifetime counter and a decaying
fatigue value, so the next-track ranking can prefer a fresh compatible pairing
without throwing away acoustic compatibility.

Playback memory is stored separately in `playback_memory.sqlite3`. Clearing the
LUFS/analysis cache does not erase operating history. This file should be backed
up with other cinema state when continuity across machine replacement matters.
Periodic persistence uses versioned snapshots on a single background writer, so
SQLite I/O never runs in the normal audio-update path. Configuration reload and
clean shutdown drain that writer and synchronously commit any newer mutation;
unloading a track does not discard a dirty snapshot. On the next application
start, the heatmap and directed-transition history are restored before they are
used for selection.

Because SQLite uses write-ahead logging, take a file-level backup only after a
clean application shutdown; an online backup must use SQLite's backup mechanism
rather than copying the main file without its `-wal` state.

## Observability

The now-playing and debug views report the chosen start, end, and duration.
Smart decisions expose entry, exit, acoustic transition, exploration, and total
scores with machine-readable reasons. The track detail view renders the current
fatigue cells as a wrapped colour grid: light cells are available, dark cells
were heard recently.

The CLI exposes the same decision data in `segmentDecision`; playback-memory
inspection and reset are explicit operations so an integration can audit or
commission the exploration policy without editing SQLite directly.

## Operational boundary

Smart selection and playback memory improve normal programme continuity; they
are not life-safety functions. A cinema alarm or safety fault still bypasses
these scores, hard-stops normal programme audio, and follows the protected
evacuation policy documented in [CINEMA.md](CINEMA.md).
