#pragma once
#include <string>
#include <utility>

#include "OpdsServerStore.h"
#include "network/OpdsSyncEngine.h"

// Ensures the parent directory of `filePath` exists on the SD card (creating it,
// with intermediate directories, when missing). No-op for root-level paths.
// Call before downloading a book so author folders (e.g. /R.A. Salvatore/) exist.
// Shared by the bulk-sync backend and the single-book download flow so both
// build the same folder layout that opdsLocalEpubPath() produces.
void opdsEnsureParentDir(const std::string& filePath);

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
