#include <gtest/gtest.h>

#include "network/OpdsSyncEngine.h"

TEST(OpdsLocalEpubPath, AuthorFolderThenTitle) {
  EXPECT_EQ(opdsLocalEpubPath("Jane Doe", "A Book"), "/Jane Doe/A Book.epub");
}

TEST(OpdsLocalEpubPath, EmptyAuthorStaysAtRoot) {
  EXPECT_EQ(opdsLocalEpubPath("", "A Book"), "/A Book.epub");
}

TEST(OpdsLocalEpubPath, SanitizesTitleIllegalChars) {
  // '/' is illegal in a filename and must be replaced, not left to split paths.
  EXPECT_EQ(opdsLocalEpubPath("", "a/b"), "/a_b.epub");
}

TEST(OpdsLocalEpubPath, SanitizesAuthorFolderIllegalChars) {
  // The author segment is sanitized too, so it can't escape into other folders.
  EXPECT_EQ(opdsLocalEpubPath("a/b", "T"), "/a_b/T.epub");
}

#include <algorithm>
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
  std::vector<std::string> downloadedUrls;  // resolved source URLs passed to download(), in order
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
    downloadedUrls.push_back(url);
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
  // Relative href, resolved via UrlUtils::buildUrl's append semantics: a
  // relative href is appended to the *full* current feed URL (not resolved
  // against its parent directory), so nav("By Author", "authors") from
  // "http://s/opds" resolves to "http://s/opds/authors" — that's the key the
  // child feed must be registered under. The child feed's own book hrefs then
  // resolve against ITS feed URL: "b1"/"b2" -> "http://s/opds/authors/b1"
  // and ".../b2".
  be.feeds["http://s/opds"] = {{nav("By Author", "authors")}, ""};
  be.feeds["http://s/opds/authors"] = {{book("A", "1", "b1"), book("A", "2", "b2")}, ""};
  FakeObserver ob;
  OpdsSyncEngine engine(be, ob);
  auto stats = engine.run("http://s/opds");
  EXPECT_EQ(stats.added, 2);
  // Prove the nav AND book hrefs were actually resolved through buildUrl
  // (not just that the fake happened to tolerate whatever URL was passed):
  // if resolution were wrong, the child feed lookup above would have missed
  // entirely and stats.added would be 0.
  EXPECT_NE(std::find(be.downloadedUrls.begin(), be.downloadedUrls.end(), "http://s/opds/authors/b1"),
            be.downloadedUrls.end());
  EXPECT_NE(std::find(be.downloadedUrls.begin(), be.downloadedUrls.end(), "http://s/opds/authors/b2"),
            be.downloadedUrls.end());
}

TEST(OpdsSyncEngine, TerminatesOnCycleWithoutDoubleDownloading) {
  FakeBackend be;
  // Forward href is relative: "child" resolves (append semantics) against
  // "http://s/opds" to "http://s/opds/child". The back-link to root, however,
  // stays absolute — a relative href can't express "return to the start URL"
  // under append-only resolution (it would resolve to a nonsensical nested
  // path), and real OPDS feeds commonly use an absolute "start" link for
  // exactly this reason.
  be.feeds["http://s/opds"] = {{nav("Loop", "child")}, ""};
  be.feeds["http://s/opds/child"] = {{book("A", "1", "b1"), nav("Back", "http://s/opds")}, ""};
  FakeObserver ob;
  OpdsSyncEngine engine(be, ob);
  auto stats = engine.run("http://s/opds");
  EXPECT_EQ(stats.added, 1);  // visited-guard prevents re-walking root
}

TEST(OpdsSyncEngine, FollowsPagination) {
  FakeBackend be;
  // Relative next-page href: "page2" resolves against "http://s/opds" to
  // "http://s/opds/page2". The second page's book href then resolves against
  // ITS feed URL: "b2" -> "http://s/opds/page2/b2".
  be.feeds["http://s/opds"] = {{book("A", "1", "b1")}, "page2"};
  be.feeds["http://s/opds/page2"] = {{book("A", "2", "b2")}, ""};
  FakeObserver ob;
  OpdsSyncEngine engine(be, ob);
  auto stats = engine.run("http://s/opds");
  EXPECT_EQ(stats.added, 2);
  EXPECT_NE(std::find(be.downloadedUrls.begin(), be.downloadedUrls.end(), "http://s/opds/b1"),
            be.downloadedUrls.end());
  EXPECT_NE(std::find(be.downloadedUrls.begin(), be.downloadedUrls.end(), "http://s/opds/page2/b2"),
            be.downloadedUrls.end());
}

TEST(OpdsSyncEngine, CountsFailuresAndContinues) {
  FakeBackend be;
  // Relative book hrefs: "b1"/"b2" resolve against "http://s/opds" to
  // "http://s/opds/b1" / "http://s/opds/b2". failDownloads is keyed by the
  // RESOLVED url, proving the engine passes buildUrl's output (not the raw
  // href) to download().
  be.feeds["http://s/opds"] = {{book("A", "1", "b1"), book("A", "2", "b2")}, ""};
  be.failDownloads.insert("http://s/opds/b1");
  FakeObserver ob;
  OpdsSyncEngine engine(be, ob);
  auto stats = engine.run("http://s/opds");
  EXPECT_EQ(stats.added, 1);
  EXPECT_EQ(stats.failed, 1);
}

TEST(OpdsSyncEngine, StopsWhenObserverCancels) {
  FakeBackend be;
  // Third entry, positioned AFTER the cancellation point, already exists on
  // disk. The exists()-check happens BEFORE onBeforeDownload() is consulted,
  // so it does not depend on observer state at all: if the walk truly stops
  // (engine's `return stats;` on cancel), book 3 is never even reached, so
  // stats.skipped stays 0. If cancel merely skipped one item (a `continue;`
  // regression instead of `return stats;`), the loop would reach book 3, call
  // exists() -> true, and increment stats.skipped to 1 — distinguishing
  // "stop the whole walk" from "skip one item" in a way that plain
  // stats.added can't (the observer keeps rejecting every book from the
  // cancellation point onward regardless, so added would be 1 either way).
  be.feeds["http://s/opds"] = {{book("A", "1", "b1"), book("A", "2", "b2"), book("A", "3", "b3")}, ""};
  be.existing.insert(opdsLocalEpubPath("A", "3"));
  FakeObserver ob;
  ob.allowBeforeCancel = 1;  // allow first, cancel before second
  OpdsSyncEngine engine(be, ob);
  auto stats = engine.run("http://s/opds");
  EXPECT_EQ(stats.added, 1);
  EXPECT_EQ(stats.skipped, 0);  // book 3 never reached -- proves the walk stopped, not just skipped one book
  const std::string book3Target = opdsLocalEpubPath("A", "3");
  EXPECT_EQ(std::find(be.downloaded.begin(), be.downloaded.end(), book3Target), be.downloaded.end());
  EXPECT_EQ(be.downloaded.size(), 1u);
}

TEST(OpdsSyncEngine, RespectsMaxDepthCap) {
  // Build a chain of navigation feeds 10 deep: root (depth 0) -> d1 -> d2 ->
  // ... -> d9, each with a single nav entry to the next. A book planted at
  // depth 2 (well within OpdsSyncEngine::MAX_DEPTH == 8) must download; a
  // book planted inside the feed reachable only at depth 9 (beyond the cap)
  // must never even be fetched, let alone downloaded, proving the
  // `if (depth > MAX_DEPTH) continue;` guard discriminates by depth rather
  // than being a no-op.
  FakeBackend be;
  be.feeds["http://s/opds"] = {{nav("Nav1", "http://s/d1")}, ""};
  be.feeds["http://s/d1"] = {{nav("Nav2", "http://s/d2")}, ""};
  be.feeds["http://s/d2"] = {{book("A", "Within", "http://s/bWithin"), nav("Nav3", "http://s/d3")}, ""};
  be.feeds["http://s/d3"] = {{nav("Nav4", "http://s/d4")}, ""};
  be.feeds["http://s/d4"] = {{nav("Nav5", "http://s/d5")}, ""};
  be.feeds["http://s/d5"] = {{nav("Nav6", "http://s/d6")}, ""};
  be.feeds["http://s/d6"] = {{nav("Nav7", "http://s/d7")}, ""};
  be.feeds["http://s/d7"] = {{nav("Nav8", "http://s/d8")}, ""};
  be.feeds["http://s/d8"] = {{nav("Nav9", "http://s/d9")}, ""};  // processed at depth 8 (== MAX_DEPTH); queues d9 at depth 9
  be.feeds["http://s/d9"] = {{book("A", "TooDeep", "http://s/bTooDeep")}, ""};  // never fetched: depth 9 > MAX_DEPTH

  FakeObserver ob;
  OpdsSyncEngine engine(be, ob);
  auto stats = engine.run("http://s/opds");

  EXPECT_EQ(stats.added, 1);  // only the within-cap book
  const std::string withinTarget = opdsLocalEpubPath("A", "Within");
  const std::string tooDeepTarget = opdsLocalEpubPath("A", "TooDeep");
  EXPECT_NE(std::find(be.downloaded.begin(), be.downloaded.end(), withinTarget), be.downloaded.end());
  EXPECT_EQ(std::find(be.downloaded.begin(), be.downloaded.end(), tooDeepTarget), be.downloaded.end());
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

namespace {

// Backend where every fetchFeed synthesizes a brand-new navigation child
// (a bottomless catalog: feed1 -> feed2 -> feed3 -> ...). Deterministic
// (counter-derived URLs, no randomness). Without the visited-feed cap this
// walk never terminates; it exists to prove the cap stops it gracefully.
class UnboundedBackend : public OpdsSyncBackend {
 public:
  int fetchCount = 0;
  bool fetchFeed(const std::string&, std::vector<OpdsSyncEntry>& out, std::string& next) override {
    fetchCount++;
    out = {nav("Next", "http://s/feed" + std::to_string(fetchCount))};
    next = "";
    return true;
  }
  bool exists(const std::string&) override { return false; }
  OpdsDownloadOutcome download(const std::string&, const std::string&) override {
    return OpdsDownloadOutcome::Ok;
  }
};

}  // namespace

TEST(OpdsSyncEngine, StopsAtVisitedFeedCapOnUnboundedCatalog) {
  UnboundedBackend be;
  FakeObserver ob;
  OpdsSyncEngine engine(be, ob, /*maxVisitedFeeds=*/3);
  auto stats = engine.run("http://s/opds");

  // The walk must terminate (the test itself would hang otherwise) and
  // report that it stopped early rather than exhausting the (infinite) feed.
  EXPECT_TRUE(stats.limitReached);
  // Exactly `maxVisitedFeeds` feeds get inserted into `visited` and fetched;
  // the feed that would be the (maxVisitedFeeds+1)th is never fetched.
  EXPECT_EQ(be.fetchCount, 3);
}

TEST(OpdsSyncEngine, DeduplicatesBookSeenViaMultipleTrees) {
  // The same book is reachable via two navigation trees (e.g. "By author" and
  // "By series"). It must be handled once per run, not re-downloaded per tree.
  FakeBackend be;
  be.feeds["http://s/opds"] = {{nav("By Author", "authors"), nav("By Series", "series")}, ""};
  // Both leaf feeds list the SAME book via an absolute download href, so it
  // resolves to the identical URL from either tree.
  be.feeds["http://s/opds/authors"] = {{book("A", "1", "http://s/dl/9")}, ""};
  be.feeds["http://s/opds/series"] = {{book("A", "1", "http://s/dl/9")}, ""};
  FakeObserver ob;
  OpdsSyncEngine engine(be, ob);
  auto stats = engine.run("http://s/opds");
  EXPECT_EQ(stats.added, 1);          // downloaded once despite two trees
  EXPECT_EQ(be.downloaded.size(), 1u);
}
