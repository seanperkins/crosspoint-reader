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
