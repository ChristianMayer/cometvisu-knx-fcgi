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

/**
 * @file test_cemi.cpp
 * @brief Hex vectors are the contract for the cEMI codec.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "../../src/knxnetip/cemi.h"

namespace cvknx::cemi {
namespace {

using Bytes = std::vector<std::uint8_t>;

TEST(CemiTest, EncodeGroupWriteOneByte) {
  const auto frame = encode_group_request(0x1101, 0x0A03, {0x00, 0x80, 0x42});
  const Bytes expected = {0x11, 0x00, 0xBC, 0xE0, 0x11, 0x01, 0x0A, 0x03, 0x02, 0x00, 0x80, 0x42};
  EXPECT_EQ(frame, expected);
}

TEST(CemiTest, EncodeGroupWriteTwoBytes) {
  const auto frame = encode_group_request(0x0000, 0x0A03, {0x00, 0x80, 0x0C, 0x6F});
  ASSERT_EQ(frame.size(), 13U);
  EXPECT_EQ(frame[8], 0x03);  // length byte = APDU octets - 1
  EXPECT_EQ(Bytes(frame.begin() + 9, frame.end()), (Bytes{0x00, 0x80, 0x0C, 0x6F}));
}

TEST(CemiTest, EncodeGroupValueRead) {
  const auto frame = encode_group_request(0x1101, 0x0A03, {0x00, 0x00});
  ASSERT_EQ(frame.size(), 11U);
  EXPECT_EQ(frame[8], 0x01);
  EXPECT_EQ(Bytes(frame.begin() + 9, frame.end()), (Bytes{0x00, 0x00}));
}

TEST(CemiTest, EncodeEmptyApduRejected) {
  EXPECT_TRUE(encode_group_request(0x1101, 0x0A03, {}).empty());
  EXPECT_TRUE(encode_group_indication(0x1101, 0x0A03, {}).empty());
}

TEST(CemiTest, EncodeGroupIndicationOneByte) {
  // Routing participants send indications: the message code is 0x29.
  const auto frame = encode_group_indication(0xFFFA, 0x0A03, {0x00, 0x80, 0x42});
  const Bytes expected = {0x29, 0x00, 0xBC, 0xE0, 0xFF, 0xFA, 0x0A, 0x03, 0x02, 0x00, 0x80, 0x42};
  EXPECT_EQ(frame, expected);
}

TEST(CemiTest, EncodeGroupIndicationTwoBytes) {
  const auto frame = encode_group_indication(0x1101, 0x0A03, {0x00, 0x80, 0x0C, 0x6F});
  ASSERT_EQ(frame.size(), 13U);
  EXPECT_EQ(frame[0], kMessageCodeLDataInd);
  EXPECT_EQ(frame[8], 0x03);  // length byte = APDU octets - 1
  EXPECT_EQ(Bytes(frame.begin() + 9, frame.end()), (Bytes{0x00, 0x80, 0x0C, 0x6F}));
}

TEST(CemiTest, IndicationRoundTripsThroughTheParser) {
  const auto frame = encode_group_indication(0x1101, 0x0A04, {0x00, 0x80, 0x01});
  const auto parsed = parse_ldata_ind(frame);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->source, 0x1101);
  EXPECT_EQ(parsed->destination, 0x0A04);
  EXPECT_EQ(parsed->apdu, (Bytes{0x00, 0x80, 0x01}));
}

TEST(CemiTest, ParseLDataIndGroupWrite) {
  const Bytes raw = {0x29, 0x00, 0xBC, 0xE0, 0x11, 0x01, 0x0A, 0x03, 0x02, 0x00, 0x80, 0x42};
  const auto frame = parse_ldata_ind(raw);
  ASSERT_TRUE(frame.has_value());
  EXPECT_TRUE(frame->is_group);
  EXPECT_EQ(frame->source, 0x1101);
  EXPECT_EQ(frame->destination, 0x0A03);
  EXPECT_EQ(frame->apdu, (Bytes{0x00, 0x80, 0x42}));
}

TEST(CemiTest, ParseLDataIndIgnoresIndividualDestination) {
  // Control field 2 without the group bit.
  const Bytes raw = {0x29, 0x00, 0xBC, 0x60, 0x11, 0x01, 0x11, 0x02, 0x01, 0x00, 0x81};
  EXPECT_FALSE(parse_ldata_ind(raw).has_value());
}

TEST(CemiTest, ParseLDataIndDropsConfirmations) {
  // 0x2E = L_Data.con: our own write must never reach the cache.
  const Bytes raw = {0x2E, 0x00, 0xBC, 0xE0, 0x11, 0x01, 0x0A, 0x03, 0x02, 0x00, 0x80, 0x42};
  EXPECT_FALSE(parse_ldata_ind(raw).has_value());
}

TEST(CemiTest, ParseLDataIndDropsBusmonitorAndRequests) {
  EXPECT_FALSE(
      parse_ldata_ind(Bytes{0x2F, 0x00, 0xBC, 0xE0, 0x11, 0x01, 0x0A, 0x03, 0x02, 0x00, 0x80, 0x42})
          .has_value());
  EXPECT_FALSE(
      parse_ldata_ind(Bytes{0x11, 0x00, 0xBC, 0xE0, 0x11, 0x01, 0x0A, 0x03, 0x02, 0x00, 0x80, 0x42})
          .has_value());
}

TEST(CemiTest, ParseTruncatedFrameRejected) {
  const Bytes full = {0x29, 0x00, 0xBC, 0xE0, 0x11, 0x01, 0x0A, 0x03, 0x02, 0x00, 0x80, 0x42};
  for (std::size_t len = 0; len < full.size(); ++len) {
    const Bytes prefix(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(len));
    EXPECT_FALSE(parse_ldata_ind(prefix).has_value()) << "prefix length " << len;
  }
}

TEST(CemiTest, ParseLengthMismatchRejected) {
  // Length byte says 3 APDU octets but only 3 are present in total.
  const Bytes raw = {0x29, 0x00, 0xBC, 0xE0, 0x11, 0x01, 0x0A, 0x03, 0x03, 0x00, 0x80};
  EXPECT_FALSE(parse_ldata_ind(raw).has_value());
}

TEST(CemiTest, EncodeThenParseRoundTrips) {
  const auto encoded = encode_group_request(0x1101, 0x0A03, {0x00, 0x80, 0x0C, 0x6F});
  Bytes indication = encoded;
  indication[0] = kMessageCodeLDataInd;
  const auto frame = parse_ldata_ind(indication);
  ASSERT_TRUE(frame.has_value());
  EXPECT_EQ(frame->destination, 0x0A03);
  EXPECT_EQ(frame->apdu, (Bytes{0x00, 0x80, 0x0C, 0x6F}));
}

}  // namespace
}  // namespace cvknx::cemi
