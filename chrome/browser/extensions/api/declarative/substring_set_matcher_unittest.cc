// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/extensions/api/declarative/substring_set_matcher.h"

#include <set>
#include <string>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"

using extensions::SubstringPattern;
using extensions::SubstringSetMatcher;

TEST(SubstringSetMatcherTest, SubstringPattern) {
  SubstringPattern r1("Test", 2);
  EXPECT_EQ("Test", r1.pattern());
  EXPECT_EQ(2, r1.id());

  EXPECT_FALSE(r1 < r1);
  SubstringPattern r2("Test", 3);
  EXPECT_TRUE(r1 < r2);
  SubstringPattern r3("ZZZZ", 2);
  EXPECT_TRUE(r1 < r3);
}

namespace {
void TestOnePattern(const std::string& test_string,
                    const std::string& pattern,
                    bool is_match) {
  std::string test =
      "TestOnePattern(" + test_string + ", " + pattern + ", " +
      (is_match ? "1" : "0") + ")";
  std::vector<const SubstringPattern*> patterns;
  SubstringPattern substring_pattern(pattern, 1);
  patterns.push_back(&substring_pattern);
  SubstringSetMatcher matcher;
  matcher.RegisterPatterns(patterns);
  std::set<int> matches;
  matcher.Match(test_string, &matches);

  size_t expected_matches = (is_match ? 1 : 0);
  EXPECT_EQ(expected_matches, matches.size()) << test;
  EXPECT_EQ(is_match, matches.find(1) != matches.end()) << test;
}

void TestTwoPatterns(const std::string& test_string,
                     const std::string& pattern_1,
                     const std::string& pattern_2,
                     bool is_match_1,
                     bool is_match_2) {
  std::string test =
      "TestTwoPatterns(" + test_string + ", " + pattern_1 + ", " + pattern_2 +
      ", " + (is_match_1 ? "1" : "0") + ", " + (is_match_2 ? "1" : "0") + ")";
  SubstringPattern substring_pattern_1(pattern_1, 1);
  SubstringPattern substring_pattern_2(pattern_2, 2);
  // In order to make sure that the order in which patterns are registered
  // does not make any difference we try both permutations.
  for (int permutation = 0; permutation < 2; ++permutation) {
    std::vector<const SubstringPattern*> patterns;
    if (permutation == 0) {
      patterns.push_back(&substring_pattern_1);
      patterns.push_back(&substring_pattern_2);
    } else {
      patterns.push_back(&substring_pattern_2);
      patterns.push_back(&substring_pattern_1);
    }
    SubstringSetMatcher matcher;
    matcher.RegisterPatterns(patterns);
    std::set<int> matches;
    matcher.Match(test_string, &matches);

    size_t expected_matches = (is_match_1 ? 1 : 0) + (is_match_2 ? 1 : 0);
    EXPECT_EQ(expected_matches, matches.size()) << test;
    EXPECT_EQ(is_match_1, matches.find(1) != matches.end()) << test;
    EXPECT_EQ(is_match_2, matches.find(2) != matches.end()) << test;
  }
}
}

TEST(SubstringSetMatcherTest, TestMatcher) {
  // Test overlapping patterns
  // String    abcde
  // Pattern 1  bc
  // Pattern 2   cd
  TestTwoPatterns("abcde", "bc", "cd", true, true);

  // Test subpatterns - part 1
  // String    abcde
  // Pattern 1  bc
  // Pattern 2  b
  TestTwoPatterns("abcde", "bc", "b", true, true);

  // Test subpatterns - part 2
  // String    abcde
  // Pattern 1  bc
  // Pattern 2   c
  TestTwoPatterns("abcde", "bc", "c", true, true);

  // Test identical matches
  // String    abcde
  // Pattern 1 abcde
  TestOnePattern("abcde", "abcde", true);

  // Test multiple matches
  // String    aaaaa
  // Pattern 1 a
  TestOnePattern("abcde", "a", true);

  // Test matches at beginning and end
  // String    abcde
  // Pattern 1 ab
  // Pattern 2    de
  TestTwoPatterns("abcde", "ab", "de", true, true);

  // Test duplicate patterns with different IDs
  // String    abcde
  // Pattern 1  bc
  // Pattern 2  bc
  TestTwoPatterns("abcde", "bc", "bc", true, true);

  // Test non-match
  // String    abcde
  // Pattern 1        fg
  TestOnePattern("abcde", "fg", false);

  // Test empty pattern and too long pattern
  // String    abcde
  // Pattern 1
  // Pattern 2 abcdef
  TestTwoPatterns("abcde", "", "abcdef", true, false);
}

TEST(SubstringSetMatcherTest, RegisterAndRemove) {
  SubstringSetMatcher matcher;

  SubstringPattern pattern_1("a", 1);
  SubstringPattern pattern_2("b", 2);
  SubstringPattern pattern_3("c", 3);

  std::vector<const SubstringPattern*> patterns;
  patterns.push_back(&pattern_1);
  matcher.RegisterPatterns(patterns);

  patterns.clear();
  patterns.push_back(&pattern_2);
  patterns.push_back(&pattern_3);
  matcher.RegisterPatterns(patterns);

  std::set<int> matches;
  matcher.Match("abd", &matches);
  EXPECT_EQ(2u, matches.size());
  EXPECT_TRUE(matches.end() != matches.find(1));
  EXPECT_TRUE(matches.end() != matches.find(2));

  patterns.clear();
  patterns.push_back(&pattern_2);
  matcher.UnregisterPatterns(patterns);

  matches.clear();
  matcher.Match("abd", &matches);
  EXPECT_EQ(1u, matches.size());
  EXPECT_TRUE(matches.end() != matches.find(1));
  EXPECT_TRUE(matches.end() == matches.find(2));

  patterns.clear();
  patterns.push_back(&pattern_1);
  patterns.push_back(&pattern_3);
  matcher.UnregisterPatterns(patterns);
  EXPECT_TRUE(matcher.IsEmpty());
}

TEST(SubstringSetMatcherTest, TestEmptyMatcher) {
  SubstringSetMatcher matcher;
  std::set<int> matches;
  matcher.Match("abd", &matches);
  EXPECT_TRUE(matches.empty());
}
