# Swift formatting and linting

CI checks every tracked `ui/macos/*.swift` file with two pinned tools. A developer runs the same
checks with one script. The pinned versions and rules match CI; OS/toolchain differences can still affect execution.

| Tool | Version | Job |
| --- | --- | --- |
| [SwiftFormat](https://github.com/nicklockwood/SwiftFormat) | 0.64.7 | Layout and whitespace |
| [SwiftLint](https://github.com/realm/SwiftLint) | 0.65.1 | A small set of bug and dead-code rules |

Both tools read Swift 6 sources. The versions, and the SHA-256 of each release archive, are
pinned in `scripts/swift_tools.sh`. The script downloads the macOS release into
`.cache/swift-tools/` (ignored by git), verifies each downloaded archive before unpacking. Existing cached executables are reused;
delete the cache to re-download and verify. Changing a version means changing both the version and the checksum in that script.

## Commands

```sh
scripts/swift_tools.sh check    # what CI runs: fails on any finding, changes nothing
scripts/swift_tools.sh format   # rewrites the files to match .swiftformat, then lints them
scripts/swift_tools.sh install  # only downloads the tools
```

The script lints `git ls-files 'ui/macos/*.swift'`, so a new file is checked once it is tracked
(`git add` it first). It needs `curl`, `unzip`, `shasum`, and an Xcode or Command Line Tools
install, which the portable SwiftLint binary uses for sourcekitd. The script sets `TOOLCHAIN_DIR`
from `xcode-select -p` unless you have already set it. `HM_SWIFT_TOOLS_DIR` moves the download
cache.

## Configuration

`.swiftformat` and `.swiftlint.yml` sit at the repository root.

SwiftFormat uses its default rules with a 4-space indent, no spaces around range operators, and no
trailing commas. These rules are off because the sources already make the opposite choice on
purpose:

- `wrap`, `wrapArguments`, and `wrapMultilineStatementBraces` leave line breaks as written. There
  is no line-length limit.
- `wrapIfStatementBodies`, `wrapPropertyBodies`, `wrapFunctionBodies`, and `wrapSwitchCases` keep
  short bodies on one line.
- `unusedArguments` and `redundantSelf` can change a signature or capture semantics, so the
  compiler and review decide those.
- `numberFormatting`, `docComments`, and `andOperator` produced churn without catching a defect.

SwiftLint runs with `only_rules`, so nothing outside the list is checked, and `--strict` turns
warnings into failures. The list covers likely bugs and dead code: `force_cast`, `force_try`,
`empty_count`, `duplicate_conditions`, `duplicate_enum_cases`, `unused_closure_parameter`,
`unused_enumerated`, `unused_optional_binding`, `unused_setter_value`, `redundant_nil_coalescing`,
`unneeded_override`, `no_fallthrough_only`, `class_delegate_protocol`, `compiler_protocol_init`,
`legacy_multiple`, `unavailable_function`, `discarded_notification_center_observer`,
`notification_center_detachment`, `invalid_swiftlint_command`, `valid_ibinspectable`, and
`xctfail_message`. No layout rule is in the list, so SwiftLint never disagrees with SwiftFormat.

Add a rule only when it finds a real defect in this codebase. To silence one finding, use
`// swiftlint:disable:next <rule>` with a reason.

## Known limits

- `is_disjoint` is not enabled. It flags `OptionSet.intersection(...).isEmpty` in
  `ClipboardBezel.swift` and `FileShelf.swift`. The suggested `isDisjoint(with:)` is equivalent,
  so enable the rule when those three lines are changed.
- SwiftLint's analyzer rules (`unused_import`, `unused_declaration`) need a compiler log and are
  not run.
- The checks do not compile anything. They run no tests and do not replace the app smoke test.
- The pinned binaries are macOS only, so CI runs the Swift job on a macOS runner.
