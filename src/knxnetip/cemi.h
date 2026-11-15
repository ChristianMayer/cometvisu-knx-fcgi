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
 * @file cemi.h
 * @brief cEMI frame codec (KNX link layer) for the direct transports.
 *
 * Layout (standard frame):
 *
 * @code
 * [0]     message code      0x11 L_Data.req | 0x29 L_Data.ind | 0x2E L_Data.con
 * [1]     additional info length 0x00
 * [2]     control field 1
 * [3]     control field 2
 * [4..5]  source address (individual address)
 * [6..7]  destination address (group or individual)
 * [8]     length = (number of APDU octets) - 1
 * [9..]   APDU (TPCI/APCI + data)
 * @endcode
 *
 * Control fields used when sending: 0xBC (standard frame, repeat not allowed,
 * system broadcast, low priority) and 0xE0 (group destination, hop count 6).
 * Received frames are parsed generically — they are never compared against
 * these values.
 *
 * Only L_Data.ind reaches the cache.  L_Data.con confirms *our own* write and
 * must be dropped, otherwise every write would be delivered twice (AGENTS.md
 * rule 1).
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace cvknx::cemi {

inline constexpr std::uint8_t kMessageCodeLDataReq = 0x11;
inline constexpr std::uint8_t kMessageCodeLDataInd = 0x29;
inline constexpr std::uint8_t kMessageCodeLDataCon = 0x2E;

inline constexpr std::uint8_t kControl1Standard = 0xBC;
inline constexpr std::uint8_t kControl2Group = 0xE0;

/// A decoded link-layer frame.
struct CemiFrame {
  bool is_group = false;           ///< destination is a group address
  std::uint16_t source = 0;        ///< individual address of the sender
  std::uint16_t destination = 0;   ///< group (or individual) address
  std::vector<std::uint8_t> apdu;  ///< TPCI/APCI + data
};

/// Encode an L_Data.req for a group value.
/// @param source Individual address (0x0000 = let the device fill it in).
/// @param destination 16-bit group address.
/// @param apdu TPCI/APCI + data (at least one byte).
/// @return the cEMI frame, or an empty vector when @p apdu is empty (the
///         length byte would underflow).
[[nodiscard]] std::vector<std::uint8_t> encode_group_request(std::uint16_t source,
                                                             std::uint16_t destination,
                                                             const std::vector<std::uint8_t>& apdu);

/// Parse an L_Data.ind with a group destination.
/// @return the frame, or std::nullopt for any other message code, a truncated
///         frame, an individual destination or a length mismatch.
[[nodiscard]] std::optional<CemiFrame> parse_ldata_ind(const std::uint8_t* data, std::size_t size);

/// Convenience overload.
[[nodiscard]] std::optional<CemiFrame> parse_ldata_ind(const std::vector<std::uint8_t>& data);

}  // namespace cvknx::cemi
