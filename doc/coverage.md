# Core and bridge coverage

The `coverage` CMake preset instruments C++ and Objective-C++ with Clang's source-based coverage
(`-fprofile-instr-generate -fcoverage-mapping`) in a separate Debug build. It does not change the
normal Debug/Release or ASan/UBSan/TSan configurations. Use the selected Xcode/Command Line Tools
compiler and its matching `xcrun llvm-profdata` / `xcrun llvm-cov` readers together.

## Reproduce locally

```sh
cmake --preset coverage
build/hm-run --cpu 2 --memory-mb 2048 --title 'Build coverage tests' --project hugin_munin -- \
  cmake --build build/coverage --target hm_tests --parallel 2
build/hm-run --cpu 1 --memory-mb 1024 --title 'Measure coverage' --project hugin_munin -- \
  python3 scripts/coverage.py
open build/coverage/coverage-report/html/index.html
```

The suite includes `bridge_test`, which temporarily changes the system clipboard. Save anything
needed first. The script runs the C++ suite serially, clears old raw profiles before each run, and
uses `%m-%p.profraw` to distinguish instrumented binaries and subprocesses. The LLVM merge is
limited to two threads. With an existing measurement, `python3 scripts/coverage.py --report-only`
regenerates the report without rerunning tests. `--build-dir PATH` selects another instrumented
build. Do not reuse raw profiles after recompiling; rerun the tests instead.

The report directory contains `html/index.html` with annotated source and true/false branch counts,
`summary.txt`, machine-readable `summary.json`, and merged `coverage.profdata`. HTML colors use
LLVM defaults; they are display watermarks, not project requirements. There is no percentage gate.
If tests fail but produce profiles, the script still generates the report and then fails the job.

## CI

The dedicated **Core and bridge coverage** job builds the instrumented C++ tests, runs the script,
adds the text summary to the job summary, and uploads **core-bridge-coverage** for 14 days, including
on test failure when a report can be generated. Download and unpack it, then open `html/index.html`.
This job runs alongside the existing normal builds, Swift checks and sanitizer jobs.

## Scope and limitations

- An explicit whitelist of tracked `core/**/*.cpp`, `.mm`, `.hpp` and `.h` paths limits reports to
  the core, platform backends and C bridge. Tests, utilities, vendors, system headers and generated
  files are excluded. Utility executables are supplied as coverage objects only because they link
  core functions; their own source is outside the report.
- This is C++/Objective-C++ coverage. `swift_behavior_test` is deliberately excluded here because
  `hm_tests` builds C++ only and Swift cannot link these instrumented libraries without additional
  runtime setup. Normal Debug/Release CI runs the Swift behavior test separately.
- Only code emitted into the instrumented executables has coverage mappings. Uninstantiated
  templates, optimized-away/never-linked code, macros and some compiler-generated branches do
  not produce useful entries. Branch totals are not a count of distinct product behaviors.
- Live backend results and permissions vary by machine. A pre-existing shared `hm-service` may be
  uninstrumented, so that process's internal work does not produce coverage in this run; in-process
  core tests still exercise coordinator logic. The script does not restart a user's service.
- A crashing/killed process may not flush its profile. C++ functions linked into multiple test
  executables can have aggregated execution counts; these are coverage evidence, not call-rate
  or performance measurements. Never compare percentages across different compiler versions
  without examining the source and denominator changes.

## Initial baseline

Measured 2026-11-17 on Apple M2 Pro / 16 GiB, macOS 27.0.1, Apple Clang/LLVM 21.0.0,
Debug `coverage` preset, base `0082c7e` plus the coverage changes. All eleven C++ suite entries
passed (ten C++ executables plus MCP integration); Swift remains a separate normal-build check.
A local pass does not establish that hosted CI has produced an artifact; that remains unverified
until the job runs.

The first report exposed untested failed atomic history-file replacement and the bridge's
multibyte UTF-8 preview limit. Added deterministic replacement failure/cleanup/recovery checks
and a long Unicode preview/complete-restore round trip, then reran coverage. Failure handling
for temporary-file creation, full-disk writes, corrupt/truncated history reads, service IPC and
live backend permissions remains useful follow-up work. Those failure paths need injected I/O
or platform/service fakes rather than unreliable changes to a user's live system.

LLVM reports missing-profile warnings for unexecuted utility `main` functions and unused inline
mapping stubs. An initial `llvm-cov show -dump` inspection found missing records (mostly
zero-hash stubs and excluded vendor mappings), rather than stale source profiles. Raw profiles
were cleared before the build-matched suite run; warnings remain visible instead of being hidden.
These are diagnostic coverage figures with the mapping limitations above, not a regression gate.

Final baseline after those two behavior tests:

| Scope | Lines covered/total | Line coverage | Branches covered/total | Branch coverage |
| --- | ---: | ---: | ---: | ---: |
| All reported core/bridge code | 2240 / 2520 | 88.89% | 869 / 1129 | 76.97% |
| Clipboard core (`clipboard.cpp`) | 440 / 453 | 97.13% | 174 / 198 | 87.88% |
| C bridge (`bridge.cpp`) | 340 / 382 | 89.01% | 67 / 92 | 72.83% |

The summary contained 17 files, all verified under `core/`; no test, utility, vendor, generated
or SDK source appeared. Live backend and IPC coverage are lower and depend on the host/service.
