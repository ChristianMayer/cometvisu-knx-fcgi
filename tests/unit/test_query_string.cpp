// Copyright (C) 2026 Christian Mayer and the CometVisu contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include <gtest/gtest.h>

#include "util/query_string.h"

using namespace cvknxd;

TEST(QueryStringTest, EmptyInput) {
  QueryString qs("");
  EXPECT_FALSE(qs.has("anything"));
  EXPECT_EQ(qs.size(), 0);
}

TEST(QueryStringTest, SingleParameter) {
  QueryString qs("a=1/2/3");
  EXPECT_TRUE(qs.has("a"));
  auto val = qs.get("a");
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(*val, "1/2/3");
}

TEST(QueryStringTest, MultipleParameters) {
  QueryString qs("a=1/2/3&t=5&a=4/5/6");
  EXPECT_TRUE(qs.has("a"));
  EXPECT_TRUE(qs.has("t"));

  auto first = qs.get("a");
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(*first, "1/2/3");

  auto all = qs.get_all("a");
  ASSERT_EQ(all.size(), 2);
  EXPECT_EQ(all[0], "1/2/3");
  EXPECT_EQ(all[1], "4/5/6");

  auto t_val = qs.get("t");
  ASSERT_TRUE(t_val.has_value());
  EXPECT_EQ(*t_val, "5");
}

TEST(QueryStringTest, MissingParameter) {
  QueryString qs("a=foo");
  EXPECT_FALSE(qs.has("b"));
  auto val = qs.get("b");
  EXPECT_FALSE(val.has_value());
  EXPECT_TRUE(qs.get_all("b").empty());
}

TEST(QueryStringTest, CometVisuLoginParams) {
  QueryString qs("u=admin&p=secret&d=visu-living");
  EXPECT_EQ(*qs.get("u"), "admin");
  EXPECT_EQ(*qs.get("p"), "secret");
  EXPECT_EQ(*qs.get("d"), "visu-living");
}

TEST(QueryStringTest, CometVisuReadParams) {
  QueryString qs("s=abc123&a=KNX:1/2/3&a=KNX:4/5/6&t=0&i=42");
  EXPECT_EQ(*qs.get("s"), "abc123");
  auto addrs = qs.get_all("a");
  ASSERT_EQ(addrs.size(), 2);
  EXPECT_EQ(addrs[0], "KNX:1/2/3");
  EXPECT_EQ(addrs[1], "KNX:4/5/6");
  EXPECT_EQ(*qs.get("t"), "0");
  EXPECT_EQ(*qs.get("i"), "42");
}

TEST(QueryStringTest, URLDecoding) {
  QueryString qs("key=hello%20world&path=%2Ftmp%2Ftest");
  EXPECT_EQ(*qs.get("key"), "hello world");
  EXPECT_EQ(*qs.get("path"), "/tmp/test");
}

TEST(QueryStringTest, ValueWithEquals) {
  QueryString qs("a=foo=bar");
  auto val = qs.get("a");
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(*val, "foo=bar");
}

TEST(QueryStringTest, SmallRequestDoesNotReportLimitExceeded) {
  QueryString qs("s=abc123&a=1/2/3&a=4/5/6&t=0&i=42");
  EXPECT_FALSE(qs.limit_exceeded());
  EXPECT_EQ(qs.get_all("a").size(), 2);
}

// ---------------------------------------------------------------------------
// Regression test for https://github.com/CometVisu/CometVisu/issues/1521
//
// CometVisu subscribes every address of the config in ONE request.  The old
// limits (250 values per key / 256 total pairs) silently dropped everything
// beyond the 250th address, so those tiles stayed empty forever — neither the
// initial read nor any long poll ever saw them.
// ---------------------------------------------------------------------------
TEST(QueryStringTest, HundredsOfAddressesAreNotTruncated) {
  std::string q = "i=981&t=0&s=0123456789abcdef";
  constexpr size_t kAddresses = 600;
  for (size_t i = 0; i < kAddresses; ++i) {
    q += "&a=KNX:11/2/" + std::to_string(i);
  }

  QueryString qs(q);

  EXPECT_FALSE(qs.limit_exceeded());
  EXPECT_EQ(*qs.get("i"), "981");
  EXPECT_EQ(*qs.get("t"), "0");
  EXPECT_EQ(*qs.get("s"), "0123456789abcdef");

  const auto all = qs.get_all("a");
  ASSERT_EQ(all.size(), kAddresses);
  EXPECT_EQ(all.front(), "KNX:11/2/0");

  const std::string last_expected = "KNX:11/2/" + std::to_string(kAddresses - 1);
  EXPECT_EQ(all.back(), last_expected);
}

TEST(QueryStringTest, UniqueKeyLimitIsEnforcedAndReported) {
  const size_t limit = QueryString::kMaxQueryParams;
  std::string q;
  for (size_t i = 0; i < limit + 10; ++i) {
    if (i > 0) {
      q += '&';
    }
    q += "p" + std::to_string(i) + "=v" + std::to_string(i);
  }

  QueryString qs(q);

  // Should stop at kMaxQueryParams
  EXPECT_EQ(qs.size(), limit);
  EXPECT_TRUE(qs.has("p0"));
  EXPECT_EQ(*qs.get("p0"), "v0");
  EXPECT_FALSE(qs.has("p" + std::to_string(limit + 5)));

  // Dropping data must never be silent
  EXPECT_TRUE(qs.limit_exceeded());
}

TEST(QueryStringTest, ValueCountPerKeyLimitIsEnforcedAndReported) {
  const size_t limit = QueryString::kMaxValuesPerKey;
  std::string q;
  for (size_t i = 0; i < limit + 10; ++i) {
    if (i > 0) {
      q += '&';
    }
    q += "a=v" + std::to_string(i);
  }

  QueryString qs(q);
  const auto all = qs.get_all("a");

  // Should stop exactly at kMaxValuesPerKey
  ASSERT_EQ(all.size(), limit);
  EXPECT_EQ(all.front(), "v0");
  const std::string last_expected = "v" + std::to_string(limit - 1);
  EXPECT_EQ(all.back(), last_expected);

  EXPECT_TRUE(qs.limit_exceeded());
}

TEST(QueryStringTest, TotalParamCountLimitIsEnforcedAndReported) {
  // Alternate between two keys so the total pair limit is reached before
  // the per-key limit.
  const size_t limit = QueryString::kMaxTotalPairs;
  std::string q;
  for (size_t i = 0; i < limit + 10; ++i) {
    if (i > 0) {
      q += '&';
    }
    q += std::string((i % 2 == 0) ? "a" : "b") + "=v" + std::to_string(i);
  }

  QueryString qs(q);

  EXPECT_EQ(qs.size(), 2);

  const auto a = qs.get_all("a");
  const auto b = qs.get_all("b");

  // Exactly kMaxTotalPairs values should be retained.
  EXPECT_EQ(a.size() + b.size(), limit);
  ASSERT_EQ(a.size(), limit / 2);
  ASSERT_EQ(b.size(), limit / 2);

  EXPECT_TRUE(qs.limit_exceeded());
}
