Freeciv (Replay System Fork)
============================

[![Build Status](https://github.com/freeciv/freeciv/workflows/continuous%20integration/badge.svg)](https://github.com/freeciv/freeciv/actions?query=workflow%3A%22continuous+integration%22)
[![License: GPL v2](https://img.shields.io/badge/License-GPL%20v2-blue.svg)](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html)

This repository is a **fork of Freeciv** with an implemented **Replay System** developed as a project assignment.

Freeciv is a Free and Open Source empire-building strategy game inspired by the history of human civilization.

---

## 🚀 Added Feature: Replay System

This fork adds a complete **recording and replay playback system** for Freeciv game sessions.

### Key Features

### Recording
- Server-side automatic replay recording
- Runtime enable/disable via server setting: `replayrecording`
- Dedicated replay storage directory:
    - `~/.freeciv/replays/`
- Gzip-compressed replay files
- Replay file extension: `.fcreplay`

### Replay Playback
- Deterministic playback using recorded packet streams
- Initial snapshot/bootstrap loading
- Legacy uncompressed replay compatibility
- Replay loading from CLI or in-app browser

### Replay Controls (Qt Client)
- Play / Pause
- Speed control:
    - `0.5x`
    - `1x`
    - `2x`
    - `4x`
    - `8x`
- Frame timeline slider
- Frame counter
- Turn / final turn / year display
- Step backward / forward by turn
- Jump to turn
- Move by signed turn delta

### Replay Browser (Qt Client)
- In-app replay browser dialog
- Replay list with metadata:
    - file name
    - modified date
    - size
    - ruleset
    - scenario
    - turn range
- Replay details:
    - players
    - duration
    - result
    - winner
- Initial minimap preview
- Safe handling of invalid/corrupt replay files

### Observer / Player View Modes
- Full Observer mode (full map)
- Safe Player Focus mode:
    - switch between players
    - camera centers on selected player assets
    - full map remains visible

---

## 🏗️ Project Goals

The goal of this project was to:

- Integrate replay functionality into the existing Freeciv architecture
- Reuse the packet/network model where possible
- Preserve compatibility with existing rulesets
- Avoid regressions in normal gameplay
- Provide a practical replay UI in at least one supported client GUI (Qt)

---

## 🏆 Acceptance Highlights

Validated successfully in this repository state:

- Replay recording and playback end-to-end
- 100+ turn replay sessions
- Replay file size under target limits
- Dedicated replay storage
- Legacy replay compatibility
- Multiplayer replay smoke validation
- Standard ruleset compatibility:

    - `classic`
    - `civ1`
    - `civ2`
    - `civ2civ3`
    - `alien`

---

## ⚙️ Build & Run

Detailed environment/build instructions:

- `AGENTS.md`

### Example Build

```bash
meson setup buildDir -Daudio=none -Dclients=qt -Dfcmp=qt -Dnls=false -Dreplay-recorder=true
meson compile -C buildDir
```

### Run Qt Client

```bash
./buildDir/freeciv-qt
```

### Run Replay Directly
```bash
./buildDir/freeciv-qt --replay /path/to/file.fcreplay
```

---

## 🧪 Testing

Replay functionality was validated with:
* Normal client startup checks
* Replay load/playback checks
* Long replay sessions (100+ turns)
* Ruleset compatibility runs
* Multiplayer smoke tests
* Replay browser metadata/preview checks

Original Freeciv tests should continue to pass without replay regressions.

---

## 📄 Documentation
* `AGENTS.md` — developer / AI-agent workflow notes
* `doc/replay-system.md` — replay architecture and implementation details
* `doc/replay-usage.md`— user guide / operation guide

Original Freeciv documentation remains available in the doc/ directory.

---

## ⚠️ Current Limitations
* No checkpoint-based rewind system
* Seek uses restart + replay-forward strategy
* Browser preview is initial-state only
* No replay database/library management
* No true historical fog-of-war player POV replay mode

---

## 🔗 Original Project Links

Freeciv website: https://www.freeciv.org/

Submit patches: https://redmine.freeciv.org/projects/freeciv

Community forum: https://forum.freeciv.org/

---

## 📌 Notes
* This repository is intended for educational/project purposes
* It is not an official Freeciv distribution
* All original copyrights belong to the Freeciv project