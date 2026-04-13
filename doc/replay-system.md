# Replay System

This document describes the replay system implemented in this Freeciv fork as it exists today.

It is intentionally implementation-focused. It does not describe planned features as if they already exist.

## Scope

Current replay support includes:

- server-side recording of a game into `.fcreplay`
- runtime server setting to enable or disable recording
- dedicated replay storage under the Freeciv user storage directory
- gzip-compressed replay files
- legacy plain replay compatibility
- client-side replay playback in the Qt client
- replay browser UI in the Qt client
- replay metadata display and initial minimap preview in the browser
- exact replay speed levels (`0.5x`, `1x`, `2x`, `4x`, `8x`)
- frame-based replay timeline slider and seek
- turn-based step forward and backward
- jump-to-turn and signed move-by-turn navigation
- safe replay player focus mode while preserving full observer/full-map replay

Current replay support does not include:

- checkpoints
- rewind without replay restart
- true historical fog-of-war replay POV
- replay database/library management
- embedded replay playback inside the browser

## Key Files

- `server/replay/replay.c`
- `server/replay/replay.h`
- `server/srv_main.c`
- `server/settings.c`
- `common/game.h`
- `client/replay.c`
- `client/replay.h`
- `client/gui-qt/fc_client.cpp`
- `client/gui-qt/pages.cpp`
- `client/gui-qt/menu.cpp`

## Architecture Overview

The replay system is packet-stream based.

The server records the normal server-to-client bootstrap and gameplay packets seen by a dedicated replay observer connection. The client later replays those same transport frames through the normal client packet decode path.

In practice:

- the server writes one replay file per recorded game
- the file contains metadata, snapshot/bootstrap frames, and later event frames
- the client loads the snapshot/bootstrap first
- the client then applies event frames in order

This keeps replay behavior close to normal client packet handling and avoids a separate gameplay simulation path inside the replay client.

## Server-Side Recording

Replay recording is compiled in behind the Meson option:

- `-Dreplay-recorder=true`

At runtime, actual recording is controlled by the server setting:

- `replayrecording`

Behavior:

- if the server was built with replay recorder support and `replayrecording` is enabled, the recorder starts when the game becomes ready
- if `replayrecording` is disabled, the game runs normally and no replay file is written
- if the server was built without recorder support, enabling `replayrecording` is rejected by server setting validation

### Dedicated Replay Connection

Recording uses a dedicated internal observer-style connection created on the server.

That connection:

- is local to the server process
- is marked established and observer-capable
- receives the normal bootstrap snapshot and later gameplay packets
- feeds those packets into the replay recorder

This means the replay recorder captures canonical server-generated client traffic rather than a separate custom event model.

## Replay Storage Location

New replay files are written to a dedicated replay directory under the Freeciv storage directory:

- `freeciv_storage_dir()/replays`

On a typical Linux setup this is:

- `~/.freeciv/replays/`

If dedicated replay path creation fails, the recorder falls back to the current working directory.

The Qt Replay Browser scans the dedicated replay directory first and still scans the current working directory as a compatibility fallback for older replay files.

## Replay File Format

Replay files use the `.fcreplay` extension.

### Header

Each replay starts with:

- 8-byte magic: `FCREPLAY`
- 16-bit format version
- 16-bit flags field

Current version:

- version `1`

### Chunks

After the header, the file is chunked.

Each chunk is:

- 4-byte ASCII chunk id
- 32-bit little-endian chunk size
- chunk payload

Current chunk ids are:

- `INFO`
- `SNAP`
- `EVNT`
- `DONE`

### `INFO`

The `INFO` chunk stores replay metadata.

Base metadata:

- Freeciv version string
- capability string
- ruleset name
- scenario name if applicable, otherwise empty string
- starting turn
- starting year
- timestamp

Extended metadata present in newer replay files:

- final turn
- duration in seconds
- player list
- result text, if available
- winner text, if available

The client parses the extended metadata tail when present and safely skips it for older replay files.

### `SNAP`

The `SNAP` chunk stores the initial bootstrap transport frames.

These are the packets needed to bring a client into the initial replay state. In current practice this includes the normal bootstrap packet stream needed for ruleset and map state.

### `EVNT`

The `EVNT` chunk stores subsequent replay frames after the initial snapshot/bootstrap.

These are replayed sequentially during playback.

### `DONE`

The recorder writes a terminal `DONE` chunk with zero payload when finalizing the replay file.

## Compression

New replay files are gzip-compressed while still using the `.fcreplay` extension.

Current behavior:

- the recorder writes the replay stream to a temporary plain file during recording
- when recording stops, the temporary file is gzip-compressed into the final `.fcreplay`

## Legacy Plain Replay Compatibility

The client replay loader supports both:

- current gzip-compressed `.fcreplay` files
- older plain uncompressed `.fcreplay` files

Loading is automatic. The client inspects the file and selects gzip or plain loading accordingly.

## Client Playback Path

Replay playback is implemented in `client/replay.c`.

Current high-level flow:

1. load replay file bytes into memory
2. parse the file header and `INFO`
3. load `SNAP` through the normal client packet pipeline
4. switch the Qt client into the game view
5. continue applying `EVNT` frames over time

The client reuses the normal packet decode path rather than a separate replay protocol.

## Replay Controls

The Qt replay UI currently provides:

- play / pause
- exact speed selection (`0.5x`, `1x`, `2x`, `4x`, `8x`)
- turn/year/final-turn display
- frame counter display
- frame-based timeline slider
- step backward by turn
- step forward by turn
- jump to turn
- move by signed turn delta
- replay focus selector with:
  - `Full Observer`
  - one entry per replay player

### Navigation Semantics

The timeline slider is frame-based.

Current implementation seeks by:

- reset replay to snapshot/bootstrap state
- replay forward until the requested frame is reached

Jump-to-turn and move-by-turn use the same restart-and-replay-forward strategy, but target turns instead of frame positions.

### Step Semantics

Step forward is turn-based, not frame-based.

Current implementation advances replay frames until `game.info.turn` changes.

Step backward is also turn-based, but it is implemented in a simple MVP way:

- determine the previous target turn
- reset replay to the snapshot/bootstrap
- replay forward again until the target turn is reached

This is correct for current MVP needs, but it is not a checkpoint-based rewind system.

### Player Focus Mode

Replay currently supports a safe player focus mode, not true historical player POV.

Current focus behavior:

- `Full Observer` keeps full-map replay and recenters to the map center
- selecting a replay player keeps full-map replay visible
- the camera recenters to that player's current capital, city, or unit
- focus selection persists across replay seek/jump/restart-based navigation

Important limitation:

- replay focus mode does **not** switch the client into that player's live fog-of-war visibility state
- this is intentional, because direct reuse of live per-player visibility state in replay mode was found to be unsafe

## Replay Browser

The Qt client includes an in-app Replay Browser dialog.

Current browser behavior:

- opened from the startup screen `Open Replay...` button
- also reachable from the menu entry `Open Replay...`
- lists `.fcreplay` files from the dedicated replay storage directory first
- also checks the current working directory for legacy replay files

For each replay, the browser shows:

- file name
- modified date/time
- file size
- ruleset
- scenario
- turn range

Selected replay details additionally show, when available:

- players
- result
- winner
- duration

### Browser Preview

The browser preview is a static initial-state minimap preview.

It is generated from replay bootstrap packets only. It does not start a full replay session inside the browser.

Current preview generation uses:

- replay `INFO` metadata
- replay `SNAP` packet decoding for initial map preview data
- early `EVNT` packet decoding when the ruleset produces an empty or incomplete `SNAP` chunk for preview purposes
- ruleset terrain color packets
- map info packet
- tile info packets

The preview is intentionally lightweight and limited to the initial state.

Invalid or corrupt replay files are handled defensively:

- metadata may be omitted or marked invalid
- preview may fall back safely instead of crashing the dialog

## Current Limitations

The following limitations are current and intentional:

- checkpoints are not implemented
- backward stepping is implemented by replay restart plus replay-forward
- frame and turn seek are implemented by replay restart plus replay-forward
- the browser only previews the initial state, not later turns
- the browser does not host an embedded replay player
- there is no replay library database or metadata index
- replay focus mode is full-map only and does not implement historical fog-of-war POV
- unsafe direct player-visibility POV switching is intentionally disabled

## Validation Notes

Useful manual checks:

### Record a replay

1. Start `./buildDir/freeciv-server`
2. Ensure server setting `replayrecording` is enabled
3. Start `./buildDir/freeciv-qt`
4. Connect and play a short game
5. Exit client normally
6. Quit the server normally
7. Confirm a new replay exists in the dedicated replay directory:

```bash
ls -lh ~/.freeciv/replays/replay-*.fcreplay
```

### Playback from CLI

```bash
./buildDir/freeciv-qt --replay /absolute/path/to/file.fcreplay
```

### Playback from UI

1. Start `./buildDir/freeciv-qt`
2. Click `Open Replay...`
3. Select a replay in the Replay Browser
4. Confirm browser metadata and minimap preview update
5. Click `Open`

## Acceptance Status

The following replay acceptance checks have been completed successfully in this repository state:

- replay recording and playback end-to-end
- runtime enable/disable of replay recording
- gzip-compressed replay files
- legacy plain replay compatibility
- dedicated replay storage directory
- replay browser metadata and minimap preview
- frame slider seek
- jump-to-turn and move-by-turn navigation
- exact replay speed levels
- 100+ turn replay validation
- record-and-load validation for standard rulesets:
  - `classic`
  - `civ1`
  - `civ2`
  - `civ2civ3`
  - `alien`
- small real multiplayer replay smoke validation

Remaining notable limitation:

- true historical player POV with fog-of-war reconstruction is not implemented

## Summary

Current replay support is a practical packet-stream implementation built around:

- a dedicated server-side replay observer connection
- `.fcreplay` chunked files with `INFO`, `SNAP`, and `EVNT`
- gzip-compressed replay storage with legacy plain-file compatibility
- dedicated replay storage under the Freeciv user data directory
- client playback through the normal packet decode path
- a Qt replay browser that can inspect metadata, show an initial minimap preview, and launch playback
- replay navigation built around restart-and-replay-forward seek, rather than checkpoints
