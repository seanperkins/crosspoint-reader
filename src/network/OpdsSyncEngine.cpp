#include "network/OpdsSyncEngine.h"

#include <cstdint>

#include "util/StringUtils.h"
#include "util/UrlUtils.h"

namespace {
// FNV-1a 64-bit. Used to track visited feeds and handled books as fixed-size
// hashes instead of full URL strings — far less heap during a deep recursive
// walk. A fixed 64-bit width keeps results identical on the 64-bit host tests
// and the 32-bit device (std::hash<std::string> would differ by pointer width).
uint64_t hashUrl(const std::string& s) {
  uint64_t h = 1469598103934665603ULL;
  for (const unsigned char c : s) {
    h ^= static_cast<uint64_t>(c);
    h *= 1099511628211ULL;
  }
  return h;
}
}  // namespace

std::string opdsLocalEpubPath(const std::string& author, const std::string& title) {
  // Organize by author: /{Author}/{Title}.epub. Each segment is sanitized
  // independently so an illegal character (e.g. '/') can't escape its folder.
  // Books with no author fall back to the SD root so no empty folder is made.
  const std::string titleFile = StringUtils::sanitizeFilename(title) + ".epub";
  if (author.empty()) {
    return "/" + titleFile;
  }
  return "/" + StringUtils::sanitizeFilename(author) + "/" + titleFile;
}

OpdsSyncStats OpdsSyncEngine::run(const std::string& startUrl) {
  OpdsSyncStats stats;
  std::set<uint64_t> visited;    // hashes of navigation-feed URLs already walked
  std::set<uint64_t> seenBooks;  // hashes of book download URLs handled this run
  std::deque<std::pair<std::string, int>> queue;
  queue.push_back({startUrl, 0});

  while (!queue.empty()) {
    const std::string url = queue.front().first;
    const int depth = queue.front().second;
    queue.pop_front();

    if (depth > MAX_DEPTH) continue;
    const uint64_t urlHash = hashUrl(url);
    if (visited.count(urlHash) > 0) continue;  // already walked this feed URL
    if (visited.size() >= maxVisitedFeeds) {
      // Cap reached: stop gracefully rather than growing `visited` (and the
      // queue) without bound, which risks abort() on OOM under
      // -fno-exceptions.
      stats.limitReached = true;
      return stats;
    }
    visited.insert(urlHash);

    std::vector<OpdsSyncEntry> entries;
    std::string nextPageUrl;
    if (!backend.fetchFeed(url, entries, nextPageUrl)) {
      return stats;  // network/parse failure — stop, keep what we have
    }

    for (const auto& entry : entries) {
      if (entry.isBook) {
        const std::string bookUrl = UrlUtils::buildUrl(url, entry.href);
        // The same book is commonly listed under several nav trees (author,
        // series, category). Handle each book once per run: skip re-encounters
        // before touching the SD card or re-downloading. Bounded by the same
        // cap; once full, dedup degrades to the on-disk exists() check rather
        // than growing without bound.
        const uint64_t bookHash = hashUrl(bookUrl);
        if (seenBooks.count(bookHash) > 0) continue;
        if (seenBooks.size() < maxVisitedFeeds) seenBooks.insert(bookHash);

        const std::string target = opdsLocalEpubPath(entry.author, entry.title);
        if (backend.exists(target)) {
          stats.skipped++;
          continue;
        }
        OpdsSyncProgress progress{stats, entry.title};
        if (!observer.onBeforeDownload(progress)) return stats;  // cancelled between books
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
