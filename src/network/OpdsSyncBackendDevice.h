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
