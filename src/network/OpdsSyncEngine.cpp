#include "network/OpdsSyncEngine.h"

#include "util/StringUtils.h"
#include "util/UrlUtils.h"

std::string opdsLocalEpubPath(const std::string& author, const std::string& title) {
  return "/" + StringUtils::sanitizeFilename((author.empty() ? "" : author + " - ") + title) + ".epub";
}

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
