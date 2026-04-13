# Replay Usage Guide

This is a short operator and demo guide for the replay system as implemented in this repository.

For architecture and format details, see `doc/replay-system.md`.

## Build Replay Support

Configure with replay recording enabled:

```bash
meson setup buildDir -Daudio=none -Dclients=qt -Dfcmp=qt -Dnls=false -Dreplay-recorder=true
```

Build:

```bash
meson compile -C buildDir
```

## Enable or Disable Replay Recording

Replay recording is controlled at runtime by the server setting:

- `replayrecording`

Example in the server console:

```text
set replayrecording enabled
```

Disable it with:

```text
set replayrecording disabled
```

If the server was not built with replay recorder support, enabling the setting is rejected.

## Where Replay Files Are Stored

New replay files are written to:

- `~/.freeciv/replays/`

File naming remains:

- `replay-YYYYMMDD-HHMMSS.fcreplay`

Older replay files stored in the source tree or another working directory can still be opened explicitly and are still scanned by the Replay Browser as a fallback.

## Open Replay Files from the Qt UI

### Startup Screen

1. Start the Qt client:

```bash
./buildDir/freeciv-qt
```

2. Click `Open Replay...`
3. Select a replay in the Replay Browser
4. Click `Open`

### Menu Entry

The same browser is also available from:

- `Game -> Open Replay...`

## Replay Browser

The Replay Browser shows:

- file name
- modified time
- file size
- ruleset
- scenario
- turn range

The details pane can also show:

- players
- result
- winner
- duration

The minimap preview is a static preview of the replay's initial map state.

## Replay Controls

Current replay controls provide:

- `Play` / `Pause`
- speed selector:
  - `0.5x`
  - `1x`
  - `2x`
  - `4x`
  - `8x`
- frame slider for frame-based seek
- frame counter label
- turn/year/final-turn status label
- `Step -` and `Step +` for turn-based stepping
- `Jump` to an absolute turn
- `Move` by signed turn delta
- replay focus selector

## Full Observer vs Player Focus

### Full Observer

- full map remains visible
- safe default replay mode
- selecting it recenters the map to the map center

### Player Focus

- still keeps full map visible
- does **not** enable true player fog-of-war
- recenters the camera to the selected player's current capital, city, or unit
- persists through replay seek/jump/restart-based navigation

This is intentionally a safe focus mode, not true historical player POV.

## CLI Replay Playback

Explicit replay loading is unchanged:

```bash
./buildDir/freeciv-qt --replay /absolute/path/to/file.fcreplay
```

Useful smoke command:

```bash
./buildDir/freeciv-qt --replay /absolute/path/to/file.fcreplay --debug 3 -- -platform offscreen
```

## Current Limitations

- no checkpoints
- seek uses replay restart plus replay-forward
- backward stepping also uses replay restart plus replay-forward
- browser preview is initial-state only
- no true historical fog-of-war player POV
- no replay database/library management

## Acceptance Highlights

Validated successfully in the current repository state:

- replay recording and playback
- dedicated replay storage directory
- gzip-compressed replay files
- legacy replay compatibility
- replay browser metadata and minimap preview
- 100+ turn replay record and playback
- standard ruleset record-and-load validation:
  - `classic`
  - `civ1`
  - `civ2`
  - `civ2civ3`
  - `alien`
- small real multiplayer replay smoke validation
