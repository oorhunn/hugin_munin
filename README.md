<p align="center">
  <img src="doc/media/icon.png" alt="Two ravens, Hugin and Munin" width="220">
</p>

<h1 align="center">Hugin &amp; Munin</h1>

<p align="center">
  An overall system watcher with a C++23 core and a minimal UI: a notch display, a settings menu,
  and a disk tracker window.
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

The hover panel also lists registered jobs, their states, and app-supplied time estimates,
with a control to pause new jobs.

Bundled `hm-mcp`, `hm-service`, and `hm-run` utilities coordinate agent resource requests through
one shared per-user service. See [setup and usage](doc/resource-coordination.md). Reservations
and command parallelism hints are cooperative; they are not hard CPU or RAM limits.

Drop documents or folders onto the notch to keep them on the **File shelf**, which only appears
once it holds something. Drag a shelf item to
Finder or another app when ready. Successful moves or copies remove the entry; cancelled drags
keep it. The × button only removes the reference. The shelf is temporary and clears when the app
quits; adding an item leaves its original in place.

## Disk tracker

The disk tracker answers "what used up my free space?". Open **Disk Tracker** from the expanded
notch and click **Scan Now**. After that, swipe the expanded notch or click **Disk** at the bottom of
it for a summary: free space and the three things that grew most. Each scan records the size of your folders, and later scans show what
changed over the last scan, day, week, or month.

- **What used the space** names the projects, folders, and large files that grew. A project is a
  folder with a `.git` entry.
- The headline compares the change in free space with what the scans found, so space used outside
  the scanned folders is reported rather than hidden.
- **Folders** browses the sizes as a tree, and **Large files** lists files of 50 MB or more. The
  right-click menu shows an item in Finder. The tracker never changes or deletes anything.
- **Folders…** chooses what to scan. The default is the home folder, `/Applications`, `/Library`,
  `/opt`, and `/usr/local`.

After the first scan, scans repeat every 6 hours while the app runs, and history stays in
`~/Library/Application Support/HuginMunin/DiskHistory` for up to 90 days. Nothing scans until you
ask for the first one, because macOS asks for permission to read folders such as Documents.
Folders that cannot be read are left out and counted at the bottom of the window. **Allow Access…**
opens Full Disk Access in System Settings.

Sizes are measured as space allocated on disk, and hard links count once. Known limits:

- Changes under 50 MB are not listed, and folders under 10 MB are not recorded, so a change can be
  overstated by up to 10 MB (50 MB for a file) when something crosses that size between scans.
- A moved folder shows as one loss and one gain.
- APFS clones count in full for each copy, because the shared blocks are not visible to a scan.
- Free space is measured on the startup volume, so the headline does not cover a folder on another
  volume.

## Build and test

Requires CMake 3.29 or later, Ninja, a C++23 compiler, Swift 6, and Python 3 for integration tests.

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
