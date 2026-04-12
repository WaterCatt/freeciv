# Replay System

This document describes the replay system implemented in this Freeciv fork as it exists today.

It is intentionally implementation-focused. It does not describe planned features as if they already exist.

## Scope

Current replay support includes:

- server-side recording of a game into `.fcreplay`
- runtime server setting to enable or disable recording
- client-side replay playback in the Qt client
- replay browser UI in the Qt client
- replay metadata display and initial minimap preview in the browser
- turn-based forward and backward stepping in replay mode

Current replay support does not include:

- arbitrary seek
- checkpoints
- rewind without restart
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

The server records the normal server-to-client bootstrap and game packets seen by a dedicated replay observer connection. The client later replays those same transport frames through the normal client packet decode path.

In practice:

- the server writes one replay file per recorded game
- the file contains metadata, snapshot frames, and event frames
- the client loads the snapshot first
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

The `INFO` chunk stores replay metadata:

- Freeciv version string
- capability string
- ruleset name
- scenario name if applicable, otherwise empty string
- starting turn
- starting year
- timestamp

This chunk is used by the Qt replay browser for metadata display.

### `SNAP`

The `SNAP` chunk stores the initial bootstrap transport frames.

These are the packets needed to bring a client into the initial replay state. In current practice this includes the normal bootstrap packet stream needed for the initial map and ruleset state.

The replay browser minimap preview also uses selected packets from `SNAP`, specifically enough to derive:

- terrain colors from ruleset terrain packets
- map dimensions from map info
- initial terrain layout from tile info packets

### `EVNT`

The `EVNT` chunk stores subsequent replay frames after the initial snapshot.

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
- speed selection
- turn/year display
- frame progress display
- progress bar
- step backward by turn
- step forward by turn

### Step Semantics

Step forward is turn-based, not frame-based.

Current implementation advances replay frames until `game.info.turn` changes.

Step backward is also turn-based, but it is implemented in a simple MVP way:

- determine the previous target turn
- reset replay to the snapshot
- replay forward again until the target turn is reached

This is correct for current MVP needs, but it is not a checkpoint-based rewind system.

## Replay Browser

The Qt client includes an in-app Replay Browser dialog.

Current browser behavior:

- opened from the startup screen `Open Replay...` button
- also reachable from the menu entry `Open Replay...`
- lists `.fcreplay` files from the current working directory first
- also checks the Freeciv storage directory if available

For each replay, the browser shows:

- file name
- modified date/time
- file size
- ruleset
- scenario
- starting turn/year

### Browser Preview

The browser preview is a static initial-state minimap preview.

It is generated from selected `SNAP` packets only. It does not start a full replay session inside the browser.

Current preview generation uses:

- replay `INFO` metadata
- replay `SNAP` packet decoding for initial map preview data
- ruleset terrain color packets
- map info packet
- tile info packets

The preview is intentionally lightweight and limited to the initial state.

Invalid or corrupt replay files are handled defensively:

- metadata may be omitted or marked invalid
- preview may fall back safely instead of crashing the dialog

## Current Limitations

The following limitations are current and intentional:

- arbitrary seek is not implemented
- checkpoints are not implemented
- backward stepping is implemented by replay restart plus replay-forward
- the browser only previews the initial state, not later turns
- the browser does not host an embedded replay player
- there is no replay library database or metadata index

## Validation Notes

Useful manual checks:

### Record a replay

1. Start `./buildDir/freeciv-server`
2. Ensure server setting `replayrecording` is enabled
3. Start `./buildDir/freeciv-qt`
4. Connect and play a short game
5. Exit client normally
6. Quit the server normally
7. Confirm a new `replay-*.fcreplay` file exists

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

## Summary

Current replay support is a practical packet-stream implementation built around:

- a dedicated server-side replay observer connection
- `.fcreplay` chunked files with `INFO`, `SNAP`, and `EVNT`
- gzip-compressed replay storage with legacy plain-file compatibility
- client playback through the normal packet decode path
- a Qt replay browser that can inspect metadata, show an initial minimap preview, and launch playback
