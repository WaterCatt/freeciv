# AGENTS.md

## Project

Freeciv fork for implementing a Replay System.

The goal of this project is to add a game recording and playback system to Freeciv.

---

## Project Goals

Implement a system for:

1. Recording game events on the server
2. Saving replay files (`.fcreplay`)
3. Deterministic replay playback
4. Replay control UI in the Qt client
5. Seeking and playback speed control

---

## Development Environment

- OS: Ubuntu
- IDE: CLion
- Agent: OpenCode
- Compiler: GCC / G++
- Build system: Meson + Ninja
- GUI target: Qt6
- Build directory: `buildDir`

---

## Build Instructions

### Configure

```bash
meson setup buildDir -Daudio=none -Dclients=qt -Dfcmp=qt -Dnls=false -Dreplay-recorder=true
```

### Build

```bash
meson compile -C buildDir
```

### Clean Rebuild
```bash
rm -rf buildDir
meson setup buildDir -Daudio=none -Dclients=qt -Dfcmp=qt -Dnls=false -Dreplay-recorder=true
meson compile -C buildDir
```
---

## Run Instructions

### Server

```bash
./buildDir/freeciv-server
```

### Client
```bash
./buildDir/freeciv-qt
```

### Replay Client Smoke Test

```bash
./buildDir/freeciv-qt --replay /absolute/path/to/file.fcreplay --debug 3 -- -platform offscreen
```

This is the current minimal non-UI replay playback entrypoint for validation.

---

## Current Replay Entry Points

### Server-side recorder
Replay recording is currently enabled only when built with:

`-Dreplay-recorder=true`

### Client-side playback
Replay playback is currently started via:

```bash
./buildDir/freeciv-qt --replay /absolute/path/to/file.fcreplay --debug 3 -- -platform offscreen
```
This is currently the main non-UI validation path.

---

## Manual Verification

### Recording Smoke Test

Recommended flow:

1. Start the server:
   `./buildDir/freeciv-server`

2. Start the client:
   `./buildDir/freeciv-qt`

3. Connect manually using:
   - host: `localhost`
   - port: `5556`

4. Start a game

5. Let several turns pass

6. Quit the client normally

7. Quit the server normally by typing:
   `quit`

8. Verify that a fresh `.fcreplay` file exists in the project root:
   `ls -lh replay-*.fcreplay`

### Playback Smoke Test

Run:

```bash
./buildDir/freeciv-qt --replay /absolute/path/to/file.fcreplay --debug 3 -- -platform offscreen
```

### Expected current behavior:

* replay file loads
* snapshot is applied
* event frames are applied sequentially
* replay completes without crashing

### Important:

* For manual server/client testing, connect to the already running local server.
* Do not kill the server process abruptly if you want the replay file finalized correctly.

---

## Installed Dependencies

"sudo apt install -y build-essential git meson ninja-build pkg-config python3 qt6-base-dev qt6-base-dev-tools qt6-tools-dev qt6-tools-dev-tools libicu-dev libcurl4-openssl-dev libsqlite3-dev zlib1g-dev libbz2-dev liblzma-dev libzstd-dev libreadline-dev libssl-dev libfreetype6-dev libharfbuzz-dev libpng-dev libsdl2-dev cmake"

---

## Freeciv Architecture Summary

Freeciv uses a client-server architecture.

Important directories and files:

- `server/` — core game logic
- `client/` — UI code (Qt, GTK, etc.)
- `common/` — shared structures and protocol
- `common/networking/packets.def` — key packet/event definitions
- `server/savegame/savegame2.c` and `server/savegame/savegame3.c` — savegame implementation

---

## Replay-Specific Files (Current MVP)

Important files for the current replay implementation:

- `server/replay/replay.c`
- `server/replay/replay.h`
- `client/replay.c`
- `client/replay.h`
- `server/srv_main.c`
- `server/connecthand.c`
- `client/client_main.c`
- `client/gui-qt/fc_client.cpp`


---

## Replay System Requirements

### Recording

- Record all game events on the server
- Save the initial state
- Record player and game actions in chronological order
- Allow enabling/disabling replay recording

### File Format

- Use a binary `.fcreplay` format
- Support format versioning
- Store replay metadata
- Store incremental changes efficiently

### Playback

- Load the initial state
- Replay recorded events
- Preserve deterministic behavior, including RNG-sensitive behavior
- Support seeking to arbitrary turns

### UI (Qt)

- Play / Pause
- Speed control (0.5x, 1x, 2x, 4x, 8x)
- Timeline / slider
- Current turn and in-game date display

### Observer Mode

- Switch replay point of view between players
- Free-view mode with full map visibility
- Support fog-of-war-free replay inspection

---

## Architecture Constraints

Important constraints:

- Do not break the client-server architecture
- Do not duplicate core gameplay logic
- Avoid large refactors unless absolutely necessary
- Reuse the existing packet infrastructure in `common/networking/packets.def`

---

## Current Replay Architecture

Current implemented MVP architecture is packet-stream based.

Replay file currently contains:

1. File header and metadata
2. `SNAP` chunk with initial bootstrap/snapshot transport frames
3. `EVNT` chunk with subsequent transport frames

The current implementation reuses the normal client packet decode and packet handling pipeline.

Current practical model:

`replay = snapshot_frames + event_frames`

Long-term architecture may still evolve toward snapshot/checkpoint + packet stream, but the current MVP is based on canonical server-generated transport frames recorded from a dedicated replay connection.

---

## Required Development Phases

### Phase 1 — Research

This phase is mandatory before writing replay code.

The agent must first identify:

- where the server sends gameplay packets
- how `packets.def` is structured and generated
- where the client receives and processes packets
- where RNG-sensitive behavior exists
- how save/load is implemented

No replay implementation should begin before this phase is complete.

---

### Phase 2 — Design

The agent should propose:

- `.fcreplay` file format structure
- recording strategy
- playback strategy
- replay-mode integration strategy
- minimal viable implementation plan

---

### Phase 3 — Minimal Implementation

Completed MVP status:
- replay recorder build toggle added
- dedicated server-side replay capture connection added
- replay files are written as `.fcreplay`
- initial snapshot/bootstrap transport frames are recorded
- subsequent event transport frames are recorded

---

### Phase 5 — UI

Not implemented yet.

Planned:
- replay buttons
- play / pause
- speed controls
- replay timeline
- current turn/date display
- 
---

### Phase 5 — UI

- replay buttons
- speed controls
- replay timeline

---

### Phase 6 — Advanced Features

- checkpoints
- rewind / seek
- observer mode
- player switching during replay

---

## Coding Guidelines

- Make small, focused changes
- One commit should correspond to one logical task
- Do not mix build fixes, replay logic, and unrelated refactors
- Reuse existing code whenever possible
- Avoid code duplication
- Prefer minimal and explainable solutions
- Keep replay-specific code isolated where possible

---

## Testing

### Current minimum validation flow

#### Recording
1. Start the server
2. Connect the client
3. Start a game
4. Let several turns pass
5. Quit the client
6. Quit the server with `quit`
7. Verify that a `.fcreplay` file was created

#### Playback
1. Run:
   `./buildDir/freeciv-qt --replay /absolute/path/to/file.fcreplay --debug 3 -- -platform offscreen`
2. Verify that:
   - replay loads successfully
   - snapshot is applied
   - event frames are applied
   - replay completes without crashing
   - turn/year progress during replay

If existing tests do not cover replay behavior, add dedicated replay tests.

---

## Rules for the Agent

The agent must:

1. Start with Plan mode
2. Investigate the code before making changes
3. Propose multiple possible solutions when architecture decisions are involved
4. Choose the smallest viable implementation path
5. Only then begin implementation

---

## OpenCode Workflow

Use:

- Plan → investigation and architecture analysis
- Build → implementation

---

## OpenSpec / SDD Workflow

Use the following sequence:
```
/opsx-explore
/opsx-propose
/opsx-apply  
/opsx-verify  
/opsx-archive
```

This should be used for spec-driven development before major replay work begins.

---

## Error Handling Policy

When something fails:

- inspect the exact error
- do not guess blindly
- avoid broad unrelated fixes
- add logging or debug output when necessary
- document the cause and the fix

---

## Current Project Status

- build works
- server starts
- client starts
- server-side replay recording MVP works
- `.fcreplay` files are generated successfully
- client-side replay loading works
- client-side sequential replay playback works
- replay currently runs without a dedicated replay UI
- replay is currently started via command line using `--replay`

---

## Immediate Next Step

The next step is:

Implement minimal replay controls on top of the working playback loop, without building the full replay UI yet.

The next deliverable should be:

- pause/resume support
- basic replay speed control in code
- single-step-forward support
- validation that replay controls do not break normal live networking