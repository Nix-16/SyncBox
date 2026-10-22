#include <gtest/gtest.h>

#include <syncbox/fs/ignore_rules.hpp>

TEST(IgnoreRulesTest, MatchesBasenamesAtAnyDepth) {
  const syncbox::fs::IgnoreRules rules({"*.tmp"});

  EXPECT_TRUE(rules.ignored("a.tmp"));
  EXPECT_TRUE(rules.ignored("dir/a.tmp"));
  EXPECT_TRUE(rules.ignored("dir/sub/a.tmp"));
  EXPECT_FALSE(rules.ignored("a.txt"));
  EXPECT_FALSE(rules.ignored("a.tmp2"));
}

TEST(IgnoreRulesTest, MatchesPathPatternsAndDirectories) {
  const syncbox::fs::IgnoreRules rules({".git/**", "build/**", "docs/*.md"});

  EXPECT_TRUE(rules.ignored(".git"));
  EXPECT_TRUE(rules.ignored(".git/config"));
  EXPECT_TRUE(rules.ignored(".git/objects/ab/cd"));
  EXPECT_TRUE(rules.ignored("build"));
  EXPECT_TRUE(rules.ignored("build/x/y.o"));
  EXPECT_TRUE(rules.ignored("docs/spec.md"));
  EXPECT_FALSE(rules.ignored("docs/sub/spec.md"));
  EXPECT_FALSE(rules.ignored("src/build.cpp"));
  EXPECT_FALSE(rules.ignored("src/main.cpp"));
}

TEST(IgnoreRulesTest, SupportsDoubleStarAndQuestionMark) {
  const syncbox::fs::IgnoreRules rules({"logs/**/*.log", "file?.txt"});

  EXPECT_TRUE(rules.ignored("logs/app.log"));
  EXPECT_TRUE(rules.ignored("logs/2026/app.log"));
  EXPECT_FALSE(rules.ignored("logs/2026/app.txt"));
  EXPECT_TRUE(rules.ignored("file1.txt"));
  EXPECT_TRUE(rules.ignored("dir/file9.txt"));
  EXPECT_FALSE(rules.ignored("file10.txt"));
}

TEST(IgnoreRulesTest, EmptyRulesIgnoreNothing) {
  const syncbox::fs::IgnoreRules rules;
  EXPECT_TRUE(rules.empty());
  EXPECT_FALSE(rules.ignored("anything.txt"));
}
