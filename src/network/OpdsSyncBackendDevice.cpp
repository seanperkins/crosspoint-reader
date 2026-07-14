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
