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
 * @file cemi.cpp
 * @brief cEMI frame encode/decode.
 */

#include "knxnetip/cemi.h"

namespace cvknx::cemi {
namespace {

/// Standard frame header length before the APDU.
constexpr std::size_t kHeaderSize = 9;

/// Encode a group-addressed L_Data frame with an explicit message code.
std::vector<std::uint8_t> encode_group_frame(std::uint8_t message_code, std::uint16_t source,
                                             std::uint16_t destination,
                                             const std::vector<std::uint8_t>& apdu) {
  if (apdu.empty()) {
    return {};
  }

  std::vector<std::uint8_t> frame;
  frame.reserve(kHeaderSize + apdu.size());
  frame.push_back(message_code);
  frame.push_back(0x00);  // additional info length
  frame.push_back(kControl1Standard);
  frame.push_back(kControl2Group);
  frame.push_back(static_cast<std::uint8_t>((source >> 8) & 0xFF));
  frame.push_back(static_cast<std::uint8_t>(source & 0xFF));
  frame.push_back(static_cast<std::uint8_t>((destination >> 8) & 0xFF));
  frame.push_back(static_cast<std::uint8_t>(destination & 0xFF));
  frame.push_back(static_cast<std::uint8_t>(apdu.size() - 1));
  frame.insert(frame.end(), apdu.begin(), apdu.end());
  return frame;
}

}  // namespace

std::vector<std::uint8_t> encode_group_request(std::uint16_t source, std::uint16_t destination,
                                               const std::vector<std::uint8_t>& apdu) {
  return encode_group_frame(kMessageCodeLDataReq, source, destination, apdu);
}

std::vector<std::uint8_t> encode_group_indication(std::uint16_t source, std::uint16_t destination,
                                                  const std::vector<std::uint8_t>& apdu) {
  return encode_group_frame(kMessageCodeLDataInd, source, destination, apdu);
}

std::optional<CemiFrame> parse_ldata_ind(const std::uint8_t* data, std::size_t size) {
  if (data == nullptr || size < kHeaderSize) {
    return std::nullopt;
  }
  // Only indications carry bus traffic.  L_Data.con (0x2E) is the transport
  // acknowledgement of our own write and must never reach the cache.
  if (data[0] != kMessageCodeLDataInd) {
    return std::nullopt;
  }

  const std::size_t additional_info_len = data[1];
  const std::size_t apdu_offset = 2 + kHeaderSize - 2 + additional_info_len;
  if (size < apdu_offset + 2) {  // at least TPCI/APCI must follow
    return std::nullopt;
  }

  // data[apdu_offset - 1] is the length byte: (APDU octets) - 1.
  const std::size_t apdu_len = static_cast<std::size_t>(data[apdu_offset - 1]) + 1;
  if (size != apdu_offset + apdu_len) {
    return std::nullopt;  // truncated or padded — reject rather than guess
  }

  const std::uint8_t control2 = data[3];
  const bool is_group = (control2 & 0x80) != 0;
  if (!is_group) {
    return std::nullopt;  // individual (management) traffic is not for us
  }

  CemiFrame frame;
  frame.is_group = true;
  frame.source = static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[4]) << 8) | data[5]);
  frame.destination =
      static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[6]) << 8) | data[7]);
  frame.apdu.assign(data + apdu_offset, data + apdu_offset + apdu_len);
  return frame;
}

std::optional<CemiFrame> parse_ldata_ind(const std::vector<std::uint8_t>& data) {
  return parse_ldata_ind(data.data(), data.size());
}

}  // namespace cvknx::cemi
