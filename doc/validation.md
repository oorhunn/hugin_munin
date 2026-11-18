# Supported macOS and performance validation

The supported minimum remains macOS 14 on Apple Silicon. A deployment target is build metadata,
not evidence that the app runs there. CI currently builds and runs on macOS 26 only.

## Supported-OS release procedure

On a clean macOS 14 Apple Silicon machine or VM with a desktop session, record the release SHA,
`sw_vers`, `system_profiler SPHardwareDataType` (omit serial numbers), Xcode/Swift version and
signing method. Install the Release artifact being considered for release, not a different rebuild.

1. Run `python3 scripts/check_macos_compatibility.py /path/to/HuginMunin.app`. CI also runs this
   against all four bundled executables. This checks minimum OS metadata, not API availability.
2. Run `scripts/smoke_test.sh /path/to/HuginMunin.app 30`. Check unified logs for launch failures.
3. Run the automated core/bridge tests built from the same SHA on that machine:
   `ctest --test-dir build --output-on-failure`. This needs development tools; `bridge_test`
   temporarily changes the system clipboard. Save anything needed first.
4. Follow the clipboard keyboard and file shelf checklists in [Development](development.md),
   including copy, restore, image preview, persistence after quit, clear, Settings, disk scans,
   unavailable permissions and launch at login. Check System/Disk views and fallback layout in
   a VM; verify the notch on physical hardware separately.
5. Record pass/fail per step, OS/hardware, artifact SHA-256 and any skipped checks in the release
   notes. Repeat on the current OS. macOS 14 runtime checks are **unverified** until this record
   exists. A metadata/host smoke pass must never be reported as a macOS 14 runtime pass.

There is no macOS 14 machine configured in this workspace. No runner label is added without an
available runner. Release reviewers must explicitly record the unverified minimum-OS risk if
shipping without that machine; the existing release workflow does not enforce the manual step.

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
