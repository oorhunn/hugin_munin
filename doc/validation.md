# Supported macOS and performance validation

The supported minimum remains macOS 14 on Apple Silicon. A deployment target is build metadata,
not evidence that the app runs there.

## What is checked where

| Check | macOS 26 | macOS 15 | macOS 14 |
| --- | --- | --- | --- |
| Minimum-OS metadata of all four executables | CI and release | CI and release | Manual, below |
| Core, bridge, and Swift behavior tests | CI and release, built there | CI and release, macOS 26 build run unchanged | Manual, below |
| App stays running after launch | CI and release | CI and release | Manual, below |
| Clipboard, Settings, and disk scan by hand | Manual | Not checked | Manual, below |
| Notch on a MacBook | Manual | Not checked | Manual, below |
| Performance baselines | Manual, local runs below | Not checked | Not checked |

GitHub retired its macOS 14 runners on 2026-11-02, so macOS 15 is the oldest hosted runner.
The macOS 15 job runs the exact Release build that CI made on macOS 26 through
`scripts/downlevel_check.sh`. It does not rebuild anything, so it catches back-deployment failures
in newer APIs and the Swift runtime that a deployment target alone can hide. It is not a macOS 14
result. Treat macOS 14 runtime behavior as **unverified** for any release without the record below.

## macOS 14 release procedure

On a clean macOS 14 Apple Silicon machine or VM with a desktop session, record the release SHA,
`sw_vers`, `system_profiler SPHardwareDataType` (omit serial numbers), Xcode/Swift version and
signing method. Test the Release build being considered for release, not a different rebuild.

1. CTest records absolute paths, so build and test at one fixed path on both Macs. On the build
   Mac, clone the release SHA to `/Users/Shared/hm-release`, then run
   `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release`, `cmake --build build`, and
   `scripts/package_build.sh build build-release.tar.gz`. On the macOS 14 Mac, clone the same
   SHA to the same path and run `tar -xzf build-release.tar.gz` there. That Mac needs CMake and
   the Command Line Tools for `ctest` and `python3`, but no compiler run.
2. Run `scripts/downlevel_check.sh build 30`. It prints the host, checks the minimum-OS metadata,
   runs every test from the prebuilt tree, and checks that the app stays running for 30 seconds.
   `bridge_test` temporarily changes the system clipboard, so save anything needed first. Check
   the unified log for launch failures.
3. Follow the clipboard keyboard and file shelf checklists in [Development](development.md),
   including copy, restore, image preview, persistence after quit, clear, Settings, disk scans,
   unavailable permissions and launch at login. Check System/Disk views and fallback layout in
   a VM; verify the notch on physical hardware separately.
4. Record pass/fail per step, OS/hardware, artifact SHA-256 and any skipped checks in the release
   notes. A macOS 15 CI pass or a metadata pass must never be reported as a macOS 14 runtime pass.

No macOS 14 machine is set up for this project. The release workflow cannot enforce this manual
step, so release reviewers must record the unverified minimum-OS risk when shipping without it.

## Synthetic clipboard core benchmark

This opt-in executable uses a fake pasteboard, a private temporary history file, and never reads
or changes the user's clipboard/history. It measures the watcher at a 0.5-second poll interval,
with capacity 500. Each workload warms up for one second, measures idle CPU and RSS over the
requested duration, then averages ten deep-copy snapshots and three save/load round trips.
The three workloads are empty history, 500 text entries (about 1 KiB each), and 500 opaque image
payloads of 96 KiB each (46.875 MiB total). The image bytes exercise storage and copying;
they are not decodable TIFFs and do not measure Swift image decoding or preview rendering.

```sh
cmake -S . -B build/performance -G Ninja -DCMAKE_BUILD_TYPE=Release -DHM_BUILD_BENCHMARKS=ON
build/hm-run --cpu 2 --memory-mb 2048 --title 'Build benchmark' --project hugin_munin -- \
  cmake --build build/performance --target clipboard_benchmark
build/hm-run --cpu 1 --memory-mb 512 --title 'Clipboard benchmark' --project hugin_munin -- \
  build/performance/clipboard_benchmark 120 > build/performance/clipboard-baseline.jsonl
```

The duration is **per workload**. Repeat three times on idle hardware with the same compiler,
build type, power mode and OS. Record CPU/chip, RAM, OS, compiler, SHA, duration and workload.
CPU is percent of one core (100 means one full core); RSS is process resident bytes. All three
workloads share a process, so allocator retention can carry into later workloads. Save/load
includes snapshot copies and OS filesystem caching; it is not a durable-disk latency benchmark.

## Full app idle and memory growth

Use a disposable macOS user account to avoid real clipboard history and preference changes.
In Settings choose a small fixed scan root and finish the initial disk scan, choose a 1-second
metrics interval, capacity 500 and persistence on, then close Settings and collapse the notch.
Record those settings, scan status, display/notch configuration, and whether other clients are
using the shared service. Warm up for 60 seconds. Sample both the app and `hm-service` separately:

```sh
python3 scripts/sample_process.py APP_PID --seconds 600 --interval 5 \
  --workload 'empty history, 1s metrics, scans complete, notch collapsed' > app-empty.json
python3 scripts/sample_process.py SERVICE_PID --seconds 600 --interval 5 \
  --workload 'same app run, no other clients' > service-empty.json
```

Use `pgrep -x HuginMunin` and `pgrep -x hm-service` to find the PIDs; choose the process under test
if more than one exists. Each JSON records hardware/OS, elapsed time, CPU, sampled peak RSS,
start/end RSS and the RSS time series. Sampling fails if the process disappears. It does not
launch or terminate anything, inspect content, or count child processes in the supplied PID.

Repeat after 500 unique text copies and after 500 distinct valid 96-KiB TIFF images, waiting at
least 0.5 seconds between copies so the watcher observes each. Record actual saved payload size,
entry count, image dimensions/encoding and the fixture-generation method; encoded size affects
persistence while decoded pixels affect UI memory. Exercise the preview through all entries once,
collapse it, warm up again, and measure ten minutes. Repeat three times. Keep fixture production
outside the measurement interval. Real image/UI and ten-minute app baselines remain unverified
until this controlled fixture run has been recorded.

## Baselines and tolerances

The initial local synthetic measurements are recorded below. They are diagnostic,
not regression gates. There is no defensible absolute CPU/RSS budget yet. For later comparisons,
use the median of three runs on the same hardware, flag over 20% latency/CPU change for
investigation (with a 1 ms latency / 0.1 percentage-point CPU noise floor), and inspect RSS series
for sustained growth. These are review heuristics, not CI failure thresholds. Require repeated
measurements and an explained workload before establishing any enforced tolerance.

### Initial local core baseline — 2026-11-17

One exploratory run, 30 seconds idle **per workload** plus one second warmup, on Mac14,10,
Apple M2 Pro (12 CPU cores), 16 GiB RAM, macOS 27.0.1 build 26A434. Apple Clang
21.0.0 (clang-2100.3.34.2), Swift 6.4, Ninja Release, base `09b060706a47c735ff30856f45912b0379c4896c`
plus this issue-cleanup change. The host had constrained memory; these are preliminary values,
not results from the recommended three controlled 120-second runs. Other desktop apps were
running; this is not a machine-idle certification. There is no threshold enforced in CI.

| Workload | Payload bytes | Idle CPU (% one core) | RSS start/end (bytes) | Snapshot mean (ms) | Save/load mean (ms) |
| --- | ---: | ---: | ---: | ---: | ---: |
| Empty | 0 | 0.00522 | 7,094,272 / 5,619,712 | 0.000208 | 1.225 |
| 500 text entries | 513,390 | 0.00515 | 7,831,552 / 5,636,096 | 0.205 | 4.940 |
| 500 image payloads | 49,152,000 | 0.00509 | 58,441,728 / 53,592,064 | 1.792 | 35.390 |

Reproduction used `build/issue-validation/clipboard_benchmark 30` after the Release build.
Resource coordination initially blocked admission at about 84% RAM; the owner explicitly
allowed up to two local build jobs without a lease. The synthetic run itself was one process.
The process sampler also completed a ten-second diagnostic against the already-running app;
its uncontrolled history/scan state makes that unsuitable as an app baseline, so it is omitted.
