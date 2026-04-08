Freeciv (Replay System Fork)
============================

[![Build Status](https://github.com/freeciv/freeciv/workflows/continuous%20integration/badge.svg)](https://github.com/freeciv/freeciv/actions?query=workflow%3A%22continuous+integration%22)
[![License: GPL v2](https://img.shields.io/badge/License-GPL%20v2-blue.svg)](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html)

This repository is a **fork of Freeciv** with an implementation of a **Replay System** developed as a project assignment.

Freeciv is a Free and Open Source empire-building strategy game inspired by the history of human civilization.

---

## 🚀 Added Feature: Replay System

This fork introduces a **game replay system**, allowing recording and playback of game sessions.

### Key features:
- Server-side recording of gameplay
- Efficient binary replay format (`.fcreplay`)
- Storage of initial game state using existing savegame system
- Deterministic playback of recorded sessions
- Playback controls (Play/Pause, speed control)
- Turn-based navigation (seek/jump using checkpoints)
- Replay browser in client (Qt)
- Observer mode support (view from different players / free view)

---

## 🏗️ Project Goals

The goal of this project is to:

- Integrate replay functionality into existing Freeciv architecture
- Preserve compatibility with existing rulesets and save/load system
- Maintain stability of original functionality
- Make the project **agent-friendly** for AI-assisted development (Codex)

---

## ⚙️ Build & Run (Windows)

See detailed instructions in:
- `AGENTS.md` — environment setup, build, run, and test instructions

---

## 🧪 Testing

- Original Freeciv tests must pass without regressions
- Additional replay-related tests are included
- Replay functionality is tested with automated scenarios and long games (100+ turns)

---

## 📄 Documentation

- `AGENTS.md` — instructions for AI agents and developers
- `doc/replay-system.md` — replay system architecture
- `doc/replay-format.md` — replay file format specification

Original Freeciv documentation is available in the [doc](doc) directory.

---

## 🔗 Original Project Links

Freeciv website: [Freeciv.org](https://www.freeciv.org/)  
Submit patches: [redmine.freeciv.org](https://redmine.freeciv.org/projects/freeciv)  
Community forum: [forum.freeciv.org](https://forum.freeciv.org/)

---

## ⚠️ Notes

- This repository is intended for educational purposes
- It is not an official Freeciv distribution
- All original copyrights belong to the Freeciv project
