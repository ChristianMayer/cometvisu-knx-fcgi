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
 * @file test_knxnetip_tunnel.cpp
 * @brief Tunnelling session against a scriptable loopback "router".
 *
 * The fake router is a plain UDP socket on 127.0.0.1 with an ephemeral port:
 * CI never needs port 3671, multicast or a real device.
 */

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include "../../src/knxnetip/cemi.h"
#include "../../src/knxnetip/knxnetip_frame.h"
#include "../../src/knxnetip/tunneling_bus_link.h"

namespace cvknx {
namespace {

using Bytes = std::vector<std::uint8_t>;
using knxnetip::Hpai;
using knxnetip::ServiceType;

constexpr std::uint8_t kChannel = 0x21;

/// Minimal scriptable KNXnet/IP device on loopback.
class FakeRouter {
public:
  FakeRouter() {
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
  ~FakeRouter() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }
  FakeRouter(const FakeRouter&) = delete;
  FakeRouter& operator=(const FakeRouter&) = delete;

  [[nodiscard]] std::uint16_t port() const { return port_; }

  bool receive(Bytes& out) {
    std::uint8_t buffer[2048] = {};
    struct sockaddr_in from {};
    socklen_t from_len = sizeof(from);
    const ssize_t received = ::recvfrom(fd_, buffer, sizeof(buffer), 0,
                                        reinterpret_cast<struct sockaddr*>(&from), &from_len);
    if (received <= 0) {
      return false;
    }
    peer_ = from;
    have_peer_ = true;
    out.assign(buffer, buffer + static_cast<std::ptrdiff_t>(received));
    return true;
  }

  void reply(const Bytes& data) {
    if (!have_peer_) {
      return;
    }
    (void)::sendto(fd_, data.data(), data.size(), 0,
                   reinterpret_cast<const struct sockaddr*>(&peer_), sizeof(peer_));
  }

private:
  int fd_ = -1;
  std::uint16_t port_ = 0;
  struct sockaddr_in peer_ {};
  bool have_peer_ = false;
};

Bytes ConnectResponse(std::uint8_t channel, std::uint8_t status, std::uint16_t crd) {
  Bytes body{channel, status};
  const auto hpai = knxnetip::encode_hpai(Hpai{0x7F000001U, 3671});
  body.insert(body.end(), hpai.begin(), hpai.end());
  body.push_back(0x04);
  body.push_back(0x04);
  body.push_back(static_cast<std::uint8_t>((crd >> 8) & 0xFF));
  body.push_back(static_cast<std::uint8_t>(crd & 0xFF));
  return knxnetip::build_frame(ServiceType::ConnectResponse, body);
}

Bytes ConnectionStateResponse(std::uint8_t channel, std::uint8_t status) {
  return knxnetip::build_frame(ServiceType::ConnectionStateResponse, {channel, status});
}

/// cEMI L_Data.ind for a group write.
Bytes IndicationCemi(std::uint16_t group, const Bytes& apdu) {
  Bytes cemi = cemi::encode_group_request(0x1101, group, apdu);
  cemi[0] = cemi::kMessageCodeLDataInd;
  return cemi;
}

TunnelOptions LoopbackOptions(std::uint16_t port) {
  TunnelOptions options;
  options.host = "127.0.0.1";
  options.port = port;
  options.heartbeat_sec = 60;
  options.ack_timeout_ms = 200;
  options.retries = 3;
  options.connect_timeout_ms = 1000;
  return options;
}

/// Run connect() concurrently with the router side.
bool ConnectWithRouter(TunnelingBusLink& link, FakeRouter& router, std::uint8_t channel = kChannel,
                       std::uint8_t status = 0x00) {
  bool connected = false;
  std::thread connect_thread([&link, &connected] { connected = link.connect(); });
  Bytes request;
  if (router.receive(request)) {
    router.reply(ConnectResponse(channel, status, 0x1101));
  }
  connect_thread.join();
  return connected;
}

TEST(TunnelingBusLinkTest, ConnectSendsConnectRequestAndParsesChannel) {
  FakeRouter router;
  TunnelingBusLink link(LoopbackOptions(router.port()));

  bool connected = false;
  std::thread connect_thread([&link, &connected] { connected = link.connect(); });

  Bytes request;
  ASSERT_TRUE(router.receive(request));
  EXPECT_EQ(request.size(), 25U);
  const auto frame = knxnetip::parse_frame(request);
  ASSERT_TRUE(frame.has_value());
  EXPECT_EQ(frame->service, ServiceType::ConnectRequest);
  EXPECT_EQ(request[22], 0x04);  // connection type tunnelling
  EXPECT_EQ(request[23], 0x02);  // KNX layer: link layer
  EXPECT_EQ(request[24], 0x00);  // reserved

  router.reply(ConnectResponse(kChannel, 0x00, 0x1101));
  connect_thread.join();

  EXPECT_TRUE(connected);
  EXPECT_EQ(link.channel(), kChannel);
  EXPECT_TRUE(link.is_connected());
}

TEST(TunnelingBusLinkTest, ConnectMapsNoMoreConnectionsToError) {
  FakeRouter router;
  TunnelingBusLink link(LoopbackOptions(router.port()));

  EXPECT_FALSE(ConnectWithRouter(
      link, router, 0x00, static_cast<std::uint8_t>(knxnetip::ConnectStatus::NoMoreConnections)));
  EXPECT_FALSE(link.is_connected());
}

TEST(TunnelingBusLinkTest, ConnectTimesOutAfterDeadline) {
  FakeRouter router;
  TunnelOptions options = LoopbackOptions(router.port());
  options.connect_timeout_ms = 100;
  TunnelingBusLink link(options);

  const auto start = std::chrono::steady_clock::now();
  EXPECT_FALSE(link.connect());  // nobody answers
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();
  EXPECT_LT(elapsed, 1000) << "connect() must not hang without a CONNECT_RESPONSE";
}

TEST(TunnelingBusLinkTest, SendIncrementsSequenceAndWaitsForAck) {
  FakeRouter router;
  TunnelingBusLink link(LoopbackOptions(router.port()));
  ASSERT_TRUE(ConnectWithRouter(link, router));

  bool sent = false;
  std::thread sender([&link, &sent] { sent = link.send_group_packet(0x0A03, {0x00, 0x80, 0x42}); });

  Bytes request;
  ASSERT_TRUE(router.receive(request));
  const auto parsed = knxnetip::parse_tunnelling_request(request);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->channel, kChannel);
  EXPECT_EQ(parsed->sequence, 0);
  // L_Data.req carrying the CRD address as source and the group as destination.
  const auto expected_cemi = cemi::encode_group_request(0x1101, 0x0A03, {0x00, 0x80, 0x42});
  EXPECT_EQ(parsed->cemi, expected_cemi);

  router.reply(knxnetip::build_tunnelling_ack(kChannel, 0));
  sender.join();
  if (::testing::Test::HasFatalFailure()) {
    return;
  }

  EXPECT_TRUE(sent);
  EXPECT_EQ(link.next_sequence(), 1);
}

TEST(TunnelingBusLinkTest, RetransmitsSameSequenceOnMissingAck) {
  FakeRouter router;
  TunnelOptions options = LoopbackOptions(router.port());
  options.ack_timeout_ms = 40;
  options.retries = 2;
  TunnelingBusLink link(options);
  ASSERT_TRUE(ConnectWithRouter(link, router));

  bool sent = true;
  std::thread sender([&link, &sent] { sent = link.send_group_packet(0x0A03, {0x00, 0x80, 0x42}); });

  for (int attempt = 0; attempt <= 2; ++attempt) {
    Bytes request;
    ASSERT_TRUE(router.receive(request)) << "attempt " << attempt;
    const auto parsed = knxnetip::parse_tunnelling_request(request);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->sequence, 0) << "retransmissions must reuse the sequence number";
  }
  sender.join();

  EXPECT_FALSE(sent);
}

TEST(TunnelingBusLinkTest, AcksAndDiscardsDuplicateInbound) {
  FakeRouter router;
  TunnelingBusLink link(LoopbackOptions(router.port()));
  ASSERT_TRUE(ConnectWithRouter(link, router));

  const auto cemi_bytes = IndicationCemi(0x0A03, {0x00, 0x80, 0x42});
  router.reply(knxnetip::build_tunnelling_request(kChannel, 0, cemi_bytes));

  std::uint16_t addr = 0;
  Bytes apdu;
  ASSERT_TRUE(link.poll_group_telegram(addr, apdu));
  EXPECT_EQ(addr, 0x0A03);

  Bytes ack;
  ASSERT_TRUE(router.receive(ack));
  const auto parsed_ack = knxnetip::parse_tunnelling_ack(ack);
  ASSERT_TRUE(parsed_ack.has_value());
  EXPECT_EQ(parsed_ack->sequence, 0);

  // The very same frame again: ACKed, but delivered only once.
  router.reply(knxnetip::build_tunnelling_request(kChannel, 0, cemi_bytes));
  EXPECT_FALSE(link.poll_group_telegram(addr, apdu));
  ASSERT_TRUE(router.receive(ack));
  EXPECT_EQ(link.get_telegram_count(), 1U);
}

TEST(TunnelingBusLinkTest, AcksOutOfOrderInboundAndDiscards) {
  FakeRouter router;
  TunnelingBusLink link(LoopbackOptions(router.port()));
  ASSERT_TRUE(ConnectWithRouter(link, router));

  const auto cemi_bytes = IndicationCemi(0x0A03, {0x00, 0x80, 0x42});
  router.reply(knxnetip::build_tunnelling_request(kChannel, 0, cemi_bytes));
  std::uint16_t addr = 0;
  Bytes apdu;
  ASSERT_TRUE(link.poll_group_telegram(addr, apdu));
  Bytes ack;
  ASSERT_TRUE(router.receive(ack));

  // Expected 1, got 5 → discarded (but ACKed).
  router.reply(knxnetip::build_tunnelling_request(kChannel, 5, cemi_bytes));
  EXPECT_FALSE(link.poll_group_telegram(addr, apdu));
  ASSERT_TRUE(router.receive(ack));
  const auto parsed_ack = knxnetip::parse_tunnelling_ack(ack);
  ASSERT_TRUE(parsed_ack.has_value());
  EXPECT_EQ(parsed_ack->sequence, 5);
  EXPECT_EQ(link.get_telegram_count(), 1U);
}

TEST(TunnelingBusLinkTest, DropsLDataCon) {
  FakeRouter router;
  TunnelingBusLink link(LoopbackOptions(router.port()));
  ASSERT_TRUE(ConnectWithRouter(link, router));

  Bytes cemi_bytes = IndicationCemi(0x0A03, {0x00, 0x80, 0x42});
  cemi_bytes[0] = cemi::kMessageCodeLDataCon;  // our own write confirmation
  router.reply(knxnetip::build_tunnelling_request(kChannel, 0, cemi_bytes));

  std::uint16_t addr = 0;
  Bytes apdu;
  EXPECT_FALSE(link.poll_group_telegram(addr, apdu));
  EXPECT_EQ(link.get_telegram_count(), 0U);

  // It was still ACKed, otherwise the device would retransmit forever.
  Bytes ack;
  ASSERT_TRUE(router.receive(ack));
}

TEST(TunnelingBusLinkTest, SequenceWraps255To0) {
  FakeRouter router;
  TunnelingBusLink link(LoopbackOptions(router.port()));
  ASSERT_TRUE(ConnectWithRouter(link, router));

  std::thread acknowledger([&router] {
    for (int i = 0; i < 256; ++i) {
      Bytes request;
      if (!router.receive(request)) {
        return;
      }
      const auto parsed = knxnetip::parse_tunnelling_request(request);
      if (!parsed) {
        return;
      }
      router.reply(knxnetip::build_tunnelling_ack(parsed->channel, parsed->sequence));
    }
  });

  for (int i = 0; i < 256; ++i) {
    ASSERT_TRUE(link.send_group_packet(0x0A03, {0x00, 0x80, 0x01})) << "send " << i;
  }
  acknowledger.join();
  EXPECT_EQ(link.next_sequence(), 0) << "the sequence counter must wrap 255 -> 0";
}

TEST(TunnelingBusLinkTest, SendsHeartbeatAfterInterval) {
  FakeRouter router;
  TunnelOptions options = LoopbackOptions(router.port());
  options.heartbeat_sec = 1;
  TunnelingBusLink link(options);
  ASSERT_TRUE(ConnectWithRouter(link, router));

  // The wait must not block past the heartbeat deadline.
  EXPECT_EQ(link.wait_for_activity(1500), BusLink::WaitResult::Timeout);

  Bytes request;
  ASSERT_TRUE(router.receive(request));
  const auto frame = knxnetip::parse_frame(request);
  ASSERT_TRUE(frame.has_value());
  EXPECT_EQ(frame->service, ServiceType::ConnectionStateRequest);
}

TEST(TunnelingBusLinkTest, ReconnectsAfterConnectionStateError) {
  FakeRouter router;
  TunnelingBusLink link(LoopbackOptions(router.port()));
  ASSERT_TRUE(ConnectWithRouter(link, router));

  router.reply(ConnectionStateResponse(kChannel, 0x04));  // unknown channel id
  EXPECT_EQ(link.wait_for_activity(200), BusLink::WaitResult::Error);
  EXPECT_FALSE(link.is_connected());

  // A fresh session works again.
  ASSERT_TRUE(ConnectWithRouter(link, router));
  EXPECT_TRUE(link.is_connected());
}

TEST(TunnelingBusLinkTest, DisconnectSendsDisconnectRequest) {
  FakeRouter router;
  TunnelingBusLink link(LoopbackOptions(router.port()));
  ASSERT_TRUE(ConnectWithRouter(link, router));

  link.disconnect();
  EXPECT_FALSE(link.is_connected());

  Bytes request;
  ASSERT_TRUE(router.receive(request));
  const auto frame = knxnetip::parse_frame(request);
  ASSERT_TRUE(frame.has_value());
  EXPECT_EQ(frame->service, ServiceType::DisconnectRequest);
}

TEST(TunnelingBusLinkTest, NoBusyLoopWhenSocketSilent) {
  FakeRouter router;
  TunnelingBusLink link(LoopbackOptions(router.port()));
  ASSERT_TRUE(ConnectWithRouter(link, router));

  const auto start = std::chrono::steady_clock::now();
  const auto result = link.wait_for_activity(150);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();

  EXPECT_EQ(result, BusLink::WaitResult::Timeout);
  EXPECT_GE(elapsed, 100);
  EXPECT_LT(elapsed, 1000);
}

TEST(TunnelingBusLinkTest, LabelNamesTheDevice) {
  TunnelingBusLink link(LoopbackOptions(3671));
  EXPECT_NE(link.label().find("127.0.0.1:3671"), std::string::npos);
}

// ---- Error and boundary paths ----

TEST(TunnelingBusLinkTest, ConnectRejectsInvalidHost) {
  TunnelOptions options = LoopbackOptions(3671);
  options.host = "not-an-ip";
  TunnelingBusLink link(options);
  EXPECT_FALSE(link.connect());
}

TEST(TunnelingBusLinkTest, ConnectRejectsInvalidLocalAddress) {
  TunnelOptions options = LoopbackOptions(3671);
  options.local_addr = "not-an-ip";
  TunnelingBusLink link(options);
  EXPECT_FALSE(link.connect());
}

TEST(TunnelingBusLinkTest, SendFailsWithoutConnection) {
  TunnelingBusLink link(LoopbackOptions(3671));
  EXPECT_FALSE(link.send_group_packet(0x0A03, {0x00, 0x80, 0x42}));
  EXPECT_EQ(link.wait_for_activity(1), BusLink::WaitResult::Error);
  link.set_nonblocking(true);  // no socket — must be a no-op
}

TEST(TunnelingBusLinkTest, SendRejectsEmptyApdu) {
  FakeRouter router;
  TunnelingBusLink link(LoopbackOptions(router.port()));
  ASSERT_TRUE(ConnectWithRouter(link, router));
  EXPECT_FALSE(link.send_group_packet(0x0A03, {}));
}

TEST(TunnelingBusLinkTest, IgnoresUnparsableDatagrams) {
  FakeRouter router;
  TunnelingBusLink link(LoopbackOptions(router.port()));
  ASSERT_TRUE(ConnectWithRouter(link, router));

  router.reply(Bytes{0x01, 0x02, 0x03});
  EXPECT_EQ(link.wait_for_activity(100), BusLink::WaitResult::Timeout);
  std::uint16_t addr = 0;
  Bytes apdu;
  EXPECT_FALSE(link.poll_group_telegram(addr, apdu));
}

TEST(TunnelingBusLinkTest, IgnoresForeignAcknowledgement) {
  FakeRouter router;
  TunnelingBusLink link(LoopbackOptions(router.port()));
  ASSERT_TRUE(ConnectWithRouter(link, router));

  router.reply(knxnetip::build_tunnelling_ack(0x99, 7));  // other channel
  EXPECT_EQ(link.wait_for_activity(100), BusLink::WaitResult::Timeout);

  router.reply(knxnetip::build_tunnelling_ack(kChannel, 7));  // wrong sequence
  EXPECT_EQ(link.wait_for_activity(100), BusLink::WaitResult::Timeout);
}

TEST(TunnelingBusLinkTest, IgnoresConnectionStateResponseForAnotherChannel) {
  FakeRouter router;
  TunnelingBusLink link(LoopbackOptions(router.port()));
  ASSERT_TRUE(ConnectWithRouter(link, router));

  router.reply(ConnectionStateResponse(0x99, 0x04));
  EXPECT_EQ(link.wait_for_activity(100), BusLink::WaitResult::Timeout);
  EXPECT_TRUE(link.is_connected());
}

TEST(TunnelingBusLinkTest, DeviceDisconnectMakesTheTunnelLost) {
  FakeRouter router;
  TunnelingBusLink link(LoopbackOptions(router.port()));
  ASSERT_TRUE(ConnectWithRouter(link, router));

  router.reply(knxnetip::build_disconnect_request(kChannel, Hpai{0U, 0U}));
  EXPECT_EQ(link.wait_for_activity(100), BusLink::WaitResult::Error);
  EXPECT_FALSE(link.is_connected());
}

TEST(TunnelingBusLinkTest, IgnoresTunnellingRequestForAnotherChannel) {
  FakeRouter router;
  TunnelingBusLink link(LoopbackOptions(router.port()));
  ASSERT_TRUE(ConnectWithRouter(link, router));

  const auto cemi_bytes = IndicationCemi(0x0A03, {0x00, 0x80, 0x42});
  router.reply(knxnetip::build_tunnelling_request(0x99, 0, cemi_bytes));

  std::uint16_t addr = 0;
  Bytes apdu;
  EXPECT_FALSE(link.poll_group_telegram(addr, apdu));
  EXPECT_EQ(link.get_telegram_count(), 0U);
}

TEST(TunnelingBusLinkTest, DestructorWithoutConnectIsSafe) {
  { TunnelingBusLink link(LoopbackOptions(3671)); }
  SUCCEED();
}

TEST(TunnelingBusLinkTest, IgnoresNonConnectDatagramsWhileConnecting) {
  FakeRouter router;
  TunnelingBusLink link(LoopbackOptions(router.port()));

  bool connected = false;
  std::thread connect_thread([&link, &connected] { connected = link.connect(); });
  Bytes request;
  ASSERT_TRUE(router.receive(request));

  // A stray datagram before the CONNECT_RESPONSE must be ignored, not fatal.
  router.reply(Bytes{0x06, 0x10, 0x03, 0x30, 0x00, 0x06});
  router.reply(ConnectResponse(kChannel, 0x00, 0x1101));
  connect_thread.join();

  EXPECT_TRUE(connected);
}

TEST(TunnelingBusLinkTest, IgnoresUnhandledServiceTypes) {
  FakeRouter router;
  TunnelingBusLink link(LoopbackOptions(router.port()));
  ASSERT_TRUE(ConnectWithRouter(link, router));

  router.reply(knxnetip::build_search_request(Hpai{0U, 0U}));
  EXPECT_EQ(link.wait_for_activity(100), BusLink::WaitResult::Timeout);
}

}  // namespace
}  // namespace cvknx
