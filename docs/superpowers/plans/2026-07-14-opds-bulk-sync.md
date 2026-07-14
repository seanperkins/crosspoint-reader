# OPDS Bulk "Sync all" Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a manual "Sync all" action that recursively walks a Calibre/OPDS catalog and downloads every book not already on the SD card.

**Architecture:** A pure, host-testable walk engine (`OpdsSyncEngine`) does the recursion, dedup, pagination, cycle-guarding, and stats. It talks to the network/storage through an injected `OpdsSyncBackend` interface (real adapter on device, fake in tests) and reports per-book progress through an `OpdsSyncObserver`. `OpdsBookBrowserActivity` gains a `SYNCING`/`SYNC_SUMMARY` state, triggers the engine on the (currently unused) `Button::Right`, and reuses its existing WiFi lifecycle.

**Tech Stack:** C++20, ESP32-C3 (Arduino-ESP32), GoogleTest host harness (CMake), existing `OpdsParser` / `HttpDownloader` / `HalStorage` / `UITheme` / I18n.

## Global Constraints

- **RAM ceiling ~380KB, no PSRAM.** Peak sync RAM must stay at one book download at a time; the walk holds one feed page at a time, never the whole library.
- **No exceptions / no RTTI.** Bare `new` aborts on OOM — use `new (std::nothrow)` / `makeUniqueNoThrow` and null-check. (This plan allocates nothing on the heap directly; note it if that changes.)
- **Avoid `std::function` in library/hot paths.** Use virtual interfaces for the backend/observer boundary (done below), not `std::function`.
- **All user-facing text via `tr(STR_*)`.** Log messages may be hardcoded.
- **Engine must be Arduino-free.** `src/network/OpdsSyncEngine.{h,cpp}` may include only `<string>`, `<vector>`, `<set>`, `<deque>`, `<utility>`, and the host-safe helpers `util/UrlUtils.h` and `util/StringUtils.h`. It must NOT include `OpdsParser.h` (drags in `<Print.h>`/`<expat.h>`), `Arduino.h`, `WiFi.h`, `HalStorage.h`, or `HttpDownloader.h`.
- **Dedup key = target filename existence**, computed by `opdsLocalEpubPath(author, title)`, which must byte-for-byte match the existing single-book scheme in `OpdsBookBrowserActivity::downloadBook` (`src/activities/browser/OpdsBookBrowserActivity.cpp:282-283`): `"/" + StringUtils::sanitizeFilename((author.empty() ? "" : author + " - ") + title) + ".epub"`.
- **Download-only, never destructive.** Sync never deletes local books; it only removes a *partial* file left by a failed download.

---

### Task 1: Shared local-path helper + engine header skeleton

Create the engine module with the data types and interfaces, plus the one piece of pure logic worth testing on its own — the target-path helper — and refactor the existing single-book download to use it so both flows dedup identically.

**Files:**
- Create: `src/network/OpdsSyncEngine.h`
- Create: `src/network/OpdsSyncEngine.cpp`
- Create: `test/opds_sync/OpdsSyncEngineTest.cpp`
- Create: `test/opds_sync/CMakeLists.txt`
- Modify: `test/CMakeLists.txt` (register the new suite)
- Modify: `src/activities/browser/OpdsBookBrowserActivity.cpp:282-283` (use the helper)

**Interfaces:**
- Consumes: `StringUtils::sanitizeFilename` (`src/util/StringUtils.h:12`).
- Produces (used by all later tasks):
  - `struct OpdsSyncEntry { bool isBook=false; std::string title, author, href; };`
  - `enum class OpdsDownloadOutcome { Ok, Failed };`
  - `struct OpdsSyncStats { int added=0, skipped=0, failed=0; };`
  - `struct OpdsSyncProgress { OpdsSyncStats stats; std::string currentTitle; };`
  - `class OpdsSyncBackend { virtual bool fetchFeed(const std::string& url, std::vector<OpdsSyncEntry>&, std::string& outNextPageUrl)=0; virtual bool exists(const std::string&)=0; virtual OpdsDownloadOutcome download(const std::string& url, const std::string& targetPath)=0; virtual ~OpdsSyncBackend()=default; };`
  - `class OpdsSyncObserver { virtual bool onBeforeDownload(const OpdsSyncProgress&)=0; virtual ~OpdsSyncObserver()=default; };`
  - `class OpdsSyncEngine { OpdsSyncEngine(OpdsSyncBackend&, OpdsSyncObserver&); OpdsSyncStats run(const std::string& startUrl); static constexpr int MAX_DEPTH=8; };`
  - `std::string opdsLocalEpubPath(const std::string& author, const std::string& title);`

- [ ] **Step 1: Write the failing test**

Create `test/opds_sync/OpdsSyncEngineTest.cpp`:

```cpp
#include <gtest/gtest.h>

#include "network/OpdsSyncEngine.h"

TEST(OpdsLocalEpubPath, AuthorAndTitle) {
  EXPECT_EQ(opdsLocalEpubPath("Jane Doe", "A Book"), "/Jane Doe - A Book.epub");
}

TEST(OpdsLocalEpubPath, EmptyAuthorOmitsSeparator) {
  EXPECT_EQ(opdsLocalEpubPath("", "A Book"), "/A Book.epub");
}

TEST(OpdsLocalEpubPath, SanitizesIllegalChars) {
  // '/' is illegal in a filename and must be replaced (not left to split paths).
  EXPECT_EQ(opdsLocalEpubPath("", "a/b"), "/a_b.epub");
}
```

- [ ] **Step 2: Create the CMake wiring so the suite is discoverable**

Create `test/opds_sync/CMakeLists.txt`:

```cmake
add_executable(OpdsSyncEngineTest
  OpdsSyncEngineTest.cpp
  ${REPO_ROOT}/src/network/OpdsSyncEngine.cpp
  ${REPO_ROOT}/src/util/UrlUtils.cpp
  ${REPO_ROOT}/src/util/StringUtils.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(OpdsSyncEngineTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(OpdsSyncEngineTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(OpdsSyncEngineTest)
```

Append to `test/CMakeLists.txt` (after the last `add_subdirectory(...)` line):

```cmake
add_subdirectory(opds_sync)
```

- [ ] **Step 3: Create the header**

Create `src/network/OpdsSyncEngine.h`:

```cpp
#pragma once
// Arduino-free, host-testable OPDS bulk-sync engine.
// MUST NOT include Arduino.h, WiFi.h, OpdsParser.h, HttpDownloader.h, or HalStorage.h.
#include <deque>
#include <set>
#include <string>
#include <utility>
#include <vector>

// One catalog entry, decoupled from OpdsParser's OpdsEntry so this header stays Arduino-free.
struct OpdsSyncEntry {
  bool isBook = false;  // true = downloadable book; false = navigation sub-catalog
  std::string title;
  std::string author;  // books only
  std::string href;    // link from the feed (relative or absolute)
};

enum class OpdsDownloadOutcome { Ok, Failed };

struct OpdsSyncStats {
  int added = 0;
  int skipped = 0;
  int failed = 0;
};

struct OpdsSyncProgress {
  OpdsSyncStats stats;       // counts accumulated so far
  std::string currentTitle;  // title of the book about to download
};

// SD-card target path for a book. Matches the single-book OPDS download scheme
// so both flows dedup against the same filename.
std::string opdsLocalEpubPath(const std::string& author, const std::string& title);

// Network + storage boundary. Real adapter on device; faked in host tests.
class OpdsSyncBackend {
 public:
  virtual ~OpdsSyncBackend() = default;
  // Fetch + parse the feed at absolute `url`. Fill entries and outNextPageUrl
  // (empty when there is no next page). Return false on network/parse failure.
  virtual bool fetchFeed(const std::string& url, std::vector<OpdsSyncEntry>& outEntries,
                         std::string& outNextPageUrl) = 0;
  // True if a book already exists locally at `targetPath`.
  virtual bool exists(const std::string& targetPath) = 0;
  // Download the book at absolute `url` to `targetPath`.
  virtual OpdsDownloadOutcome download(const std::string& url, const std::string& targetPath) = 0;
};

class OpdsSyncObserver {
 public:
  virtual ~OpdsSyncObserver() = default;
  // Called immediately before each new-book download. Return false to cancel
  // (checked between books, never mid-download).
  virtual bool onBeforeDownload(const OpdsSyncProgress& progress) = 0;
};

class OpdsSyncEngine {
 public:
  static constexpr int MAX_DEPTH = 8;

  OpdsSyncEngine(OpdsSyncBackend& backend, OpdsSyncObserver& observer)
      : backend(backend), observer(observer) {}

  // Walk from absolute `startUrl`, recursing into navigation sub-catalogs
  // (BFS, cycle-guarded, depth-capped) and following pagination. Returns
  // aggregate stats; returns partial stats early if a feed fetch fails or the
  // observer cancels.
  OpdsSyncStats run(const std::string& startUrl);

 private:
  OpdsSyncBackend& backend;
  OpdsSyncObserver& observer;
};
```

- [ ] **Step 4: Create the .cpp with the path helper only (leave `run` for Task 2)**

Create `src/network/OpdsSyncEngine.cpp`:

```cpp
#include "network/OpdsSyncEngine.h"

#include "util/StringUtils.h"
#include "util/UrlUtils.h"

std::string opdsLocalEpubPath(const std::string& author, const std::string& title) {
  return "/" + StringUtils::sanitizeFilename((author.empty() ? "" : author + " - ") + title) + ".epub";
}

// OpdsSyncEngine::run is implemented in Task 2.
```

- [ ] **Step 5: Configure and run the test — verify it PASSES**

Run:
```bash
cmake -S test -B build/test && cmake --build build/test --target OpdsSyncEngineTest && ./build/test/opds_sync/OpdsSyncEngineTest
```
Expected: 3 tests PASS. (If `opdsLocalEpubPath("", "a/b")` differs from `"/a_b.epub"`, read `StringUtils::sanitizeFilename` and adjust the expected string to match the real sanitizer — the point is that both flows share this function, not the exact replacement char.)

- [ ] **Step 6: Refactor the single-book download to use the helper**

In `src/activities/browser/OpdsBookBrowserActivity.cpp`, add near the other includes:
```cpp
#include "network/OpdsSyncEngine.h"
```
Replace lines 282-283:
```cpp
  std::string filename =
      "/" + StringUtils::sanitizeFilename((book.author.empty() ? "" : book.author + " - ") + book.title) + ".epub";
```
with:
```cpp
  std::string filename = opdsLocalEpubPath(book.author, book.title);
```

- [ ] **Step 7: Verify device build still compiles**

Run: `pio run 2>&1 | tail -5`
Expected: `SUCCESS`.

- [ ] **Step 8: Commit**

```bash
git add src/network/OpdsSyncEngine.h src/network/OpdsSyncEngine.cpp \
        test/opds_sync/ test/CMakeLists.txt \
        src/activities/browser/OpdsBookBrowserActivity.cpp
git commit -m "feat: add OPDS sync engine skeleton + shared book path helper"
```

---

### Task 2: Engine walk — recursion, dedup, pagination, cancel

Implement `OpdsSyncEngine::run` and test it thoroughly with a fake backend/observer.

**Files:**
- Modify: `src/network/OpdsSyncEngine.cpp` (implement `run`)
- Modify: `test/opds_sync/OpdsSyncEngineTest.cpp` (add walk tests + fakes)

**Interfaces:**
- Consumes: everything from Task 1 (`OpdsSyncBackend`, `OpdsSyncObserver`, `OpdsSyncStats`, `opdsLocalEpubPath`, `UrlUtils::buildUrl`).
- Produces: working `OpdsSyncStats OpdsSyncEngine::run(const std::string&)`.

- [ ] **Step 1: Write the failing tests + fakes**

Append to `test/opds_sync/OpdsSyncEngineTest.cpp`:

```cpp
#include <map>

namespace {

struct FakeFeed {
  std::vector<OpdsSyncEntry> entries;
  std::string nextPageUrl;
};

class FakeBackend : public OpdsSyncBackend {
 public:
  std::map<std::string, FakeFeed> feeds;    // url -> feed
  std::set<std::string> existing;           // target paths already on disk
  std::set<std::string> failDownloads;      // book URLs that should fail
  std::vector<std::string> downloaded;      // target paths downloaded, in order
  bool nextFetchFails = false;

  bool fetchFeed(const std::string& url, std::vector<OpdsSyncEntry>& out, std::string& next) override {
    if (nextFetchFails) return false;
    auto it = feeds.find(url);
    if (it == feeds.end()) return false;
    out = it->second.entries;
    next = it->second.nextPageUrl;
    return true;
  }
  bool exists(const std::string& p) override { return existing.count(p) > 0; }
  OpdsDownloadOutcome download(const std::string& url, const std::string& target) override {
    if (failDownloads.count(url)) return OpdsDownloadOutcome::Failed;
    downloaded.push_back(target);
    return OpdsDownloadOutcome::Ok;
  }
};

// Observer that allows N downloads then cancels; -1 = never cancel.
class FakeObserver : public OpdsSyncObserver {
 public:
  int allowBeforeCancel = -1;
  int calls = 0;
  bool onBeforeDownload(const OpdsSyncProgress&) override {
    if (allowBeforeCancel >= 0 && calls >= allowBeforeCancel) return false;
    calls++;
    return true;
  }
};

OpdsSyncEntry book(const std::string& author, const std::string& title, const std::string& href) {
  return OpdsSyncEntry{true, title, author, href};
}
OpdsSyncEntry nav(const std::string& title, const std::string& href) {
  return OpdsSyncEntry{false, title, "", href};
}

}  // namespace

TEST(OpdsSyncEngine, DownloadsAllNewBooksInFlatFeed) {
  FakeBackend be;
  be.feeds["http://s/opds"] = {{book("A", "1", "b1"), book("A", "2", "b2")}, ""};
  FakeObserver ob;
  OpdsSyncEngine engine(be, ob);
  auto stats = engine.run("http://s/opds");
  EXPECT_EQ(stats.added, 2);
  EXPECT_EQ(stats.skipped, 0);
  EXPECT_EQ(stats.failed, 0);
}

TEST(OpdsSyncEngine, SkipsBooksThatAlreadyExist) {
  FakeBackend be;
  be.feeds["http://s/opds"] = {{book("A", "1", "b1"), book("A", "2", "b2")}, ""};
  be.existing.insert(opdsLocalEpubPath("A", "1"));
  FakeObserver ob;
  OpdsSyncEngine engine(be, ob);
  auto stats = engine.run("http://s/opds");
  EXPECT_EQ(stats.added, 1);
  EXPECT_EQ(stats.skipped, 1);
}

TEST(OpdsSyncEngine, RecursesIntoNavigationSubCatalogs) {
  FakeBackend be;
  be.feeds["http://s/opds"] = {{nav("By Author", "authors")}, ""};
  be.feeds["http://s/authors"] = {{book("A", "1", "b1"), book("A", "2", "b2")}, ""};
  FakeObserver ob;
  OpdsSyncEngine engine(be, ob);
  auto stats = engine.run("http://s/opds");
  EXPECT_EQ(stats.added, 2);
}

TEST(OpdsSyncEngine, TerminatesOnCycleWithoutDoubleDownloading) {
  FakeBackend be;
  be.feeds["http://s/opds"] = {{nav("Loop", "child")}, ""};
  be.feeds["http://s/child"] = {{book("A", "1", "b1"), nav("Back", "http://s/opds")}, ""};
  FakeObserver ob;
  OpdsSyncEngine engine(be, ob);
  auto stats = engine.run("http://s/opds");
  EXPECT_EQ(stats.added, 1);  // visited-guard prevents re-walking root
}

TEST(OpdsSyncEngine, FollowsPagination) {
  FakeBackend be;
  be.feeds["http://s/opds"] = {{book("A", "1", "b1")}, "page2"};
  be.feeds["http://s/page2"] = {{book("A", "2", "b2")}, ""};
  FakeObserver ob;
  OpdsSyncEngine engine(be, ob);
  auto stats = engine.run("http://s/opds");
  EXPECT_EQ(stats.added, 2);
}

TEST(OpdsSyncEngine, CountsFailuresAndContinues) {
  FakeBackend be;
  be.feeds["http://s/opds"] = {{book("A", "1", "b1"), book("A", "2", "b2")}, ""};
  be.failDownloads.insert("http://s/b1");
  FakeObserver ob;
  OpdsSyncEngine engine(be, ob);
  auto stats = engine.run("http://s/opds");
  EXPECT_EQ(stats.added, 1);
  EXPECT_EQ(stats.failed, 1);
}

TEST(OpdsSyncEngine, StopsWhenObserverCancels) {
  FakeBackend be;
  be.feeds["http://s/opds"] = {{book("A", "1", "b1"), book("A", "2", "b2")}, ""};
  FakeObserver ob;
  ob.allowBeforeCancel = 1;  // allow first, cancel before second
  OpdsSyncEngine engine(be, ob);
  auto stats = engine.run("http://s/opds");
  EXPECT_EQ(stats.added, 1);
}

TEST(OpdsSyncEngine, StopsOnFeedFetchFailure) {
  FakeBackend be;
  be.nextFetchFails = true;
  FakeObserver ob;
  OpdsSyncEngine engine(be, ob);
  auto stats = engine.run("http://s/opds");
  EXPECT_EQ(stats.added, 0);
  EXPECT_EQ(stats.failed, 0);
}
```

- [ ] **Step 2: Run tests — verify they FAIL**

Run: `cmake --build build/test --target OpdsSyncEngineTest && ./build/test/opds_sync/OpdsSyncEngineTest`
Expected: the 8 `OpdsSyncEngine.*` tests FAIL (link error or all-zero stats — `run` is not implemented).

- [ ] **Step 3: Implement `run`**

In `src/network/OpdsSyncEngine.cpp`, replace the `// OpdsSyncEngine::run is implemented in Task 2.` comment with:

```cpp
OpdsSyncStats OpdsSyncEngine::run(const std::string& startUrl) {
  OpdsSyncStats stats;
  std::set<std::string> visited;
  std::deque<std::pair<std::string, int>> queue;
  queue.push_back({startUrl, 0});

  while (!queue.empty()) {
    const std::string url = queue.front().first;
    const int depth = queue.front().second;
    queue.pop_front();

    if (depth > MAX_DEPTH) continue;
    if (!visited.insert(url).second) continue;  // already walked this feed URL

    std::vector<OpdsSyncEntry> entries;
    std::string nextPageUrl;
    if (!backend.fetchFeed(url, entries, nextPageUrl)) {
      return stats;  // network/parse failure — stop, keep what we have
    }

    for (const auto& entry : entries) {
      if (entry.isBook) {
        const std::string target = opdsLocalEpubPath(entry.author, entry.title);
        if (backend.exists(target)) {
          stats.skipped++;
          continue;
        }
        OpdsSyncProgress progress{stats, entry.title};
        if (!observer.onBeforeDownload(progress)) return stats;  // cancelled between books
        const std::string bookUrl = UrlUtils::buildUrl(url, entry.href);
        if (backend.download(bookUrl, target) == OpdsDownloadOutcome::Ok) {
          stats.added++;
        } else {
          stats.failed++;
        }
      } else {
        queue.push_back({UrlUtils::buildUrl(url, entry.href), depth + 1});
      }
    }

    if (!nextPageUrl.empty()) {
      // Pagination of the same catalog: same depth, cycle-guarded via `visited`.
      queue.push_back({UrlUtils::buildUrl(url, nextPageUrl), depth});
    }
  }

  return stats;
}
```

- [ ] **Step 4: Run tests — verify they PASS**

Run: `cmake --build build/test --target OpdsSyncEngineTest && ./build/test/opds_sync/OpdsSyncEngineTest`
Expected: all 11 tests PASS.

Note on the cycle test: `UrlUtils::buildUrl(parent, absoluteChild)` must return the absolute child URL unchanged when the href is already absolute (`http://s/opds`), so the visited-guard matches. If that test fails because `buildUrl` mangles an absolute href, read `src/util/UrlUtils.cpp` and adjust the test's URLs to the form `buildUrl` actually produces — do not weaken the guard.

- [ ] **Step 5: Verify device build**

Run: `pio run 2>&1 | tail -5`
Expected: `SUCCESS`.

- [ ] **Step 6: Commit**

```bash
git add src/network/OpdsSyncEngine.cpp test/opds_sync/OpdsSyncEngineTest.cpp
git commit -m "feat: implement OPDS sync walk (recursion, dedup, pagination, cancel)"
```

---

### Task 3: I18n strings

Add the user-facing strings the UI will need, before wiring the UI that references them.

**Files:**
- Modify: `lib/I18n/translations/english.yaml`

**Interfaces:**
- Produces: `StrId::STR_SYNC_ALL`, `STR_SYNC_IN_PROGRESS`, `STR_SYNC_DONE`, `STR_SYNC_SUMMARY_FMT` (generated into `lib/I18n/I18nKeys.h`).

- [ ] **Step 1: Add the strings**

Append to `lib/I18n/translations/english.yaml` (near the other reader/download strings, e.g. after `STR_DOWNLOADING`):

```yaml
STR_SYNC_ALL: "Sync all"
STR_SYNC_IN_PROGRESS: "Syncing library..."
STR_SYNC_DONE: "Sync complete"
STR_SYNC_SUMMARY_FMT: "%d added, %d skipped, %d failed"
```

- [ ] **Step 2: Regenerate the i18n headers**

Run: `python scripts/gen_i18n.py lib/I18n/translations lib/I18n/`
Expected: no error; `lib/I18n/I18nKeys.h` now contains `STR_SYNC_ALL` etc. Verify:
```bash
grep -c "STR_SYNC_ALL\|STR_SYNC_IN_PROGRESS\|STR_SYNC_DONE\|STR_SYNC_SUMMARY_FMT" lib/I18n/I18nKeys.h
```
Expected: `4`.

- [ ] **Step 3: Commit (source YAML only — generated files are gitignored)**

```bash
git add lib/I18n/translations/english.yaml
git status --short   # confirm no lib/I18n/I18n*.h / .cpp staged
git commit -m "feat: add i18n strings for OPDS sync all"
```

---

### Task 4: Device backend adapter

Implement the on-device `OpdsSyncBackend` that wires the engine to `OpdsParser`, `HttpDownloader`, and `HalStorage`. Build-verified (Arduino deps, not host-testable).

**Files:**
- Create: `src/network/OpdsSyncBackendDevice.h`
- Create: `src/network/OpdsSyncBackendDevice.cpp`

**Interfaces:**
- Consumes: `OpdsSyncBackend` (Task 1), `OpdsServer` (`src/OpdsServerStore.h:8`), `OpdsParser`/`OpdsParserStream`, `HttpDownloader` (`src/network/HttpDownloader.h`), `HalStorage` (`Storage` singleton), `clearBookCache` (`src/util/BookCacheUtils.h`).
- Produces: `class OpdsSyncBackendDevice : public OpdsSyncBackend` constructed from `const OpdsServer&`.

- [ ] **Step 1: Create the header**

Create `src/network/OpdsSyncBackendDevice.h`:

```cpp
#pragma once
#include "OpdsServerStore.h"
#include "network/OpdsSyncEngine.h"

// On-device OpdsSyncBackend: fetches/parses feeds over HTTP, checks the SD card,
// and downloads books. Holds a copy of the server config (credentials included).
class OpdsSyncBackendDevice final : public OpdsSyncBackend {
 public:
  explicit OpdsSyncBackendDevice(OpdsServer server) : server(std::move(server)) {}

  bool fetchFeed(const std::string& url, std::vector<OpdsSyncEntry>& outEntries,
                 std::string& outNextPageUrl) override;
  bool exists(const std::string& targetPath) override;
  OpdsDownloadOutcome download(const std::string& url, const std::string& targetPath) override;

 private:
  OpdsServer server;
};
```

- [ ] **Step 2: Create the implementation**

Create `src/network/OpdsSyncBackendDevice.cpp`:

```cpp
#include "network/OpdsSyncBackendDevice.h"

#include <HalStorage.h>
#include <Logging.h>
#include <OpdsParser.h>
#include <OpdsStream.h>

#include "network/HttpDownloader.h"
#include "util/BookCacheUtils.h"

bool OpdsSyncBackendDevice::fetchFeed(const std::string& url, std::vector<OpdsSyncEntry>& outEntries,
                                      std::string& outNextPageUrl) {
  OpdsParser parser;
  {
    OpdsParserStream stream{parser};
    if (!HttpDownloader::fetchUrl(url, stream, server.username, server.password)) {
      LOG_ERR("OPDSSYNC", "Feed fetch failed: %s", url.c_str());
      return false;
    }
  }
  if (!parser) {
    LOG_ERR("OPDSSYNC", "Feed parse failed: %s", url.c_str());
    return false;
  }

  outNextPageUrl = parser.getNextPageUrl();
  const auto& entries = parser.getEntries();
  outEntries.reserve(entries.size());
  for (const auto& e : entries) {
    OpdsSyncEntry se;
    se.isBook = (e.type == OpdsEntryType::BOOK);
    se.title = e.title;
    se.author = e.author;
    se.href = e.href;
    outEntries.push_back(std::move(se));
  }
  return true;
}

bool OpdsSyncBackendDevice::exists(const std::string& targetPath) {
  return Storage.exists(targetPath.c_str());
}

OpdsDownloadOutcome OpdsSyncBackendDevice::download(const std::string& url, const std::string& targetPath) {
  const auto result = HttpDownloader::downloadToFile(url, targetPath, nullptr, nullptr, server.username, server.password);
  if (result == HttpDownloader::OK) {
    clearBookCache(targetPath);
    return OpdsDownloadOutcome::Ok;
  }
  LOG_ERR("OPDSSYNC", "Download failed (%d): %s", static_cast<int>(result), url.c_str());
  // Remove any partial file so a later sync re-downloads cleanly instead of
  // treating a truncated file as "already have".
  Storage.remove(targetPath.c_str());
  return OpdsDownloadOutcome::Failed;
}
```

- [ ] **Step 3: Verify device build**

Run: `pio run 2>&1 | tail -5`
Expected: `SUCCESS`.

- [ ] **Step 4: Commit**

```bash
git add src/network/OpdsSyncBackendDevice.h src/network/OpdsSyncBackendDevice.cpp
git commit -m "feat: on-device OPDS sync backend adapter"
```

---

### Task 5: Browser UI integration — trigger, progress, summary, cancel

Wire the engine into `OpdsBookBrowserActivity`: `Button::Right` starts a sync of the current catalog, a progress screen updates per book with Back-to-cancel, and a summary screen reports counts. Reuses the browser's existing WiFi lifecycle. Build- + device-verified.

**Files:**
- Modify: `src/activities/browser/OpdsBookBrowserActivity.h`
- Modify: `src/activities/browser/OpdsBookBrowserActivity.cpp`

**Interfaces:**
- Consumes: `OpdsSyncEngine`, `OpdsSyncBackendDevice`, `OpdsSyncObserver`, `OpdsSyncStats` (Tasks 1-4); `STR_SYNC_*` (Task 3); existing `UrlUtils::buildUrl`, `requestUpdate(true)`, `mappedInput`.
- Produces: no new public interface; internal `SYNCING`/`SYNC_SUMMARY` states.

- [ ] **Step 1: Extend the header**

In `src/activities/browser/OpdsBookBrowserActivity.h`:

Add to the includes block:
```cpp
#include "network/OpdsSyncEngine.h"
```
Change the `BrowserState` enum to add two states:
```cpp
  enum class BrowserState { CHECK_WIFI, WIFI_SELECTION, LOADING, BROWSING, DOWNLOADING, ERROR, SEARCH_INPUT, SYNCING, SYNC_SUMMARY };
```
Make the class implement the observer — change the class declaration:
```cpp
class OpdsBookBrowserActivity final : public Activity, public OpdsSyncObserver {
```
Add to the `private:` members:
```cpp
  OpdsSyncStats syncStats;
  std::string syncCurrentTitle;
```
Add to the private methods:
```cpp
  void startSync();
  bool onBeforeDownload(const OpdsSyncProgress& progress) override;
```

- [ ] **Step 2: Add the sync includes to the .cpp**

In `src/activities/browser/OpdsBookBrowserActivity.cpp`, add near the other `network/` includes:
```cpp
#include "network/OpdsSyncBackendDevice.h"
```
(`network/OpdsSyncEngine.h` is already pulled in via the header from Task 1 Step 6.)

- [ ] **Step 3: Trigger sync from BROWSING on Button::Right**

In `loop()`, inside the `if (state == BrowserState::BROWSING) { ... }` block, add a branch to the existing `if/else if` chain that handles Confirm/Back/Left (after the `Button::Left` branch at line ~104):
```cpp
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      startSync();
    }
```

- [ ] **Step 4: Handle input in the new states**

In `loop()`, add near the other early-state handlers (e.g. right after the `if (state == BrowserState::DOWNLOADING) return;` line):
```cpp
  if (state == BrowserState::SYNCING) return;  // sync runs synchronously; input handled in onBeforeDownload

  if (state == BrowserState::SYNC_SUMMARY) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      state = BrowserState::BROWSING;
      requestUpdate();
    }
    return;
  }
```

- [ ] **Step 5: Implement startSync + observer**

Add these definitions to the .cpp (e.g. after `downloadBook`):
```cpp
void OpdsBookBrowserActivity::startSync() {
  state = BrowserState::SYNCING;
  syncStats = OpdsSyncStats{};
  syncCurrentTitle.clear();
  requestUpdate(true);

  const std::string startUrl = UrlUtils::buildUrl(server.url, currentPath);
  OpdsSyncBackendDevice backend(server);
  OpdsSyncEngine engine(backend, *this);
  syncStats = engine.run(startUrl);

  LOG_INF("OPDSSYNC", "Sync done: +%d, skip %d, fail %d", syncStats.added, syncStats.skipped, syncStats.failed);
  state = BrowserState::SYNC_SUMMARY;
  requestUpdate(true);
}

bool OpdsBookBrowserActivity::onBeforeDownload(const OpdsSyncProgress& progress) {
  syncStats = progress.stats;
  syncCurrentTitle = progress.currentTitle;
  requestUpdate(true);  // force a synchronous render of the SYNCING screen

  // Poll input so a Back press cancels the sync after the current book.
  mappedInput.update();
  const bool cancel = mappedInput.wasReleased(MappedInputManager::Button::Back);
  delay(1);  // yield to keep the watchdog fed between books
  return !cancel;
}
```

- [ ] **Step 6: Render the two new screens**

In `render()`, add before the final "browsing list" section (right after the `if (state == BrowserState::DOWNLOADING) { ... }` block):
```cpp
  if (state == BrowserState::SYNCING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 40, tr(STR_SYNC_IN_PROGRESS));
    char counts[48];
    snprintf(counts, sizeof(counts), tr(STR_SYNC_SUMMARY_FMT), syncStats.added, syncStats.skipped, syncStats.failed);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 10, counts);
    if (!syncCurrentTitle.empty()) {
      auto title = renderer.truncatedText(UI_10_FONT_ID, syncCurrentTitle.c_str(), pageWidth - 40);
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 20, title.c_str());
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == BrowserState::SYNC_SUMMARY) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, tr(STR_SYNC_DONE));
    char counts[48];
    snprintf(counts, sizeof(counts), tr(STR_SYNC_SUMMARY_FMT), syncStats.added, syncStats.skipped, syncStats.failed);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, counts);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OK), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }
```
(If `STR_OK` does not exist, use `tr(STR_CONFIRM)` or the closest existing confirm label — grep `lib/I18n/translations/english.yaml` for `STR_OK`/`STR_CONFIRM` and pick one; do not add a new string for this.)

- [ ] **Step 7: Add the "Sync all" button hint in BROWSING**

In `render()`, find the browsing-state label line (currently):
```cpp
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, searchLabel, tr(STR_DIR_DOWN));
```
Change the 4th label (Right button) from `tr(STR_DIR_DOWN)` to `tr(STR_SYNC_ALL)`:
```cpp
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, searchLabel, tr(STR_SYNC_ALL));
```

- [ ] **Step 8: Verify device build**

Run: `pio run 2>&1 | tail -5`
Expected: `SUCCESS`, no warnings.

- [ ] **Step 9: Format**

Run: `find src -name "*.cpp" -o -name "*.h" | xargs clang-format -i && git diff --stat`
Expected: only the files touched in this task change (if any).

- [ ] **Step 10: Commit**

```bash
git add src/activities/browser/OpdsBookBrowserActivity.h src/activities/browser/OpdsBookBrowserActivity.cpp
git commit -m "feat: Sync all button in OPDS browser (progress, summary, cancel)"
```

- [ ] **Step 11: Device verification (human tester)**

Flag for the user — cannot be verified by the agent:
1. Flash: `pio run -t upload`.
2. Add the Calibre-Web OPDS server (`http://192.168.50.69:8083/opds`, with credentials) on the device.
3. Open the OPDS browser, press the **Right** front button → confirm the sync screen appears, counts climb, and the current title shows.
4. Confirm new books land at root `/` and appear in the library file browser.
5. Press **Back** mid-sync → confirm it stops after the current book and shows the summary.
6. Re-run "Sync all" → confirm everything is skipped (`0 added`), no re-downloads.
7. Monitor `ESP.getFreeHeap()` across a large sync → confirm no steady growth proportional to library size.

---

## Self-Review

**Spec coverage:**
- Calibre/OPDS transport, no SMB → Tasks 4 (adapter over existing OPDS), no new protocol. ✓
- Manual "Sync now" trigger → Task 5 (`Button::Right`). ✓
- Recursive catalog walk → Task 2 (BFS into navigation entries). ✓
- Dedup = filename existence, shared scheme → Tasks 1 (`opdsLocalEpubPath` + browser refactor) & 2. ✓
- Cycle guard + depth cap → Task 2 (`visited` set, `MAX_DEPTH`). ✓
- Pagination → Task 2 (`nextPageUrl`). ✓
- Per-book failure continues; partial-file delete → Tasks 2 (counts+continue) & 4 (`Storage.remove`). ✓
- Feed-fetch failure stops, keeps downloaded → Task 2 (`return stats`). ✓
- Progress `Syncing N/… — title`, Back = stop after current book, summary counts → Task 5. ✓
- Files land at root `/` where library scans → inherited from `opdsLocalEpubPath` returning `/`-prefixed path. ✓
- One-download-at-a-time RAM posture → engine holds one page; adapter one download. ✓
- Out of scope (SMB, auto-trigger, two-way, metadata, destructive delete) → nothing in the plan adds these. ✓

**Placeholder scan:** No TBD/TODO; every code step shows complete code. Two guarded fallbacks (sanitizer replacement char in T1S5, `STR_OK` in T5S6) give an explicit verification + resolution, not a blank. ✓

**Type consistency:** `OpdsSyncEntry`, `OpdsSyncStats`, `OpdsSyncBackend::{fetchFeed,exists,download}`, `OpdsSyncObserver::onBeforeDownload`, `OpdsDownloadOutcome::{Ok,Failed}`, `opdsLocalEpubPath`, `OpdsSyncEngine::{run,MAX_DEPTH}` used identically across Tasks 1-5. ✓
