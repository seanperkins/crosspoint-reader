#include "network/OpdsSyncEngine.h"

#include "util/StringUtils.h"
#include "util/UrlUtils.h"

std::string opdsLocalEpubPath(const std::string& author, const std::string& title) {
  return "/" + StringUtils::sanitizeFilename((author.empty() ? "" : author + " - ") + title) + ".epub";
}

// OpdsSyncEngine::run is implemented in Task 2.
