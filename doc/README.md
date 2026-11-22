# Documentation

Start with the page that matches what you need.

| Page | Read it to |
| --- | --- |
| [Architecture](architecture.md) | Understand how the core, the bridge, and the UI fit together |
| [Resource coordination utilities](resource-coordination.md) | Connect MCP clients, register jobs, and run coordinated commands |
| [Resource coordination plan](resource-coordination-plan.md) | Review the proposed MCP service, agent reservations, and responsiveness safeguards |
| [Disk tracking](disk-tracking.md) | Track internal and external volumes, including offline history |
| [External drive tracking plan](external-drive-plan.md) | Review proposed per-volume disk history and disconnect handling |
| [Development](development.md) | Build, test, format, and run sanitizers locally |
| [Clipboard persistence](clipboard-persistence.md) | Understand storage failure status, retries and recovery |
| [Coverage](coverage.md) | Generate and inspect core/bridge coverage reports |
| [Swift tooling](swift-tooling.md) | Run pinned Swift formatting and lint checks |
| [Compatibility and performance](validation.md) | Reproduce minimum-OS checks and synthetic/full-app measurements |
| [CI and releases](ci-cd.md) | Understand the checks on each change and publish a release |
| [Decisions](decisions.md) | See what has been decided and why, and what is still open |
| [Progress](progress.md) | See what has been accomplished, newest first |

[`CLAUDE.md`](../CLAUDE.md) holds the rules for agents and contributors.

## Keeping these pages current

- `progress.md` and `decisions.md` are updated in every task that accomplishes something.
- The other pages describe how things work now. Change them in the same task as the code they
  describe, and do not keep history in them.
- Only record what actually happened. Note anything skipped, failing, or unverified.
- Images go in `media/`.
