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
