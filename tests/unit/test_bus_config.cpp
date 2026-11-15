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
 * @file test_bus_config.cpp
 * @brief KNX_TRANSPORT is mandatory and every value is validated.
 */

#include <gtest/gtest.h>

#include <map>
#include <string>

#include "../../src/bus/bus_config.h"

namespace cvknx {
namespace {

/// Parse using a fixed environment map (no process-global environment).
std::optional<BusConfig> parse_env(const std::map<std::string, std::string>& env,
                                   std::string& error) {
  const BusConfig::EnvGetter get = [&env](const char* name) -> std::string {
    const auto it = env.find(name);
    return it != env.end() ? it->second : std::string{};
  };
  return BusConfig::parse(get, error);
}

TEST(BusConfigTest, TransportIsRequired) {
  std::string error;
  const auto cfg = parse_env({}, error);
  ASSERT_FALSE(cfg.has_value());
  EXPECT_NE(error.find("KNX_TRANSPORT is required"), std::string::npos) << error;
}

TEST(BusConfigTest, UnknownTransportIsRejected) {
  std::string error;
  const auto cfg = parse_env({{"KNX_TRANSPORT", "eibd"}}, error);
  ASSERT_FALSE(cfg.has_value());
  EXPECT_NE(error.find("unknown KNX_TRANSPORT 'eibd'"), std::string::npos) << error;
}

TEST(BusConfigTest, KnxdKeepsCurrentDefaults) {
  std::string error;
  const auto cfg = parse_env({{"KNX_TRANSPORT", "knxd"}}, error);
  ASSERT_TRUE(cfg.has_value()) << error;
  EXPECT_EQ(cfg->transport, BusTransport::Knxd);
  EXPECT_EQ(cfg->knxd_socket, "/run/knx");
  EXPECT_EQ(cfg->ip_port, 3671);
}

TEST(BusConfigTest, KnxdSocketCanBeOverridden) {
  std::string error;
  const auto cfg = parse_env({{"KNX_TRANSPORT", "knxd"}, {"KNXD_SOCKET", "/tmp/knxd"}}, error);
  ASSERT_TRUE(cfg.has_value()) << error;
  EXPECT_EQ(cfg->knxd_socket, "/tmp/knxd");
}

TEST(BusConfigTest, IpRoutingNeedsNoHost) {
  std::string error;
  const auto cfg = parse_env({{"KNX_TRANSPORT", "ip-routing"}}, error);
  ASSERT_TRUE(cfg.has_value()) << error;
  EXPECT_EQ(cfg->transport, BusTransport::IpRouting);
  EXPECT_EQ(cfg->ip_multicast, "224.0.23.12");
}

TEST(BusConfigTest, IpRoutingParsesMulticastAndInterface) {
  std::string error;
  const auto cfg = parse_env({{"KNX_TRANSPORT", "ip-routing"},
                              {"KNX_IP_MULTICAST", "239.0.0.1"},
                              {"KNX_IP_IFACE", "eth0"},
                              {"KNX_IP_PORT", "3672"}},
                             error);
  ASSERT_TRUE(cfg.has_value()) << error;
  EXPECT_EQ(cfg->ip_multicast, "239.0.0.1");
  EXPECT_EQ(cfg->ip_iface, "eth0");
  EXPECT_EQ(cfg->ip_port, 3672);
}

TEST(BusConfigTest, IpTunnelRequiresHost) {
  std::string error;
  const auto cfg = parse_env({{"KNX_TRANSPORT", "ip-tunnel"}}, error);
  ASSERT_FALSE(cfg.has_value());
  EXPECT_NE(error.find("KNX_IP_HOST is required"), std::string::npos) << error;
}

TEST(BusConfigTest, IpTunnelParsesAllOptions) {
  std::string error;
  const auto cfg = parse_env({{"KNX_TRANSPORT", "ip-tunnel"},
                              {"KNX_IP_HOST", "192.168.1.10"},
                              {"KNX_IP_LOCAL_ADDR", "192.168.1.20"},
                              {"KNX_IP_NAT", "1"},
                              {"KNX_IP_HEARTBEAT_SEC", "30"},
                              {"KNX_IP_ACK_TIMEOUT_MS", "500"},
                              {"KNX_IP_RETRIES", "5"}},
                             error);
  ASSERT_TRUE(cfg.has_value()) << error;
  EXPECT_EQ(cfg->transport, BusTransport::IpTunnel);
  EXPECT_EQ(cfg->ip_host, "192.168.1.10");
  EXPECT_EQ(cfg->ip_local_addr, "192.168.1.20");
  EXPECT_TRUE(cfg->ip_nat);
  EXPECT_EQ(cfg->heartbeat_sec, 30);
  EXPECT_EQ(cfg->ack_timeout_ms, 500);
  EXPECT_EQ(cfg->retries, 5);
}

TEST(BusConfigTest, InvalidPortIsRejectedNotEmptyDefault) {
  std::string error;
  const auto cfg = parse_env({{"KNX_TRANSPORT", "knxd"}, {"KNX_IP_PORT", "70000"}}, error);
  ASSERT_FALSE(cfg.has_value());
  EXPECT_NE(error.find("KNX_IP_PORT"), std::string::npos) << error;
}

TEST(BusConfigTest, GarbagePortIsRejected) {
  std::string error;
  const auto cfg = parse_env({{"KNX_TRANSPORT", "knxd"}, {"KNX_IP_PORT", "36x1"}}, error);
  ASSERT_FALSE(cfg.has_value());
}

TEST(BusConfigTest, HeartbeatBelowTenIsRejected) {
  std::string error;
  const auto cfg = parse_env(
      {{"KNX_TRANSPORT", "ip-tunnel"}, {"KNX_IP_HOST", "10.0.0.1"}, {"KNX_IP_HEARTBEAT_SEC", "5"}},
      error);
  ASSERT_FALSE(cfg.has_value());
  EXPECT_NE(error.find("KNX_IP_HEARTBEAT_SEC"), std::string::npos) << error;
}

TEST(BusConfigTest, ZeroRetriesIsRejected) {
  std::string error;
  const auto cfg = parse_env({{"KNX_TRANSPORT", "knxd"}, {"KNX_IP_RETRIES", "0"}}, error);
  ASSERT_FALSE(cfg.has_value());
  EXPECT_NE(error.find("KNX_IP_RETRIES"), std::string::npos) << error;
}

TEST(BusConfigTest, InvalidNatFlagIsRejected) {
  std::string error;
  const auto cfg = parse_env(
      {{"KNX_TRANSPORT", "ip-tunnel"}, {"KNX_IP_HOST", "10.0.0.1"}, {"KNX_IP_NAT", "maybe"}},
      error);
  ASSERT_FALSE(cfg.has_value());
  EXPECT_NE(error.find("KNX_IP_NAT"), std::string::npos) << error;
}

TEST(BusConfigTest, OutageKeyIsTransportDependent) {
  std::string error;
  const auto knxd = parse_env({{"KNX_TRANSPORT", "knxd"}}, error);
  ASSERT_TRUE(knxd.has_value());
  EXPECT_EQ(knxd->outage_key(), "knxd");

  const auto routing = parse_env({{"KNX_TRANSPORT", "ip-routing"}}, error);
  ASSERT_TRUE(routing.has_value());
  EXPECT_EQ(routing->outage_key(), "bus");

  const auto tunnel =
      parse_env({{"KNX_TRANSPORT", "ip-tunnel"}, {"KNX_IP_HOST", "10.0.0.1"}}, error);
  ASSERT_TRUE(tunnel.has_value());
  EXPECT_EQ(tunnel->outage_key(), "bus");
}

TEST(BusConfigTest, DescribeNamesTheTransport) {
  std::string error;
  const auto cfg = parse_env({{"KNX_TRANSPORT", "knxd"}, {"KNXD_SOCKET", "/tmp/knxd"}}, error);
  ASSERT_TRUE(cfg.has_value());
  EXPECT_NE(cfg->describe().find("knxd"), std::string::npos);
  EXPECT_NE(cfg->describe().find("/tmp/knxd"), std::string::npos);
}

TEST(BusConfigTest, TransportNamesRoundTrip) {
  for (const char* name : {"knxd", "ip-tunnel", "ip-routing"}) {
    const auto parsed = bus_transport_from_string(name);
    ASSERT_TRUE(parsed.has_value()) << name;
    EXPECT_EQ(std::string{bus_transport_to_string(*parsed)}, name);
  }
  EXPECT_FALSE(bus_transport_from_string("IP-TUNNEL").has_value());
  EXPECT_FALSE(bus_transport_from_string("").has_value());
}

}  // namespace
}  // namespace cvknx
