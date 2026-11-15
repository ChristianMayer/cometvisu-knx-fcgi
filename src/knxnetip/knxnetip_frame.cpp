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
 * @file knxnetip_frame.cpp
 * @brief KNXnet/IP framing implementation.  All lengths are derived from the
 *        actual buffer size; every parser validates before reading.
 */

#include "knxnetip/knxnetip_frame.h"

#include <arpa/inet.h>

#include <cstring>

namespace cvknx::knxnetip {
namespace {

std::uint16_t read_u16(const std::uint8_t* data) {
  return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[0]) << 8) | data[1]);
}

void append_u16(std::vector<std::uint8_t>& out, std::uint16_t value) {
  out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
  out.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

/// Length of the body of a given service type, or -1 when variable/unknown.
bool is_known_service(std::uint16_t value) {
  switch (static_cast<ServiceType>(value)) {
    case ServiceType::SearchRequest:
    case ServiceType::SearchResponse:
    case ServiceType::DescriptionRequest:
    case ServiceType::DescriptionResponse:
    case ServiceType::ConnectRequest:
    case ServiceType::ConnectResponse:
    case ServiceType::ConnectionStateRequest:
    case ServiceType::ConnectionStateResponse:
    case ServiceType::DisconnectRequest:
    case ServiceType::DisconnectResponse:
    case ServiceType::TunnellingRequest:
    case ServiceType::TunnellingAck:
    case ServiceType::RoutingIndication:
    case ServiceType::RoutingLostMessage:
    case ServiceType::RoutingBusy:
      return true;
  }
  return false;
}

}  // namespace

const char* connect_status_text(std::uint8_t status) {
  switch (static_cast<ConnectStatus>(status)) {
    case ConnectStatus::NoError:
      return "no error";
    case ConnectStatus::HostProtocolType:
      return "host protocol type not supported";
    case ConnectStatus::ProtocolVersion:
      return "protocol version not supported";
    case ConnectStatus::SequenceNumber:
      return "sequence number error";
    case ConnectStatus::ConnectionId:
      return "unknown connection id";
    case ConnectStatus::ConnectionType:
      return "connection type not supported";
    case ConnectStatus::ConnectionOption:
      return "connection option not supported";
    case ConnectStatus::NoMoreConnections:
      return "no more tunnelling connections available (device tunnel limit reached)";
    case ConnectStatus::NoMoreUniqueConnections:
      return "no more unique tunnelling connections available (device tunnel limit reached)";
    case ConnectStatus::DataConnection:
      return "data connection error";
    case ConnectStatus::KnxConnection:
      return "KNX connection error";
    case ConnectStatus::TunnellingLayer:
      return "tunnelling layer not supported";
  }
  return "unknown status";
}

std::vector<std::uint8_t> build_frame(ServiceType service, const std::vector<std::uint8_t>& body) {
  const auto total = static_cast<std::uint16_t>(kHeaderLength + body.size());
  std::vector<std::uint8_t> frame;
  frame.reserve(total);
  frame.push_back(kHeaderLength);
  frame.push_back(kProtocolVersion);
  append_u16(frame, static_cast<std::uint16_t>(service));
  append_u16(frame, total);
  frame.insert(frame.end(), body.begin(), body.end());
  return frame;
}

std::optional<Frame> parse_frame(const std::uint8_t* data, std::size_t size) {
  if (data == nullptr || size < kHeaderLength) {
    return std::nullopt;
  }
  if (data[0] != kHeaderLength || data[1] != kProtocolVersion) {
    return std::nullopt;
  }
  const std::uint16_t service = read_u16(data + 2);
  const std::uint16_t total_length = read_u16(data + 4);
  if (total_length != size) {
    return std::nullopt;
  }
  // Unknown service types are ignored (routers emit extras) — callers simply
  // get std::nullopt and drop the datagram.
  if (!is_known_service(service)) {
    return std::nullopt;
  }

  Frame frame;
  frame.service = static_cast<ServiceType>(service);
  frame.body.assign(data + kHeaderLength, data + size);
  return frame;
}

std::optional<Frame> parse_frame(const std::vector<std::uint8_t>& data) {
  return parse_frame(data.data(), data.size());
}

std::vector<std::uint8_t> encode_hpai(const Hpai& hpai) {
  std::vector<std::uint8_t> out;
  out.reserve(8);
  out.push_back(0x08);
  out.push_back(kHostProtocolIpv4Udp);
  const std::uint32_t network_address = htonl(hpai.address);
  const auto* bytes = reinterpret_cast<const std::uint8_t*>(&network_address);
  out.insert(out.end(), bytes, bytes + 4);
  append_u16(out, hpai.port);
  return out;
}

std::optional<Hpai> parse_hpai(const std::uint8_t* data, std::size_t size) {
  if (data == nullptr || size < 8 || data[0] != 0x08 || data[1] != kHostProtocolIpv4Udp) {
    return std::nullopt;
  }
  Hpai hpai;
  hpai.address = (static_cast<std::uint32_t>(data[2]) << 24) |
                 (static_cast<std::uint32_t>(data[3]) << 16) |
                 (static_cast<std::uint32_t>(data[4]) << 8) | static_cast<std::uint32_t>(data[5]);
  hpai.port = read_u16(data + 6);
  return hpai;
}

std::optional<std::uint32_t> ipv4_from_string(std::string_view text) {
  if (text.empty()) {
    return std::nullopt;
  }
  std::string copy{text};
  struct in_addr addr {};
  if (::inet_pton(AF_INET, copy.c_str(), &addr) != 1) {
    return std::nullopt;
  }
  return ntohl(addr.s_addr);
}

std::string ipv4_to_string(std::uint32_t address) {
  struct in_addr addr {};
  addr.s_addr = htonl(address);
  char buffer[INET_ADDRSTRLEN] = {};
  if (::inet_ntop(AF_INET, &addr, buffer, sizeof(buffer)) == nullptr) {
    return {};
  }
  return buffer;
}

std::vector<std::uint8_t> build_connect_request(const Hpai& control, const Hpai& data) {
  std::vector<std::uint8_t> body = encode_hpai(control);
  const auto data_hpai = encode_hpai(data);
  body.insert(body.end(), data_hpai.begin(), data_hpai.end());
  body.push_back(kConnectionTypeTunnel);
  body.push_back(kKnxLayerLink);
  body.push_back(0x00);  // reserved
  return build_frame(ServiceType::ConnectRequest, body);
}

std::optional<ConnectResponse> parse_connect_response(const std::vector<std::uint8_t>& frame) {
  const auto parsed = parse_frame(frame);
  if (!parsed || parsed->service != ServiceType::ConnectResponse) {
    return std::nullopt;
  }
  const auto& body = parsed->body;
  if (body.size() < 14) {  // channel + status + HPAI + CRD (2+2)
    return std::nullopt;
  }
  ConnectResponse response;
  response.channel = body[0];
  response.status = body[1];
  const auto endpoint = parse_hpai(body.data() + 2, body.size() - 2);
  if (!endpoint) {
    return std::nullopt;
  }
  response.endpoint = *endpoint;
  // CRD: 0x04 0x04 <individual address hi> <lo>
  response.crd_address = read_u16(body.data() + 12);
  return response;
}

std::vector<std::uint8_t> build_connectionstate_request(std::uint8_t channel, const Hpai& control) {
  std::vector<std::uint8_t> body;
  body.push_back(channel);
  body.push_back(0x00);  // reserved
  const auto hpai = encode_hpai(control);
  body.insert(body.end(), hpai.begin(), hpai.end());
  return build_frame(ServiceType::ConnectionStateRequest, body);
}

std::vector<std::uint8_t> build_disconnect_request(std::uint8_t channel, const Hpai& control) {
  std::vector<std::uint8_t> body;
  body.push_back(channel);
  body.push_back(0x00);  // reserved
  const auto hpai = encode_hpai(control);
  body.insert(body.end(), hpai.begin(), hpai.end());
  return build_frame(ServiceType::DisconnectRequest, body);
}

std::optional<StatusResponse> parse_status_response(const std::vector<std::uint8_t>& frame,
                                                    ServiceType expected) {
  const auto parsed = parse_frame(frame);
  if (!parsed || parsed->service != expected || parsed->body.size() < 2) {
    return std::nullopt;
  }
  return StatusResponse{parsed->body[0], parsed->body[1]};
}

std::vector<std::uint8_t> build_tunnelling_request(std::uint8_t channel, std::uint8_t sequence,
                                                   const std::vector<std::uint8_t>& cemi) {
  std::vector<std::uint8_t> body;
  body.reserve(4 + cemi.size());
  body.push_back(0x04);  // structure length
  body.push_back(channel);
  body.push_back(sequence);
  body.push_back(kStructuringNoMoreInfo);
  body.insert(body.end(), cemi.begin(), cemi.end());
  return build_frame(ServiceType::TunnellingRequest, body);
}

std::optional<TunnellingRequest> parse_tunnelling_request(const std::vector<std::uint8_t>& frame) {
  const auto parsed = parse_frame(frame);
  if (!parsed || parsed->service != ServiceType::TunnellingRequest) {
    return std::nullopt;
  }
  const auto& body = parsed->body;
  if (body.size() < 5 || body[0] != 0x04) {
    return std::nullopt;
  }
  TunnellingRequest request;
  request.channel = body[1];
  request.sequence = body[2];
  request.cemi.assign(body.begin() + 4, body.end());
  return request;
}

std::vector<std::uint8_t> build_tunnelling_ack(std::uint8_t channel, std::uint8_t sequence,
                                               std::uint8_t status) {
  std::vector<std::uint8_t> body;
  body.push_back(0x04);
  body.push_back(channel);
  body.push_back(sequence);
  body.push_back(kStructuringNoMoreInfo);
  body.push_back(status);
  return build_frame(ServiceType::TunnellingAck, body);
}

std::optional<TunnellingAck> parse_tunnelling_ack(const std::vector<std::uint8_t>& frame) {
  const auto parsed = parse_frame(frame);
  if (!parsed || parsed->service != ServiceType::TunnellingAck) {
    return std::nullopt;
  }
  const auto& body = parsed->body;
  if (body.size() < 5 || body[0] != 0x04) {
    return std::nullopt;
  }
  return TunnellingAck{body[1], body[2], body[4]};
}

std::vector<std::uint8_t> build_routing_indication(const std::vector<std::uint8_t>& cemi) {
  return build_frame(ServiceType::RoutingIndication, cemi);
}

std::optional<std::vector<std::uint8_t>> parse_routing_indication(
    const std::vector<std::uint8_t>& frame) {
  const auto parsed = parse_frame(frame);
  if (!parsed || parsed->service != ServiceType::RoutingIndication || parsed->body.empty()) {
    return std::nullopt;
  }
  return parsed->body;
}

std::vector<std::uint8_t> build_search_request(const Hpai& control) {
  return build_frame(ServiceType::SearchRequest, encode_hpai(control));
}

std::optional<SearchResponse> parse_search_response(const std::vector<std::uint8_t>& frame) {
  const auto parsed = parse_frame(frame);
  if (!parsed || parsed->service != ServiceType::SearchResponse) {
    return std::nullopt;
  }
  const auto& body = parsed->body;
  if (body.size() < 16) {
    return std::nullopt;
  }
  const auto control = parse_hpai(body.data(), body.size());
  const auto endpoint = parse_hpai(body.data() + 8, body.size() - 8);
  if (!control || !endpoint) {
    return std::nullopt;
  }

  SearchResponse response;
  response.control = *control;
  response.endpoint = *endpoint;

  // DIBs follow the two HPAIs; look for the device information DIB (0x01).
  std::size_t offset = 16;
  while (offset + 2 <= body.size()) {
    const std::size_t dib_length = body[offset];
    const std::uint8_t dib_type = body[offset + 1];
    if (dib_length < 2 || offset + dib_length > body.size()) {
      break;
    }
    if (dib_type == 0x01 && dib_length >= 54) {
      const std::uint8_t* dib = body.data() + offset + 2;
      response.individual_address = read_u16(dib + 2);
      response.multicast_address = (static_cast<std::uint32_t>(dib[12]) << 24) |
                                   (static_cast<std::uint32_t>(dib[13]) << 16) |
                                   (static_cast<std::uint32_t>(dib[14]) << 8) |
                                   static_cast<std::uint32_t>(dib[15]);
      // 30-byte name, NUL padded.
      const char* name = reinterpret_cast<const char*>(dib + 22);
      std::size_t name_len = 0;
      while (name_len < 30 && name[name_len] != '\0') {
        ++name_len;
      }
      response.device_name.assign(name, name_len);
    }
    offset += dib_length;
  }
  return response;
}

}  // namespace cvknx::knxnetip
