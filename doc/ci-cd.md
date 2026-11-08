# CI and releases

GitHub Actions runs both pipelines. macOS runners are free because the repository is public.

## CI

[`ci.yml`](../.github/workflows/ci.yml) runs on every pull request, on every push to `main`, and on
demand.

| Job | Runner | What it does |
| --- | --- | --- |
| Format | `ubuntu-24.04` | Checks C++ and Objective-C++ with clang-format 23.1.4 |
| Lint | `macos-26` | Checks C++ with clang-tidy 23.1.4, and fails on any warning |
| Build and test | `macos-26` | Builds the app and tests in Debug and Release, runs the tests, and launches the app |
| Sanitizers | `macos-26` | Runs the tests under ASan with UBSan, and under TSan |

To reproduce a failure, run the same commands from [Development](development.md).

### What CI cannot show

- **The notch.** Runners have no notch, so the launch check exercises the fallback for screens
  without one. Notch behavior still needs a run on a real MacBook.
- **The UI.** The launch check only proves that the app stays running.
- **A real GPU.** A runner may report no GPU, which `collectors_test` accepts.
- **Older macOS versions.** Builds run on macOS 26 only, although the app targets macOS 14.
- **The local toolchain.** Runners use Xcode 26.6, which can differ from the Swift version on a
  development Mac.

### Keeping CI current

- Actions are pinned to commit SHAs, and Dependabot proposes updates weekly.
- Runner labels name an exact version, so an image change never arrives unannounced. Move
  `macos-26` forward by hand.
- When the Ubuntu backend arrives, add a matching job.

## Releases

[`release.yml`](../.github/workflows/release.yml) runs when a tag such as `v0.2.0` is pushed. It
checks that the tag matches the version in `CMakeLists.txt`, repeats the build, test, and launch
steps in Release, and then publishes a GitHub prerelease with the zipped app and its SHA-256 sum.

To publish a release:

1. Set the version in the `project()` call in `CMakeLists.txt`.
2. Add the release to `doc/progress.md`.
3. Commit, tag, and push:

   ```sh
   git tag v0.2.0
   git push origin main v0.2.0
   ```

If the workflow fails, no release is published. Fix the problem, delete the tag, and push it again.

### Limits of these builds

Builds are signed ad hoc, not with a Developer ID, and they are not notarized. macOS therefore
blocks the app after download, which is why releases are marked as prereleases. To run one anyway:

```sh
xattr -d com.apple.quarantine HuginMunin.app
```

Builds are Apple Silicon only, because every Mac with a notch has an Apple Silicon chip.

Signing is an open question in [Decisions](decisions.md).
