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
  // buildUrl() resolves a relative href by appending it to the full feed URL
  // (it does not strip the feed URL's last path segment first), so a relative
  // href like "authors" would resolve to "http://s/opds/authors", not
  // "http://s/authors". Use an absolute href so it round-trips unchanged
  // through UrlUtils::buildUrl and matches the feed map key.
  be.feeds["http://s/opds"] = {{nav("By Author", "http://s/authors")}, ""};
  be.feeds["http://s/authors"] = {{book("A", "1", "b1"), book("A", "2", "b2")}, ""};
  FakeObserver ob;
  OpdsSyncEngine engine(be, ob);
  auto stats = engine.run("http://s/opds");
  EXPECT_EQ(stats.added, 2);
}

TEST(OpdsSyncEngine, TerminatesOnCycleWithoutDoubleDownloading) {
  FakeBackend be;
  // Absolute hrefs throughout: UrlUtils::buildUrl() only returns a path
  // unchanged when it already contains "://"; a relative "child" would
  // resolve to "http://s/opds/child" (appended to the full parent URL, not
  // resolved against its directory) and never match the "http://s/child"
  // feed key.
  be.feeds["http://s/opds"] = {{nav("Loop", "http://s/child")}, ""};
  be.feeds["http://s/child"] = {{book("A", "1", "b1"), nav("Back", "http://s/opds")}, ""};
  FakeObserver ob;
  OpdsSyncEngine engine(be, ob);
  auto stats = engine.run("http://s/opds");
  EXPECT_EQ(stats.added, 1);  // visited-guard prevents re-walking root
}

TEST(OpdsSyncEngine, FollowsPagination) {
  FakeBackend be;
  // Absolute next-page URL for the same reason as above: buildUrl() would
  // otherwise resolve relative "page2" to "http://s/opds/page2".
  be.feeds["http://s/opds"] = {{book("A", "1", "b1")}, "http://s/page2"};
  be.feeds["http://s/page2"] = {{book("A", "2", "b2")}, ""};
  FakeObserver ob;
  OpdsSyncEngine engine(be, ob);
  auto stats = engine.run("http://s/opds");
  EXPECT_EQ(stats.added, 2);
}

TEST(OpdsSyncEngine, CountsFailuresAndContinues) {
  FakeBackend be;
  // Absolute book hrefs so the URL passed to download() (via buildUrl())
  // matches the failDownloads key exactly instead of resolving to
  // "http://s/opds/b1".
  be.feeds["http://s/opds"] = {{book("A", "1", "http://s/b1"), book("A", "2", "http://s/b2")}, ""};
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
