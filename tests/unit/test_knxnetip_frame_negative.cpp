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
 * @file test_knxnetip_frame_negative.cpp
 * @brief Every rejection path of the KNXnet/IP codec (coverage gate).
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "../../src/knxnetip/knxnetip_frame.h"

namespace cvknx::knxnetip {
namespace {

using Bytes = std::vector<std::uint8_t>;

TEST(FrameNegative, ParseFrameRejectsNullAndShortBuffers) {
  EXPECT_FALSE(parse_frame(nullptr, 0).has_value());
  EXPECT_FALSE(parse_frame(static_cast<const std::uint8_t*>(nullptr), 6).has_value());
  EXPECT_FALSE(parse_frame(Bytes{0x06, 0x10, 0x03, 0x30}).has_value());
}

TEST(FrameNegative, StatusTextCoversEveryDefinedCode) {
  for (std::uint8_t status = 0x00; status <= 0x0B; ++status) {
    const std::string text = connect_status_text(status);
    EXPECT_NE(text, "unknown status") << "status " << static_cast<int>(status);
    EXPECT_FALSE(text.empty());
  }
  EXPECT_EQ(std::string{connect_status_text(0x7F)}, "unknown status");
}

TEST(FrameNegative, ParseHpaiRejectsNullAndShortBuffers) {
  EXPECT_FALSE(parse_hpai(nullptr, 8).has_value());
  const Bytes hpai = encode_hpai(Hpai{0xC0A80101U, 3671});
  for (std::size_t length = 0; length < hpai.size(); ++length) {
    EXPECT_FALSE(parse_hpai(hpai.data(), length).has_value()) << "length " << length;
  }
}

TEST(FrameNegative, ConnectResponseRejectsWrongService) {
  const auto frame = build_frame(ServiceType::RoutingIndication, {0x29});
  EXPECT_FALSE(parse_connect_response(frame).has_value());
}

TEST(FrameNegative, ConnectResponseRejectsBadHpai) {
  // Correct length, but the HPAI advertises IPv4/TCP (0x02).
  const Bytes frame = {0x06, 0x10, 0x02, 0x06, 0x00, 0x14, 0x01, 0x00, 0x08, 0x02,
                       0xC0, 0xA8, 0x01, 0x0A, 0x0E, 0x57, 0x04, 0x04, 0x11, 0x01};
  EXPECT_FALSE(parse_connect_response(frame).has_value());
}

TEST(FrameNegative, StatusResponseRejectsWrongServiceAndShortBody) {
  const auto routing = build_frame(ServiceType::RoutingIndication, {0x29});
  EXPECT_FALSE(parse_status_response(routing, ServiceType::ConnectionStateResponse).has_value());

  const auto too_short = build_frame(ServiceType::ConnectionStateResponse, {0x01});
  EXPECT_FALSE(parse_status_response(too_short, ServiceType::ConnectionStateResponse).has_value());
}

TEST(FrameNegative, TunnellingRequestRejectsWrongServiceAndShortBody) {
  const auto routing = build_frame(ServiceType::RoutingIndication, {0x29});
  EXPECT_FALSE(parse_tunnelling_request(routing).has_value());

  // Structure length byte wrong (0x03 instead of 0x04).
  const auto bad_structure = build_frame(ServiceType::TunnellingRequest, {0x03, 0x01, 0x00, 0x00});
  EXPECT_FALSE(parse_tunnelling_request(bad_structure).has_value());

  const auto too_short = build_frame(ServiceType::TunnellingRequest, {0x04, 0x01});
  EXPECT_FALSE(parse_tunnelling_request(too_short).has_value());
}

TEST(FrameNegative, TunnellingAckRejectsWrongServiceAndShortBody) {
  const auto routing = build_frame(ServiceType::RoutingIndication, {0x29});
  EXPECT_FALSE(parse_tunnelling_ack(routing).has_value());

  const auto bad_structure =
      build_frame(ServiceType::TunnellingAck, {0x03, 0x01, 0x00, 0x00, 0x00});
  EXPECT_FALSE(parse_tunnelling_ack(bad_structure).has_value());

  const auto too_short = build_frame(ServiceType::TunnellingAck, {0x04, 0x01});
  EXPECT_FALSE(parse_tunnelling_ack(too_short).has_value());
}

TEST(FrameNegative, RoutingIndicationRejectsWrongServiceAndEmptyBody) {
  const auto empty = build_frame(ServiceType::RoutingIndication, {});
  EXPECT_FALSE(parse_routing_indication(empty).has_value());

  const auto wrong = build_frame(ServiceType::SearchRequest, {0x29});
  EXPECT_FALSE(parse_routing_indication(wrong).has_value());
}

TEST(FrameNegative, SearchResponseRejectsWrongServiceAndShortBody) {
  const auto wrong = build_frame(ServiceType::RoutingIndication, {0x29});
  EXPECT_FALSE(parse_search_response(wrong).has_value());

  const auto too_short = build_frame(ServiceType::SearchResponse, Bytes(8, 0x00));
  EXPECT_FALSE(parse_search_response(too_short).has_value());
}

TEST(FrameNegative, SearchResponseRejectsBadHpai) {
  Bytes body(16, 0x00);  // both "HPAIs" have length 0
  const auto frame = build_frame(ServiceType::SearchResponse, body);
  EXPECT_FALSE(parse_search_response(frame).has_value());
}

TEST(FrameNegative, SearchResponseStopsAtMalformedDib) {
  Bytes body;
  const auto hpai = encode_hpai(Hpai{0xC0A8010AU, 3671});
  body.insert(body.end(), hpai.begin(), hpai.end());
  body.insert(body.end(), hpai.begin(), hpai.end());
  body.push_back(0x00);  // DIB length 0 — malformed
  body.push_back(0x01);
  const auto frame = build_frame(ServiceType::SearchResponse, body);

  const auto parsed = parse_search_response(frame);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_TRUE(parsed->device_name.empty());
}

TEST(FrameNegative, SearchResponseSkipsForeignKeyDibsAndReadsFullName) {
  Bytes body;
  const auto hpai = encode_hpai(Hpai{0xC0A8010AU, 3671});
  body.insert(body.end(), hpai.begin(), hpai.end());
  body.insert(body.end(), hpai.begin(), hpai.end());

  // Supported service families DIB (type 0x02) — must be skipped.
  body.push_back(0x04);
  body.push_back(0x02);
  body.push_back(0x02);
  body.push_back(0x01);

  // Device information DIB with a name that fills all 30 bytes (no NUL).
  Bytes dib(54, 0x41);  // 'A'
  dib[0] = 54;
  dib[1] = 0x01;
  dib[4] = 0x11;  // individual address high
  dib[5] = 0x01;
  body.insert(body.end(), dib.begin(), dib.end());

  const auto frame = build_frame(ServiceType::SearchResponse, body);
  const auto parsed = parse_search_response(frame);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->device_name.size(), 30U);
  EXPECT_EQ(parsed->individual_address, 0x1101);
}

TEST(FrameNegative, EmptyBodyFrameRoundTrips) {
  const auto frame = build_frame(ServiceType::DescriptionRequest, {});
  const auto parsed = parse_frame(frame);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_TRUE(parsed->body.empty());
}

}  // namespace
}  // namespace cvknx::knxnetip
