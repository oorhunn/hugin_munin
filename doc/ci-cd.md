# CI and releases

GitHub Actions runs both pipelines. macOS runners are free because the repository is public.

## CI

[`ci.yml`](../.github/workflows/ci.yml) runs on every pull request, on every push to `main`, and on
demand.

| Job | Runner | What it does |
| --- | --- | --- |
| Format | `ubuntu-24.04` | Checks C++ and Objective-C++ with clang-format 23.1.4 |
| Swift format and lint | `macos-26` | Checks tracked Swift files with pinned SwiftFormat and SwiftLint |
| Lint | `macos-26` | Checks C++ with clang-tidy 23.1.4, and fails on any warning |
| Build and test | `macos-26` | Builds the app and tests in Debug and Release, runs the tests, checks minimum-OS metadata, and launches the app |
| Run the Release build on macOS 15 | `macos-15` | Unpacks the Release build from the job above, without rebuilding it, then runs its tests, metadata check, and launch check with `scripts/downlevel_check.sh` |
| Core and bridge coverage | `macos-26` | Runs instrumented C++/Objective-C++ tests, posts a summary and uploads HTML/JSON coverage |
| Sanitizers | `macos-26` | Runs the tests under ASan with UBSan, and under TSan |

To reproduce a failure, run the same commands from [Development](development.md).

### What CI cannot show

- **The notch.** Runners have no notch, so the launch check exercises the fallback for screens
  without one. Notch behavior still needs a run on a real MacBook.
- **The UI.** The launch check only proves that the app stays running.
- **A real GPU.** A runner may report no GPU, which `collectors_test` accepts.
- **macOS 14.** The app targets macOS 14, but GitHub retired its macOS 14 runners on
  2026-11-02. CI runs the macOS 26 Release build on macOS 15, the oldest hosted runner, which
  catches back-deployment failures in newer APIs and the Swift runtime but is not a macOS 14
  result. macOS 14 stays a manual release check in [Validation](validation.md).
- **The local toolchain.** Runners use Xcode 26.6, which can differ from the Swift version on a
  development Mac.

### Keeping CI current

- Actions are pinned to commit SHAs, and Dependabot proposes updates weekly.
- Runner labels name an exact version, so an image change never arrives unannounced. Move
  `macos-26` forward by hand. When GitHub retires `macos-15`, move the older-OS job to the
  oldest label still offered and update this page.
- When the Ubuntu backend arrives, add a matching job.

See [Compatibility and performance validation](validation.md) for the executable macOS 14
release checklist and performance workloads. CI metadata checks cannot establish older-OS
runtime behavior. Synthetic core measurements and controlled full-app measurements are recorded
separately; no performance gate has been established yet.

## Releases

[`release.yml`](../.github/workflows/release.yml) runs when a tag such as `v0.2.0` is pushed. It
checks that the tag matches the version in `CMakeLists.txt`, checks minimum-OS metadata, repeats the build, test, and launch
steps in Release, runs that same build's tests and launch check on `macos-15`, and only then
publishes a GitHub prerelease with the zipped app and its SHA-256 sum. The publish job is the only
one with write access, and it checks the checksum before uploading.

To publish a release:

1. Set the version in the `project()` call in `CMakeLists.txt`.
2. Add the release to `doc/progress.md`.
3. Commit, tag, and push:

   ```sh
   git tag v0.2.0
   git push origin main v0.2.0
   ```

Run the manual macOS 14 check in [Validation](validation.md) before tagging, or record in the
release notes that it was skipped.

If the workflow fails, no release is published. Fix the problem, delete the tag, and push it again.

### Limits of these builds

Builds are signed ad hoc, not with a Developer ID, and they are not notarized. macOS therefore
blocks the app after download, which is why releases are marked as prereleases. To run one anyway:

```sh
xattr -d com.apple.quarantine HuginMunin.app
```

Builds are Apple Silicon only, because every Mac with a notch has an Apple Silicon chip.

Signing is an open question in [Decisions](decisions.md).
