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
meson setup buildDir -Daudio=none -Dclients=qt -Dfcmp=qt -Dnls=false
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
---

## Manual Verification

Recommended flow:

1. Start the server
2. Start the client
3. Connect manually using:
    - host: `localhost`
    - port: `5556`

Important:  
Do not use "New Game" if the server is already running manually. In that case, connect to the existing local server instead.

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

## Recommended Replay Architecture

Replay file structure should be based on:

1. Initial savegame state
2. Ordered stream of gameplay/network packets

Equivalent model:

`replay = initial_state + packet_stream`

This is the preferred design direction unless investigation shows a better minimal solution.

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

- add replay recording toggle / scaffolding
- record packets/events
- write replay files

---

### Phase 4 — Playback

- read replay files
- initialize replay state
- feed recorded events back into the client flow

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

Minimum validation flow:

1. Start the server
2. Connect the client
3. Play at least 20 turns
4. Record a replay
5. Replay the recorded session

If existing tests do not cover replay behavior, add new tests.

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
- local connection currently requires manually starting the server first

---

## Immediate Next Step

The next step is:

Investigate the Freeciv architecture for replay integration without changing code.

The first deliverable should be:

- a short architecture summary
- the key files/modules involved
- 2–3 possible replay integration strategies
- a recommended minimal implementation plan