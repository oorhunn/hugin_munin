# Hugin & Munin: agent instructions

Hugin & Munin is an overall system watcher. These rules apply to every agent and contributor
working in this repository. `AGENTS.md` points here, so this file is the single source of truth.

## Layout

- `core/include/hm/`: public headers. `metrics.hpp` is platform-neutral, and `collectors.hpp` is the
  platform boundary.
- `core/src/`: platform-neutral code.
- `core/src/platform/<os>/`: one backend per OS.
- `core/bridge/`: the C header the Swift UI uses to reach the core, and its implementation.
- `ui/macos/`: the SwiftUI app. It builds to `build/HuginMunin.app`.
- `tests/`: `metrics_test` and `sampler_test` need no backend. `collectors_test` and `bridge_test`
  read the live system.
- `doc/`: `README.md` is the index, with the progress log, decisions, and a guide per topic.
- `scripts/`: helpers used both locally and in CI.
- `.github/workflows/`: `ci.yml` checks every change, and `release.yml` publishes tagged releases.

## Build and test

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
open build/HuginMunin.app
```

The tests cover the core and the C bridge, not the Swift UI. After changing `ui/macos/` or the
bridge, launch the app and confirm it stays running, because a Swift concurrency error can pass
every test and still crash the app on launch. `scripts/smoke_test.sh` does this check, and CI runs
it too. CI has no notch, so notch behavior still needs a run on a real MacBook.

Keep C++ formatted, because CI fails otherwise. See `doc/development.md` for the sanitizer presets
and the format command, and `doc/ci-cd.md` for releases.

## Rules

### Documentation

- Update `doc/` as part of every task that accomplishes something. The work is not finished until
  the docs reflect it.
- Add an entry to `doc/progress.md` for what was accomplished, newest first, with an absolute date.
- Record decisions that shape the project in `doc/decisions.md`. Move an open question to the
  decided section once it is settled, and include the reason.
- Keep `README.md` accurate when setup, usage, or project status changes.
- Keep the guides in `doc/` accurate in the same task as the code they describe.
- Only record what actually happened. Note anything skipped, failing, or unverified.

### Language

- Write the project in C++23. The notch UI and settings menu UI are the only exceptions, and they
  use SwiftUI.
- CMake is the build system.
- Keep logic in the C++ core. UI code only displays state and collects settings.

### Platforms

- macOS comes first. Ubuntu support comes later.
- Keep platform-specific code behind interfaces in its own directories, so that Ubuntu support
  means adding an implementation rather than rewriting the core.
- Do not let macOS APIs or types leak into platform-neutral code.

### UI

- The project has two UI surfaces: the notch UI and the settings menu UI.
- Do not add other windows, dashboards, or UI surfaces unless the user asks for them.

### Scope

- Before choosing something listed under "Open questions" in `doc/decisions.md`, confirm it with
  the user.

## Agent resource coordination

When the Hugin & Munin MCP server is configured, register with a name and project before heavy
local work. Request a lease with CPU core equivalents, peak RAM bytes, a unique request key,
a job title, and your application's duration estimate when available. A capacity read alone
is not a reservation. Wait for `running`, then match build/test concurrency to the grant.

Keep leases alive every 30 seconds through the client integration during blocking commands.
Update remaining estimates when they change, obey pressure/yield instructions at safe boundaries,
and release only after work and descendants stop. Unregister when finished. Nested agents must
subdivide a parent grant or request additional capacity; never spend the same budget twice.
Do not invent an estimate when unknown. Do not bypass an unavailable service by assuming unlimited
capacity. See `doc/resource-coordination.md` for the MCP configuration and `hm-run` utility.
