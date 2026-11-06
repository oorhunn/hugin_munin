<p align="center">
  <img src="doc/media/icon.png" alt="Two ravens, Hugin and Munin" width="220">
</p>

<h1 align="center">Hugin &amp; Munin</h1>

<p align="center">
  An overall system watcher with a C++23 core and a minimal UI: a notch display and a settings menu.
</p>

<p align="center">
  <a href="https://github.com/oorhunn/hugin_munin/actions/workflows/ci.yml"><img src="https://github.com/oorhunn/hugin_munin/actions/workflows/ci.yml/badge.svg" alt="CI status"></a>
</p>

## Status

The macOS app shows live CPU, memory, disk, and GPU usage side by side around the notch, and
keeps a clipboard history of text and images: hover over the notch (or press **Cmd+Shift+C**) and
click an entry to put it back on the clipboard, then paste with **Cmd+V**. Open **Settings** from
the expanded notch to change the update interval, the metrics beside the notch, the clipboard
history length and its shortcut, and launch at login.

## Build and test

Requires CMake 3.29 or later, Ninja, a C++23 compiler, and Swift 6.

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
open build/HuginMunin.app
```

To quit the app, hover over the notch and click **Quit**.

## Platforms

| Platform | Status |
| --- | --- |
| macOS | Working, tested on macOS 27 |
| Ubuntu | Planned |

## Documentation

- [Architecture](doc/architecture.md): how the core, bridge, and UI fit together
- [Development](doc/development.md): build, test, format, and run sanitizers
- [CI and releases](doc/ci-cd.md): the checks on each change and how to publish a release
- [Progress](doc/progress.md): what has been accomplished so far
- [Decisions](doc/decisions.md): decisions made and open questions
- [CLAUDE.md](CLAUDE.md): project rules for agents and contributors
