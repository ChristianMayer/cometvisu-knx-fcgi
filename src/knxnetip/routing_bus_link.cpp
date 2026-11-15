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
 * @file routing_bus_link.cpp
 * @brief KNXnet/IP IP routing transport implementation.
 */

#include "knxnetip/routing_bus_link.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <vector>

#include "knxnetip/cemi.h"
#include "knxnetip/knxnetip_frame.h"

namespace cvknx {
namespace {

constexpr std::uint8_t kMulticastTtl = 16;
constexpr int kReceiveBufferBytes = 2048;

}  // namespace

RoutingOptions RoutingOptions::from_config(const BusConfig& config) {
  RoutingOptions options;
  options.address = config.ip_multicast;
  options.port = config.ip_port;
  options.iface = config.ip_iface;
  options.bind_port = config.ip_port;
  return options;
}

RoutingBusLink::RoutingBusLink(const RoutingOptions& options) : options_(options) {}

RoutingBusLink::~RoutingBusLink() {
  disconnect();
}

bool RoutingBusLink::connect() {
  disconnect();

  const auto address = knxnetip::ipv4_from_string(options_.address);
  if (!address) {
    std::cerr << "[ERROR] Invalid KNX_IP_MULTICAST '" << options_.address << "'\n";
    return false;
  }

  fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd_ < 0) {
    std::cerr << "[ERROR] Cannot create KNXnet/IP routing socket: " << std::strerror(errno) << "\n";
    return false;
  }

  int enable = 1;
  (void)::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));

  struct sockaddr_in local {};
  local.sin_family = AF_INET;
  local.sin_addr.s_addr = htonl(INADDR_ANY);
  local.sin_port = htons(options_.bind_port);
  if (::bind(fd_, reinterpret_cast<struct sockaddr*>(&local), sizeof(local)) != 0) {
    std::cerr << "[ERROR] Cannot bind KNXnet/IP routing socket to port " << options_.bind_port
              << ": " << std::strerror(errno) << "\n";
    disconnect();
    return false;
  }

  struct sockaddr_in bound {};
  socklen_t bound_len = sizeof(bound);
  if (::getsockname(fd_, reinterpret_cast<struct sockaddr*>(&bound), &bound_len) == 0) {
    local_port_ = ntohs(bound.sin_port);
  }

  // Join the group only for a real multicast address.  A unicast address is
  // the documented loopback/test mode (no join is possible there).
  const std::uint32_t host_address = *address;
  if (IN_MULTICAST(ntohl(host_address))) {
    struct ip_mreqn membership {};
    membership.imr_multiaddr.s_addr = htonl(host_address);
    membership.imr_address.s_addr = htonl(INADDR_ANY);
    if (!options_.iface.empty()) {
      const unsigned index = ::if_nametoindex(options_.iface.c_str());
      if (index == 0) {
        std::cerr << "[ERROR] Unknown KNX_IP_IFACE '" << options_.iface << "'\n";
        disconnect();
        return false;
      }
      membership.imr_ifindex = static_cast<int>(index);
    }
    if (::setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership, sizeof(membership)) != 0) {
      std::cerr << "[ERROR] Cannot join KNXnet/IP multicast group " << options_.address << ": "
                << std::strerror(errno) << "\n";
      disconnect();
      return false;
    }
  }

  const int interface_index =
      options_.iface.empty() ? 0 : static_cast<int>(::if_nametoindex(options_.iface.c_str()));
  if (interface_index != 0) {
    struct ip_mreqn iface_request {};
    iface_request.imr_ifindex = interface_index;
    (void)::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_IF, &iface_request, sizeof(iface_request));
  }

  unsigned char ttl = kMulticastTtl;
  (void)::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
  // Loop enabled: a local router (or knxd in ip: mode) must see our frames and
  // the interop smoke test relies on it.  Echoes are dropped by the codec.
  unsigned char loop = 1;
  (void)::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

  // Receive loops call recv() until EAGAIN — never block there.
  set_nonblocking(true);
  return true;
}

void RoutingBusLink::disconnect() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  local_port_ = 0;
}

bool RoutingBusLink::send_group_packet(std::uint16_t group_addr,
                                       const std::vector<std::uint8_t>& apdu) {
  if (fd_ < 0) {
    return false;
  }
  const auto address = knxnetip::ipv4_from_string(options_.address);
  if (!address) {
    return false;
  }

  const auto cemi = cemi::encode_group_request(0x0000, group_addr, apdu);
  if (cemi.empty()) {
    return false;
  }
  const auto datagram = knxnetip::build_routing_indication(cemi);

  struct sockaddr_in destination {};
  destination.sin_family = AF_INET;
  destination.sin_addr.s_addr = htonl(*address);
  destination.sin_port = htons(options_.port);

  const ssize_t sent =
      ::sendto(fd_, datagram.data(), datagram.size(), 0,
               reinterpret_cast<const struct sockaddr*>(&destination), sizeof(destination));
  if (sent != static_cast<ssize_t>(datagram.size())) {
    // Routing is fire-and-forget, but a broken socket (ENETUNREACH, ENODEV)
    // must surface as a failed write so /w answers 503.
    std::cerr << "[WARN] KNXnet/IP routing send failed: " << std::strerror(errno) << "\n";
    return false;
  }
  return true;
}

bool RoutingBusLink::poll_group_telegram(std::uint16_t& out_group_addr,
                                         std::vector<std::uint8_t>& out_apdu) {
  if (fd_ < 0) {
    return false;
  }

  std::uint8_t buffer[kReceiveBufferBytes];  // NOLINT(modernize-avoid-c-arrays)
  for (;;) {
    const ssize_t received = ::recv(fd_, buffer, sizeof(buffer), 0);
    if (received <= 0) {
      return false;  // EAGAIN or error — nothing (more) to read
    }
    const std::vector<std::uint8_t> datagram(buffer,
                                             buffer + static_cast<std::ptrdiff_t>(received));
    const auto cemi_payload = knxnetip::parse_routing_indication(datagram);
    if (!cemi_payload) {
      continue;  // other service types are ignored
    }
    const auto frame = cemi::parse_ldata_ind(*cemi_payload);
    if (!frame) {
      continue;  // L_Data.req (our own echo), L_Data.con, individual traffic
    }
    out_group_addr = frame->destination;
    out_apdu = frame->apdu;
    ++telegram_count_;
    return true;
  }
}

void RoutingBusLink::set_nonblocking(bool enable) {
  if (fd_ < 0) {
    return;
  }
  const int flags = ::fcntl(fd_, F_GETFL, 0);
  if (flags < 0) {
    return;
  }
  ::fcntl(fd_, F_SETFL, enable ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
}

BusLink::WaitResult RoutingBusLink::wait_for_activity(int timeout_ms) {
  if (fd_ < 0) {
    return WaitResult::Error;
  }
  struct pollfd pfd {};
  pfd.fd = fd_;
  pfd.events = POLLIN;
  const int rc = ::poll(&pfd, 1, timeout_ms);
  if (rc == 0) {
    return WaitResult::Timeout;
  }
  if (rc < 0) {
    return errno == EINTR ? WaitResult::Timeout : WaitResult::Error;
  }
  if ((pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
    return WaitResult::Error;
  }
  if ((pfd.revents & POLLIN) != 0) {
    return WaitResult::GroupData;
  }
  return WaitResult::Timeout;
}

std::string RoutingBusLink::label() const {
  return "KNXnet/IP routing " + options_.address + ":" + std::to_string(options_.port);
}

}  // namespace cvknx
