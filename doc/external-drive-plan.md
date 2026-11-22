# External drive disk tracking plan

Status: implemented and automated validation passed, 2026-11-21. A real external APFS fixture
also passed. Manual rename/unplug, exFAT, and interactive permission/window checks remain open.
See [disk tracking](disk-tracking.md) for implemented behavior and limits.

## Goal

Use the existing disk tracker to see sizes and growth on explicitly selected external
volumes, with accurate capacity figures and history that survives disconnection and
mount-point changes. Keep the current internal-disk experience and existing UI surfaces.

## Current behavior and gap

- `DiskWindow.swift` already lets the user choose arbitrary directories with Add Folder.
- The macOS walker accepts explicit roots, uses allocated bytes, and stays on each root’s
  filesystem (`FTS_XDEV`). An explicitly selected external root can therefore be walked;
  scanning a parent does not automatically include mounted drives beneath it.
- `DiskTracker` currently saves one timestamped snapshot stream for all selected roots,
  and obtains capacity from the general `collect_disk()` collector rather than each root’s volume.
- Snapshots identify entries by absolute path and do not record a volume identity. The
  current model cannot reliably associate history with a drive after its mount point changes.

## Proposed behavior

- Add a drive selector to the existing disk tracker window, including the internal disk
  and drives the user has chosen to track. Show each drive’s free space, scan time,
  progress, and growth report separately. Keep the notch’s existing disk summary on the
  internal disk initially, with an explicit internal-disk label.
- Allow selecting a whole external volume or folders on it. Do not start scanning newly
  attached drives automatically. The first scan remains an explicit action.
- Persist tracked volumes by stable volume identity, with roots relative to the volume’s
  current mount point. Display name and mount path are metadata, never identity.
- Show disconnected, locked, or inaccessible volumes as unavailable, retaining their last
  successful report and its timestamp. Missing data must never become zero usage or deletion.
- Reconnect the same volume to its history after a rename or changed mount point. A reformatted
  volume or another drive with the same name starts separate history. If stable identity is
  unavailable or ambiguous, require explicit re-association rather than matching by name.
- Keep history in local Application Support, so it remains readable while drives are offline.
- Retain the six-hour interval and existing retention rules per tracked volume. Once tracking
  has been started, check for overdue work after reconnect and resume it once available.
  Serialize scans so attaching several drives does not launch parallel full-drive walks.

## Implementation sequence

1. **Introduce volume discovery and identity.** Add a platform-neutral volume descriptor
   and injectable platform interface for identity, mount resolution, availability, and
   capacity. Implement macOS discovery behind that boundary. Distinguish physical external
   local volumes from network mounts and disk images; the first release targets local external
   volumes, including USB and Thunderbolt storage. Keep all scan policy in C++.
2. **Make snapshots and reports volume-aware.** Add a versioned format with volume identity
   and volume-relative paths; obtain capacity for that volume at scan time. Partition
   history, baseline selection, pruning, and reports by identity. Keep legacy snapshots
   readable as legacy internal reports. Do not assign historical external entries to a drive
   by guessing from old absolute paths; begin a fresh external baseline where identity is unknown.
3. **Handle scheduling and removal.** Store per-volume roots, first-scan activation,
   attempts, successful scans, and errors. Resolve and validate identity before scanning and
   again before committing results. Discard incomplete results if the volume disappears or is
   replaced mid-scan; preserve unreadable-folder exclusion for ordinary permission failures.
   Reconcile mounted volumes after wake/reconnect and bound retries for unavailable drives.
4. **Extend the C bridge and existing Swift UI.** Expose tracked-volume list, selection,
   status, and reports. Migrate existing root preferences without losing the internal history;
   resolve connected external roots explicitly and flag unresolved roots for user review.
   Add drive selection and availability labels to the existing window and folder controls.
   Build Finder paths using the currently resolved mount point and disable reveal while offline.
   Handle removable-volume access denial with actionable status in that window.
5. **Verify and document.** Add deterministic tests with injected volume discovery, capacity,
   walker, and clock. Extend bridge tests, run formatting/build/CTest, and run the app smoke
   test after bridge/UI changes. Manually check the window with a real external drive.
   Update architecture, usage, progress, and decisions with what actually shipped.

## Acceptance checks

- Internal tracking still works and existing history is readable after upgrade.
- Two external volumes retain separate capacity totals, baselines, roots, and reports.
- Two same-named drives never share history; volume rename and mount-path changes preserve
  comparisons for the same identity using relative paths.
- Disconnect before or during a scan never records mass deletion or corrupts the last report;
  reconnect resumes overdue tracking without overlapping scans.
- An internal scan does not traverse external mount points, and nested selected roots are
  deduplicated within their volume.
- Locked drives, permission denial, read-only volumes, missing identity, and a full local
  history disk produce recoverable status. A read-only source remains scannable when readable.
- First scan, six-hour scheduling, wake/reconnect, and retention operate independently per volume.
- Check APFS and exFAT on available hardware; record any filesystem coverage not exercised.
  Verify cancellation, progress updates, and app responsiveness on a large drive.

## Scope and limits

Network shares, disk images, automatic enrollment of every attached drive, deletion tools,
physical-device aggregation across partitions, and changes to the general system disk metric
are outside this first implementation. Capacity is per mounted filesystem. Existing allocated-byte
accounting limitations, including shared APFS blocks, remain and should be stated in the UI guide.

This original plan records the intended scope. The disk tracking guide and progress log record
implementation and verification status.
