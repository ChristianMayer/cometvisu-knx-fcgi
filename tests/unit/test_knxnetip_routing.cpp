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
 * @file test_knxnetip_routing.cpp
 * @brief IP routing link against a loopback UDP peer (never port 3671, never
 *        multicast — CI must not depend on it).
 */

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <vector>

#include "../../src/knxnetip/cemi.h"
#include "../../src/knxnetip/knxnetip_frame.h"
#include "../../src/knxnetip/routing_bus_link.h"

namespace cvknx {
namespace {

using Bytes = std::vector<std::uint8_t>;

/// A loopback UDP endpoint that plays the role of the KNX router.
class UdpPeer {
public:
  UdpPeer() {
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(fd_, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) != 0) {
      return;
    }
    socklen_t len = sizeof(address);
    (void)::getsockname(fd_, reinterpret_cast<struct sockaddr*>(&address), &len);
    port_ = ntohs(address.sin_port);
    struct timeval timeout {
      2, 0
    };
    (void)::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  }
  ~UdpPeer() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }
  UdpPeer(const UdpPeer&) = delete;
  UdpPeer& operator=(const UdpPeer&) = delete;

  [[nodiscard]] std::uint16_t port() const { return port_; }

  Bytes receive() {
    std::uint8_t buffer[2048] = {};
    const ssize_t received = ::recv(fd_, buffer, sizeof(buffer), 0);
    if (received <= 0) {
      return {};
    }
    return Bytes(buffer, buffer + received);
  }

  void send_to(std::uint16_t port, const Bytes& data) {
    struct sockaddr_in destination {};
    destination.sin_family = AF_INET;
    destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    destination.sin_port = htons(port);
    (void)::sendto(fd_, data.data(), data.size(), 0,
                   reinterpret_cast<const struct sockaddr*>(&destination), sizeof(destination));
  }

private:
  int fd_ = -1;
  std::uint16_t port_ = 0;
};

RoutingOptions LoopbackOptions(std::uint16_t destination_port) {
  RoutingOptions options;
  options.address = "127.0.0.1";
  options.port = destination_port;
  options.bind_port = 0;  // ephemeral — the peer owns the destination port
  return options;
}

Bytes Indication(uint16_t group_addr, const Bytes& apdu, std::uint8_t message_code) {
  Bytes cemi = cemi::encode_group_request(0x1101, group_addr, apdu);
  EXPECT_FALSE(cemi.empty());
  cemi[0] = message_code;
  return knxnetip::build_routing_indication(cemi);
}

TEST(RoutingBusLinkTest, SendsRoutingIndicationWithCemi) {
  UdpPeer peer;
  RoutingBusLink link(LoopbackOptions(peer.port()));
  ASSERT_TRUE(link.connect());

  ASSERT_TRUE(link.send_group_packet(0x0A03, {0x00, 0x80, 0x42}));

  const auto received = peer.receive();
  const Bytes expected = knxnetip::build_routing_indication(
      cemi::encode_group_request(0x0000, 0x0A03, {0x00, 0x80, 0x42}));
  EXPECT_EQ(received, expected);
}

TEST(RoutingBusLinkTest, ReceivesRoutingIndication) {
  UdpPeer peer;
  RoutingBusLink link(LoopbackOptions(peer.port()));
  ASSERT_TRUE(link.connect());
  ASSERT_NE(link.local_port(), 0);

  peer.send_to(link.local_port(), Indication(0x0A03, {0x00, 0x80, 0x0C, 0x6F}, 0x29));

  std::uint16_t addr = 0;
  Bytes apdu;
  ASSERT_TRUE(link.poll_group_telegram(addr, apdu));
  EXPECT_EQ(addr, 0x0A03);
  EXPECT_EQ(apdu, (Bytes{0x00, 0x80, 0x0C, 0x6F}));
  EXPECT_EQ(link.get_telegram_count(), 1U);
}

TEST(RoutingBusLinkTest, IgnoresOwnEcho) {
  // Our own frame comes back as L_Data.req (0x11) with multicast loop enabled;
  // it must never be delivered a second time.
  UdpPeer peer;
  RoutingBusLink link(LoopbackOptions(peer.port()));
  ASSERT_TRUE(link.connect());

  peer.send_to(link.local_port(), Indication(0x0A03, {0x00, 0x80, 0x42}, 0x11));

  std::uint16_t addr = 0;
  Bytes apdu;
  EXPECT_FALSE(link.poll_group_telegram(addr, apdu));
}

TEST(RoutingBusLinkTest, IgnoresConfirmations) {
  UdpPeer peer;
  RoutingBusLink link(LoopbackOptions(peer.port()));
  ASSERT_TRUE(link.connect());

  peer.send_to(link.local_port(), Indication(0x0A03, {0x00, 0x80, 0x42}, 0x2E));

  std::uint16_t addr = 0;
  Bytes apdu;
  EXPECT_FALSE(link.poll_group_telegram(addr, apdu));
}

TEST(RoutingBusLinkTest, IgnoresOtherServiceTypes) {
  UdpPeer peer;
  RoutingBusLink link(LoopbackOptions(peer.port()));
  ASSERT_TRUE(link.connect());

  const Bytes search = knxnetip::build_search_request(knxnetip::Hpai{});
  peer.send_to(link.local_port(), search);

  std::uint16_t addr = 0;
  Bytes apdu;
  EXPECT_FALSE(link.poll_group_telegram(addr, apdu));
}

TEST(RoutingBusLinkTest, SendFailsWhenNotConnected) {
  RoutingBusLink link(LoopbackOptions(3671));
  EXPECT_FALSE(link.send_group_packet(0x0A03, {0x00, 0x80, 0x42}));
  EXPECT_EQ(link.wait_for_activity(1), BusLink::WaitResult::Error);

  ASSERT_TRUE(link.connect());
  link.disconnect();
  EXPECT_FALSE(link.send_group_packet(0x0A03, {0x00, 0x80, 0x42}));
}

TEST(RoutingBusLinkTest, EmptyApduRejected) {
  UdpPeer peer;
  RoutingBusLink link(LoopbackOptions(peer.port()));
  ASSERT_TRUE(link.connect());
  EXPECT_FALSE(link.send_group_packet(0x0A03, {}));
}

TEST(RoutingBusLinkTest, DoesNotSpinWhenSocketSilent) {
  UdpPeer peer;
  RoutingBusLink link(LoopbackOptions(peer.port()));
  ASSERT_TRUE(link.connect());

  const auto start = std::chrono::steady_clock::now();
  const auto result = link.wait_for_activity(200);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();

  EXPECT_EQ(result, BusLink::WaitResult::Timeout);
  EXPECT_GE(elapsed, 150);
  EXPECT_LT(elapsed, 1000);
}

TEST(RoutingBusLinkTest, LabelNamesTheGroup) {
  RoutingOptions options;
  options.address = "224.0.23.12";
  options.port = 3671;
  RoutingBusLink link(options);
  EXPECT_NE(link.label().find("224.0.23.12:3671"), std::string::npos);
}

}  // namespace
}  // namespace cvknx
