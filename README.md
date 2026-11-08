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
keeps a clipboard history of text and images that works like Flycut. Press **Cmd+Shift+V** to
open the preview in the middle of the screen, keep **Cmd+Shift** held, and tap **V** to walk to older entries.
Release the modifiers to paste the selection. The arrows, **J / K**, **Home / End**,
**Page Up / Page Down**, and **1** to **0** also move through the history, **Return** pastes,
keypad **Enter** copies without pasting, **Delete** forgets the entry, and **Escape** cancels.
Pasting keeps the history order, and the next preview opens where the last one ended until you
copy something new. Scrolling over the preview also moves through the history, and a double click
pastes. The history is saved between launches, without encryption and readable only by your
account. Enable Accessibility through **Settings → Automatic paste**. Without permission, choosing
an entry copies it for **Cmd+V**. Copies marked private by password managers are not recorded.
The preview background is 70% opaque (30% transparent). Open **Settings** from
the expanded notch to change the update interval, the metrics beside the notch, and launch at
login, and to set the clipboard's history length, shortcut, saving, automatic paste, and clearing.

Drop documents or folders onto the notch to keep them on the **File shelf**. Drag a shelf item to
Finder or another app when ready. Successful moves or copies remove the entry; cancelled drags
keep it. The × button only removes the reference. The shelf is temporary and clears when the app
quits; adding an item leaves its original in place.

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
