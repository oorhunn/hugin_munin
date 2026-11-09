# Plan: MCP resource coordination

Status: direction accepted by the owner, 2026-11-06. The first implementation adds app utilities,
shared reservations, MCP, a command runner, and registered jobs in the hover panel. See
[Resource coordination utilities](resource-coordination.md) for current behavior and limitations.
The staged design below includes future work; it does not claim every stage is complete.

## Goal

Keep the computer responsive during agent development work. Agents register with Hugin & Munin,
request a budget before expensive local work, adapt concurrency to the granted budget, and report
completion. Preserve room for the OS, foreground apps, and unexpected load.

Success means controlled development workloads coexist with normal use.

## Starting point

The C++23 core already collects CPU utilization, memory and swap totals, startup-volume capacity,
and GPU utilization. `Sampler` and `SampleTracker` supply periodic samples; the C bridge feeds
the SwiftUI notch. Injected collectors and clocks support deterministic tests.

These are monitoring primitives, not allocation budgets. In particular:

- Disk capacity does not measure disk I/O contention.
- Used RAM alone does not describe memory pressure or swap churn.
- GPU utilization can be unavailable and has a documented interference issue with other readers.
- Memory and disk values may be retained after a collector fails; admission needs freshness.
- CPU idle time is a momentary observation, not a promise of future capacity.

## Proposed architecture

One per-user C++ service owns sampling, policy, registrations, the queue, and reservations.
All agent connections share this authority. The notch reads its state through the existing bridge,
with the bridge becoming a client of the service. Closing the UI should not erase reservations.
Keep platform APIs behind the existing platform boundary.

Proposed components:

- `core/include/hm/` and `core/src/`: pressure state, resource budgets, lease accounting, queue,
  and workload history. Pure C++ policy with injected time and observations.
- `core/src/platform/macos/`: pressure, thermal and process observations; process launch controls
  behind interfaces. Ubuntu gets its own implementation later.
- A service executable: singleton lifecycle and private local IPC, independent of the UI.
- A small MCP executable per client: stdio adapter forwarding to the shared service.
- A command runner: starts approved workloads and tracks their process trees.
- Existing notch and settings: pressure status, agent count, queued work, pause/resume admissions,
  protected capacity, and policy selection. No additional UI surface.

Stdio MCP clients launch their server as a subprocess. Therefore independent adapters must not
create independent reservation pools. Use a private Unix-domain socket and same-user access
checks for the shared service; stdout remains protocol-only. Start with stdio, leaving HTTP out
of the first release. Select a maintained C++ MCP implementation, or a narrowly scoped protocol
implementation, after checking compatibility and dependencies; this plan does not select one.

## Resource model and admission

Separate physical measurements, protected system headroom, registered reservations, and observed
workload consumption. Track CPU in core equivalents (one core at full use), RAM in bytes, disk
scratch capacity in bytes, GPU-heavy slots initially, and an I/O-heavy concurrency class.
Do not present I/O slots or GPU slots as hardware-enforced limits.

Admit a request only if its entire resource vector fits. The decision and reservation update
must be atomic. Count the still-unused portion of existing reservations against measured
headroom, avoiding double counting their observed use. Where attribution is unreliable, use a
conservative documented bound. Include unregistered workloads in system pressure calculations.
On Apple Silicon, GPU allocations also compete for unified memory; reserve their RAM demand.

Use configurable normal, constrained, and critical pressure states with hysteresis and recovery
cooldowns. Under constrained conditions, stop admitting heavy work and request smaller parallelism.
Under critical pressure, stop new heavy work and ask active clients to yield; managed workloads
can be reduced at supported boundaries. Unavailable or stale essential readings block new heavy
admissions until fresh observations recover. Keep lightweight status access working.

Start conservatively with one heavy workload at a time and configurable system headroom. Tune
CPU/RAM reserves on the actual Mac using measurements rather than declaring a universal safe
percentage. Keep service sampling independent of the user's display refresh preference. Sampling
and logging must remain bounded and cheap even when the host is overloaded.

## Proposed MCP contract

| Tool | Purpose |
| --- | --- |
| `register_agent` | Create a session with project, parent agent, capabilities, and identity metadata; return an opaque session credential and heartbeat policy |
| `get_capacity` | Return measured headroom, reservable budgets, pressure, freshness, limitations, and active constraints |
| `request_lease` | Atomically grant, offer a smaller budget, queue, or reject a workload request |
| `get_request` | Check a queued request with bounded retry guidance; no tight polling |
| `heartbeat` | Renew session/lease, report progress, get current budget and yield instructions |
| `update_lease` | Atomically request a resize or revise remaining duration; increases require admission |
| `release_lease` | Idempotently finish or cancel work and record actual duration |
| `unregister_agent` | End registration and reconcile its queued and running work |

Requests include session, idempotency key, task category, desired and minimum budgets, expected
duration, priority, and whether the workload can shrink or yield. Responses provide machine-readable
reasons, lease/request IDs, expiry, granted resources, suggested worker count, retry guidance,
and an explicit advisory or managed enforcement mode. Bind state mutations to the owning session.
Nested agents share a parent budget or request additional capacity explicitly; they cannot each
reuse the parent's full grant. Starting at a smaller offered budget requires explicit acceptance.

Offer read-only MCP resources for a capacity snapshot, policy, and active workloads. Notifications
can improve responsiveness when supported, but heartbeat/status responses remain sufficient for
correct operation. MCP resource discovery must not be confused with atomic resource reservation.

## What “available for how long” means

Return separate fields for measurement time and age, how long pressure has persisted, lease
expiry, expected workload completion, and estimated queue wait. Mark unknown values explicitly.

A lease is permission to work within a budget until its expiry, subject to pressure changes; it
is not a guarantee that the OS reserves hardware. Estimate queue waits from declared remaining
time, observed task history, queue order, and all relevant resource bottlenecks. Report a range,
confidence, and assumptions. Without enough evidence, report unknown plus a retry interval.
A queued job receives no executable grant until admission is confirmed.

Use monotonic time for expiry and wall time for human-visible timestamps. Apply priority with
aging and bounded renewals so a long workload cannot starve everyone else. Reject requests that
cannot fit the configured budget rather than leaving them queued forever.

## Agent behavior and enforcement

The required workflow is register, request, wait for a grant, run with the granted parallelism,
heartbeat, adapt, release, unregister. Put it in tool descriptions, repository instructions, and
client integration examples. Agents should reduce build jobs, test workers, browser instances,
and local model concurrency; waiting on a remote model does not itself need a heavy local lease.

MCP instructions alone cannot prevent an agent from ignoring the budget or calling its own shell.
For dependable coordination, provide a runner that requires a valid lease and applies tool-specific
concurrency plus appropriate macOS process priority. Priority is a scheduling hint, not a hard
CPU/RAM cap. Investigate supported controls before promising enforcement parity with Linux.
Avoid automatic killing or indiscriminate suspension: a suspended process retains memory and may
hold locks. Prefer graceful completion, cooperative cancellation, and controlled restart.

Leases expiring must not make still-running work disappear from accounting. Track managed process
identity beyond a PID, including children and process exit; retain orphan workload accounting
until termination is verified. Missing heartbeats stop new grants to that session and trigger
reconciliation. For untracked workloads, retain a conservative hold and rely on system pressure.
On service restart, invalidate old lease credentials, reconcile surviving managed work, and block
heavy admissions until fresh samples and reconciliation complete. A heartbeat must come from the
runner/client integration during a long tool call; do not depend on the model taking another turn.

For an unavailable service, integrations should defer heavy managed work and report the problem,
while allowing lightweight coordination/recovery operations. Never silently assume unlimited capacity.

## Delivery sequence

1. **Baseline and observability.** Add per-metric validity, timestamps, memory pressure, swap/page
   activity rates, thermal state where available, and disk I/O signals. Record a bounded event
   history. Correlate realistic development loads with system responsiveness.
   Exit: stale/failed inputs are explicit and diagnostics can explain pressure decisions.
2. **Policy and reservation core.** Implement registrations, atomic multidimensional admission,
   leases, queue fairness, expiry, resize, and pressure recovery with fake time and collectors.
   Exit: simultaneous requests cannot oversubscribe the configured pool; crash cases are covered.
3. **Shared service and MCP.** Add per-user IPC, singleton ownership, stdio adapters, schemas,
   tools/resources, and client setup documentation. Exit: two independent MCP clients see the
   same pool, and reconnects/retries do not duplicate reservations.
4. **Managed workloads and agent integrations.** Add the runner and integrations for a build/test
   workflow. Track descendants, enforce configured parallelism, and deliver budget changes during
   long calls. Exit: a real command obeys its grant and orphan accounting remains conservative.
5. **Notch/settings integration and calibration.** Connect the UI to the shared authority, show
   queued/active work and pressure reasons, add controls within existing surfaces, and tune the
   conservative policy on this Mac. Exit: normal use remains responsive during representative
   concurrent development tasks, with measured responsiveness and explicit remaining limitations.
6. **Later work.** Workload-duration learning, richer GPU policies, additional client integrations,
   and Ubuntu collectors/process controls. Do not delay the basic CPU/RAM coordination for these.

## Validation

Use deterministic policy tests for concurrent admission, minimum offers, multi-resource fits,
double-counting, parent budgets, fairness, stale inputs, renewals, expiry with live work,
PID reuse, cancellation, restart, and pressure hysteresis. Exercise protocol schema errors,
ownership checks, idempotency, disconnected clients, bounded logs, and service singleton races.

Run bounded real build/test workloads with multiple agents, foreground use.
Record granted versus observed load, memory pressure, swap activity, queue delay, sampler delay,
and service overhead. Establish machine-specific acceptance targets from a
baseline before claiming success. Avoid intentionally reproducing a complete system overload.
Run the project's CMake/CTest checks for implementation changes, and its app smoke test plus a
real Mac notch check after bridge/UI changes. This planning task does not run these checks.

## Risks and future implementation choices

Recommended default: conservative shared service, stdio adapters, advisory leases first, then
managed execution before describing the system as protection against runaway workloads.
Further calibrate strictness, protected headroom, and behavior for noncooperating agents as
implementation progresses. No existing release-signing or Ubuntu UI open questions are resolved here.

The largest limitation is adoption: clients bypassing the runner remain advisory. Resource
workloads that underestimate RAM may still produce pressure before the next sample. Conservative starting concurrency,
feedback, and measured calibration reduce that risk.

## References checked for this proposal

- [MCP stdio transport](https://ts.sdk.modelcontextprotocol.io/v2/serving/stdio): clients launch
  server subprocesses; this motivates one shared authority behind adapters (our design inference).
- [Apple: Scheduling CPU work efficiently](https://developer.apple.com/documentation/xcode/scheduling-cpu-work-efficiently):
  appropriate quality of service and event-driven work help preserve responsiveness.
- [Apple: Improving app responsiveness](https://developer.apple.com/documentation/xcode/improving-app-responsiveness):
  background work and suitable priorities support responsive apps; they do not establish hard quotas.
