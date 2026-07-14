# Design: Bulk "Sync all" from a Calibre/OPDS library

**Date:** 2026-07-14
**Status:** Approved (brainstorm) — pending spec review
**Author:** Sean Perkins (with Claude)

## Problem

The user keeps their ebook library on a NAS and wants the X4 to pull books over
local WiFi with as little friction as possible — ideally "download all of them"
rather than hand-picking one title at a time.

The X4 already connects to WiFi and already browses/downloads from OPDS servers
one book at a time (`OpdsBookBrowserActivity`). What is missing is a **bulk**
download that grabs everything not already on the device.

## Decisions made during brainstorming

1. **Transport: Calibre content server → OPDS.** The user will install Calibre on
   the NAS and run its content server. This exposes an OPDS feed at
   `http://<nas-ip>:8080/opds`, which the device already understands. No new
   network protocol is added to the firmware.
2. **No SMB/NFS client on the device.** Explicitly rejected. SMB/CIFS session
   state + packet buffers on top of WiFi's TCP buffers is a poor fit for the
   380KB RAM ceiling, and there is no mature lightweight SMB client for the
   Arduino-ESP32 RISC-V target. Calibre/OPDS removes the need entirely.
3. **Trigger: manual "Sync now."** No persistent WiFi, no boot-time network
   probing, no background task. The user initiates each sync. This keeps battery
   and RAM cost identical to today's single-book flow.
4. **Recursive catalog walk.** The sync recurses through OPDS navigation
   sub-catalogs (by author / series / etc.), not just the flat top-level feed,
   because of how the user's library is organized on the NAS.

## Dedup model

"Already have" is defined as: **the target file already exists on the SD card.**

- Target path reuses the *exact* scheme the current single-book download uses:
  `/` + `sanitizeFilename((author.empty() ? "" : author + " - ") + title)` + `.epub`
  (see `OpdsBookBrowserActivity::downloadBook`,
  `src/activities/browser/OpdsBookBrowserActivity.cpp:282`).
- Before downloading each acquisition entry, check `Storage.exists(target)`.
  If present → skip (count as "already have"). Otherwise → download.
- **No manifest, no database, no persistent state.** Zero steady-state RAM cost.
  Books downloaded via the existing per-book flow and via bulk sync are
  interchangeable because they share the naming scheme.

## Architecture

Reuse everything; add a thin bulk driver.

- **Catalog walk (new).** Recursively fetch OPDS feeds starting from the server's
  root (or a chosen sub-catalog). For each feed:
  - Parse entries via the existing `OpdsParser`.
  - **Acquisition entries** (books): collect `{ href, author, title }`.
  - **Navigation entries** (sub-catalogs): recurse into them.
- **Cycle / duplication guards (new).**
  - **Visited-feed set:** a `std::set<std::string>` (or small hash set) of
    catalog feed URLs already walked. Prevents re-entering a feed and infinite
    loops. Holds only *navigation* feed URLs (a handful of categories), never
    per-book entries, so it stays small.
  - **Depth cap:** hard limit on recursion depth (e.g. `MAX_SYNC_DEPTH = 8`) to
    bound stack use and protect against pathological catalogs.
  - Filename-existence dedup (above) also protects against the same book being
    reached via two navigation paths in one run — the first download makes the
    second a skip.
- **Download (reused).** For each new book, call the same
  `HttpDownloader::downloadToFile(url, target, progressCb, nullptr, user, pass)`
  the single-book path already uses. Peak RAM = one download at a time.
- **Memory posture.** Hold one OPDS feed page of entries at a time during the
  walk, not the whole library. No new steady-state allocation.

### Where it lives

Preferred: a new `OpdsSyncActivity` (or a `SyncAll` state inside
`OpdsBookBrowserActivity`) that reuses the browser's download routine. Final
placement decided in the implementation plan; the logic and reuse above are
fixed. Entry point: a **"Sync all"** menu item in the OPDS browser (and/or the
Network menu) for the selected server.

## UX

- **Start:** user picks a saved OPDS server → "Sync all".
- **During:** progress screen reusing the existing progress bar —
  `Syncing 12 / 137 — <title>`. **Back = stop after the current book** (graceful,
  not a hard abort mid-file).
- **Finish:** summary — `Added N, skipped M (already present)`; if any failures,
  `, failed K`.
- Downloaded files land at root `/`, exactly where `FileBrowserActivity` scans
  (`basepath = "/"`, `src/activities/home/FileBrowserActivity.h:33`), so new books
  appear in the library immediately.

## Error handling (matches existing patterns)

- **Per-book failure** → `LOG_ERR`, increment a failure counter, **continue** to
  the next book. One bad title never aborts the whole sync.
- **Partial file on failure** → delete the partial target on any non-OK
  `HttpDownloader` result, so a later retry re-downloads cleanly instead of a
  truncated file being treated as "already have."
- **WiFi drop / server unreachable** → stop the walk, show an error, keep every
  book already downloaded this run.
- **Total counts** are shown at the end regardless of how the run terminated.

## Out of scope (scope discipline)

- ❌ SMB/NFS client on the device.
- ❌ Automatic / background / boot-time sync triggers; persistent WiFi.
- ❌ Two-way sync or reading-progress upload (KOReader sync already covers that
  separately).
- ❌ Metadata/cover import beyond what a downloaded EPUB already contains.
- ❌ Deleting local books that were removed from the server (download-only;
  never destructive).

## Verification (human tester scope)

- Build clean: `pio run -t clean && pio run`.
- On device: point at a Calibre content server with a nested library, run
  "Sync all", confirm: new books download, existing books skip, sub-catalogs are
  walked, Back stops gracefully, summary counts are correct, files appear in the
  library at root.
- Heap: `ESP.getFreeHeap()` stays healthy across a large sync (no steady growth
  proportional to library size — confirms the walk holds one page at a time).
- Re-run "Sync all" immediately: everything skips, zero re-downloads.
