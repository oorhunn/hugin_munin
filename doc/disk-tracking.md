# Disk tracking

Open Disk Tracker from the expanded notch. The Drive selector lists the internal disk,
connected eligible external volumes, and tracked volumes that are offline. Selecting an
untracked drive does not enroll it or read its files. **Track and Scan** explicitly enrolls
that drive and starts its first scan. **Add Drive or Folder…** lets you choose a whole drive
or individual folders; click **Scan Now** to start tracking changes.

Each drive has separate comparison baselines, capacity figures, roots, and scan history. The
period control applies to the selected report. The Disk slide summarizes the internal disk
and shows its last scan time or latest scan error. While a scan runs, a cyan drive icon appears
beside the physical notch. Hover to see the active drive, files counted, and size scanned on
either tab, regardless of the drive selected in Disk Tracker. External scan progress refreshes
through the drive list's two-second polling interval.

The supported external targets are local physical filesystems with
a stable UUID, including USB and Thunderbolt volumes. Network shares, disk images, and
filesystems without a usable UUID are rejected with a message. A read-only source can be
scanned when readable; history is written locally.

## Disconnection and scheduling

The first scan of each newly tracked drive is manual. After tracking starts, scans repeat
every six hours while the app runs. One C++ worker scans drives sequentially. Mount state is
reconciled at least every 15 seconds while idle, so reconnecting an overdue drive resumes its
schedule without scanning drives whose first scan has never been requested. A running walk
can delay reconciliation of other drives.

An offline, locked, unsupported, or ambiguously identified drive is marked unavailable. Its
last saved report remains visible, with its scan timestamp, and Show in Finder is disabled.
A scan checks identity and mount session before and after walking; a detected disconnection
or replacement discards the partial result. An unreadable selected external root also keeps
the previous result, with an instruction to allow access and retry. Unreadable subfolders are
excluded from comparisons using the existing permission-exclusion behavior. These checks
cannot make filesystem enumeration atomic with mount changes or concurrent file writes.

Filesystem UUID, rather than drive name or mount path, associates a volume with its history.
A rename or a different mount path therefore retains comparisons. Reformatting starts a
new history. Two attached volumes with the same UUID, such as cloned media, are unavailable
until the ambiguity is removed; matching by name is never a fallback.

**Folders…** adds or removes roots on the selected drive. Add Folder can select a different
drive and switches to it. Use Defaults restores the default internal folders, or the root of
an already tracked external drive. Stop Tracking Drive removes its preferences and scheduling;
it retains saved history locally. Re-enrolling the same UUID can recover that history.

## Storage and compatibility

History lives in `~/Library/Application Support/HuginMunin/DiskHistory`:

- Existing internal snapshots remain at the top level and keep the version 1 format.
- `volumes.json` records external identities, last resolved names/mounts, relative roots, and
  whether the first scan has been requested.
- `volumes/<hex-encoded UUID>/snapshot-<time>.hmdisk` stores each external history separately.
  Version 2 records its UUID and paths relative to the volume, represented with a leading `/`.

The existing retention policy applies separately to each scanned volume: every scan for three
days, then the newest each day for up to 90 days, always retaining the latest. Pruning follows
a successful save. Unavailable drives retain their last history. Snapshot and configuration
writes use a temporary file and rename. Corrupt configuration is reported and is not silently
overwritten by tracking changes.

Existing UserDefaults roots remain the source of internal preferences. Connected legacy
external roots are resolved and enrolled with a fresh, identified baseline. Unresolved legacy
`/Volumes` roots generate a warning to reconnect and re-add them and are not scanned as internal
folders. Old mixed snapshots stay readable as legacy internal reports; external identity is
never inferred from their old absolute paths.

Sizes use allocated blocks, deduplicate hard links, and retain the existing 10 MiB directory
and 50 MiB file thresholds. APFS clone/shared-block accounting remains approximate. External
capacity uses that mounted filesystem's available blocks; internal capacity retains macOS's
startup-volume capacity calculation. The general system disk metric continues to show the
startup volume. Scans never follow symlinks or descend into other mounted filesystems.

## Verification

Synthetic core tests cover independent histories and capacities, explicit first scans,
sequential scheduling, overdue reconnects, renames/remounts, same-name replacements,
disconnection during a walk, unreadable roots, and duplicate identities. Snapshot tests cover
version 1 compatibility and version 2 identity boundaries. Bridge tests cover metadata, root
updates, owned JSON, and unknown-volume/null handling. The live walker suite checks volume
discovery and internal path resolution.

The one-job build, all 12 CTest suites, changed-source C++ lint, Swift format/lint, and a
10-second app launch smoke test passed on 2026-11-21. A temporary fixture on the connected
external APFS drive completed two scans and reported 12 MiB growth with independent capacity
and history. The external fixture and local test history were removed.

Hardware checks still needed: exFAT external drives; unplug/reconnect and rename;
locked/encrypted and read-only volumes; removable-volume permission denial; a large drive's
progress/responsiveness; and window/Finder behavior. The APFS fixture did not exercise real disconnection or mount-path changes.
