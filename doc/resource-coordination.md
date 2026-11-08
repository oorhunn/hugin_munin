# Resource coordination utilities

The app includes three C++23 utilities: `hm-mcp`, `hm-service`, and `hm-run`. CMake builds them
beside the app and copies them into `HuginMunin.app/Contents/MacOS/`. This is the first working
coordination implementation; reservations are advisory, not OS-enforced CPU or RAM quotas.

## Connect an agent

Configure your MCP client with the absolute path to `hm-mcp`:

```json
{
  "mcpServers": {
    "hugin-munin": {
      "command": "/absolute/path/to/HuginMunin.app/Contents/MacOS/hm-mcp"
    }
  }
}
```

Use `build/hm-mcp` during development. Keep the sibling `hm-service` executable with it.
The adapter implements the 2025-11-25 stdio MCP lifecycle, tools, and read-only resources.
Clients supporting newer protocols must negotiate this supported version. No HTTP port is opened.

Each adapter connects to one per-user service. The app also uses that service for its samples,
so multiple clients cannot create independent reservation pools or duplicate the GPU reader.
The service starts on demand, samples every second, and remains running after the app or adapter
closes. Its socket and singleton lock live in `~/Library/Application Support/HuginMunin/`, with
owner-only directory/socket access and peer-user checks. There is no launch-at-login requirement
for the utilities. Quit closes the UI; it does not cancel registered work.

## Agent workflow

1. Call `register_agent` with `name` and `project`. Keep the returned `agent_token` private.
2. Inspect `get_capacity`, then call `request_lease`. Only the atomic reservation grants work.
3. Supply `request_key` (unique per workload), `title`, `cpu_cores`, `memory_bytes`, and optionally
   `disk_bytes`, `gpu_slots`, and `estimated_duration_seconds`. Reuse the same key only for retries
   of the same workload. CPU units are full-core equivalents; memory/disk units are bytes.
4. If queued, call `get_request` no faster than the returned retry guidance (currently 5 seconds).
   Wait until `state` is `running` before starting expensive local work. Admission happens only
   on an owning-client request/poll; status readers cannot start abandoned queued work.
5. Match tool parallelism to the grant. During execution, call `heartbeat` every 30 seconds.
   Optional `remaining_seconds` replaces the application's remaining-time estimate.
6. Obey `yield_when_safe` by reducing work or finishing the current safe unit before doing more.
7. Stop all work and descendants, then `release_lease`. Finally `unregister_agent`.

Nested agents must request their own additional capacity or subdivide an existing grant, never
count it twice. A remote model request alone does not need a local build-sized reservation.
The integration, rather than the model, must heartbeat during a blocking command.

`list_jobs` returns active jobs; `hugin-munin://status` exposes the same read-only snapshot through
MCP resources. Tool results contain structured JSON and text. Tokens are omitted from snapshots.
Estimated queue wait is explicitly unknown; a supplied duration is not a forecast or guarantee.

## Run commands with registration and automatic heartbeats

```sh
build/hm-run --cpu 2 --memory-mb 1024 --estimate-seconds 90 \
  --title "Build project" --project "my-project" -- cmake --build build
```

The runner waits for a grant, starts a separate process group, lowers the child's priority, and
sets `CMAKE_BUILD_PARALLEL_LEVEL`, `CTEST_PARALLEL_LEVEL`, and `OMP_NUM_THREADS` to the granted
whole-core count (at least one). It renews the lease every 20 seconds, forwards interrupt and
termination signals to the group, and releases after the command and process group exit.
Command output remains on stdout; coordination messages go to stderr.

These environment variables are hints: explicit job flags and arbitrary tools may ignore them.
The runner does not cap memory, enforce GPU utilization, suspend jobs, or kill work because of
pressure. Pressure warnings require cooperation. Processes that deliberately escape their group
cannot be tracked by this runner; do not use daemonizing commands through it.

## Hover over the notch

The expanded panel keeps the four metrics and adds a scrollable registered-jobs list. Each row
shows title, agent, project, state, and the app's duration/remaining estimate. Queued jobs show
estimated duration and “waiting”; running jobs show remaining time. Missing estimates show
“No estimate”; elapsed estimates show “Estimate exceeded”; missing heartbeats show “Heartbeat
overdue”. Estimates update at the display refresh interval and are labeled as supplied by apps.
The footer can pause/resume new admissions without cancelling existing jobs.

## Initial policy and failure behavior

The initial conservative policy admits one active resource job at a time, FIFO. CPU reservations
fit within 65% of cores and measured CPU headroom below 70% utilization. RAM reservations fit
within 70% of physical memory and observed usage plus reservations stays below 80%. Scratch disk
requests leave 10 GiB free. GPU requests use one slot and require known utilization below 50%.
GPU workloads must include their unified-memory demand in `memory_bytes`.

Memory pressure comes from the macOS pressure level. Warning pressure, CPU at 80%, or RAM at
82% constrain new work; critical memory pressure, CPU at 95%, or RAM at 92% trigger critical
state. Recovery needs five normal samples. Missing memory pressure/CPU/fresh memory or a service
sample older than three seconds blocks new admissions. Disk collector failures prevent new disk
reservations. GPU readings may be unavailable; no GPU slot is granted in that case.

Reservations are conservatively counted alongside observed usage because per-job attribution is
not implemented. This can underutilize the machine. Thresholds are implementation defaults,
not calibrated guarantees, and are not yet configurable in Settings. Disk capacity does not
measure disk I/O contention; thermal and swap-rate feedback are not implemented yet.

Leases expire after 90 seconds without renewal. A queued lease is cancelled; a running lease
becomes unresponsive and retains its budget until its owner confirms work stopped and releases
it. Unregistering does not silently free running work. This avoids admitting another workload
while an orphan process still consumes resources. Unknown/failed service calls never grant work.

State is in memory. Restarting the service invalidates tokens and loses the registry; actual
system load still constrains admission, but process reconciliation across restart is not yet
implemented. Stop registered work before manually restarting the service, then re-register.
Idle registrations are pruned after five minutes. History is bounded to 512 jobs and 128 agents;
old completed records are evicted when needed, so idempotency lasts only while retained.

## Verification and remaining work

`coordinator_test` checks shared accounting, idempotency, ownership, estimates, expiry/orphan
holds, pause, freshness, and pressure recovery with injected time. `mcp_integration` exercises
two actual stdio clients against the shared service and checks discovery, job visibility,
ownership, estimate updates, resources, and malformed JSON. Existing sampler tests verify
collector freshness. The bridge test and app smoke test cover startup and callback lifetime.

Future work from the accepted plan includes machine-specific calibration, configurable budgets,
per-process attribution, durable recovery, lease resizing/minimum offers, queue wait forecasting,
priority aging, disk I/O/thermal/swap-rate signals, and stronger platform-specific process controls.
