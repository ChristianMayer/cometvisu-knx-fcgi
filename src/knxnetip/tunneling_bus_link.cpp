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
 * @file tunneling_bus_link.cpp
 * @brief KNXnet/IP tunnelling session state machine.
 */

#include "knxnetip/tunneling_bus_link.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <optional>

#include "knxnetip/cemi.h"
#include "knxnetip/knxnetip_frame.h"

namespace cvknx {
namespace {

constexpr int kReceiveBufferBytes = 2048;

/// Individual address as A.L.D (source address shown in log lines).
std::string individual_address_text(std::uint16_t address) {
  return std::to_string((address >> 12) & 0x0F) + "." + std::to_string((address >> 8) & 0x0F) +
         "." + std::to_string(address & 0xFF);
}

std::int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace

TunnelOptions TunnelOptions::from_config(const BusConfig& config) {
  TunnelOptions options;
  options.host = config.ip_host;
  options.port = config.ip_port;
  options.local_addr = config.ip_local_addr;
  options.nat = config.ip_nat;
  options.heartbeat_sec = config.heartbeat_sec;
  options.ack_timeout_ms = config.ack_timeout_ms;
  options.retries = config.retries;
  return options;
}  // GCOV_EXCL_LINE - closing brace, no statement

TunnelingBusLink::TunnelingBusLink(const TunnelOptions& options) : options_(options) {}

TunnelingBusLink::~TunnelingBusLink() {
  disconnect();
}

bool TunnelingBusLink::send_to_device(const std::vector<std::uint8_t>& datagram) {
  if (fd_ < 0) {   // GCOV_EXCL_BR_LINE - disconnect() only calls this when connected
    return false;  // GCOV_EXCL_LINE
  }
  const auto host = knxnetip::ipv4_from_string(options_.host);
  if (!host) {     // GCOV_EXCL_BR_LINE - connect() validated the host
    return false;  // GCOV_EXCL_LINE
  }
  struct sockaddr_in destination {};
  destination.sin_family = AF_INET;
  destination.sin_addr.s_addr = htonl(*host);
  destination.sin_port = htons(options_.port);
  return ::sendto(fd_, datagram.data(), datagram.size(), 0,
                  reinterpret_cast<const struct sockaddr*>(&destination),
                  sizeof(destination)) == static_cast<ssize_t>(datagram.size());
}

bool TunnelingBusLink::connect() {
  disconnect();

  const auto host = knxnetip::ipv4_from_string(options_.host);
  if (!host) {
    std::cerr << "[ERROR] Invalid KNX_IP_HOST '" << options_.host << "'\n";
    return false;
  }

  fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd_ < 0) {   // GCOV_EXCL_BR_LINE - only fails on fd exhaustion
    return false;  // GCOV_EXCL_LINE
  }

  struct sockaddr_in local {};
  local.sin_family = AF_INET;
  const auto local_address = options_.local_addr.empty()
                                 ? std::optional<std::uint32_t>{0U}
                                 : knxnetip::ipv4_from_string(options_.local_addr);
  if (!local_address) {
    std::cerr << "[ERROR] Invalid KNX_IP_LOCAL_ADDR '" << options_.local_addr << "'\n";
    disconnect();
    return false;
  }
  local.sin_addr.s_addr = htonl(*local_address);
  local.sin_port = htons(options_.local_port);
  if (::bind(fd_, reinterpret_cast<struct sockaddr*>(&local), sizeof(local)) != 0) {
    std::cerr << "[ERROR] Cannot bind KNXnet/IP tunnel socket: " << std::strerror(errno) << "\n";
    disconnect();
    return false;
  }

  struct sockaddr_in bound {};
  socklen_t bound_len = sizeof(bound);
  std::uint16_t bound_port = 0;
  if (::getsockname(fd_, reinterpret_cast<struct sockaddr*>(&bound), &bound_len) ==
      0) {  // GCOV_EXCL_BR_LINE - a bound socket always answers
    bound_port = ntohs(bound.sin_port);
  }
  set_nonblocking(true);

  // In NAT mode the HPAI must be 0.0.0.0:0 so the device answers to the source
  // address of our datagram.
  const knxnetip::Hpai hpai =
      options_.nat ? knxnetip::Hpai{0U, 0U} : knxnetip::Hpai{*local_address, bound_port};
  const auto request = knxnetip::build_connect_request(hpai, hpai);

  if (!send_to_device(request)) {  // GCOV_EXCL_BR_LINE - a local sendto() does not fail
    std::cerr << "[ERROR] Cannot send KNXnet/IP CONNECT_REQUEST: " << std::strerror(errno)
              << "\n";  // GCOV_EXCL_LINE
    disconnect();       // GCOV_EXCL_LINE
    return false;       // GCOV_EXCL_LINE
  }

  // ---- Wait for the CONNECT_RESPONSE (bounded deadline, never hang) ----
  const std::int64_t deadline = now_ms() + std::max(1, options_.connect_timeout_ms);
  std::uint8_t buffer[kReceiveBufferBytes];  // NOLINT(modernize-avoid-c-arrays)
  while (now_ms() < deadline) {
    struct pollfd pfd {};
    pfd.fd = fd_;
    pfd.events = POLLIN;
    const int remaining = static_cast<int>(deadline - now_ms());
    const int rc = ::poll(&pfd, 1, std::max(1, remaining));
    if (rc <= 0) {
      continue;  // timeout or EINTR — loop until the deadline
    }
    const ssize_t received = ::recv(fd_, buffer, sizeof(buffer), 0);
    if (received <= 0) {  // GCOV_EXCL_BR_LINE - poll() reported readability
      continue;           // GCOV_EXCL_LINE
    }
    const std::vector<std::uint8_t> datagram(buffer,
                                             buffer + static_cast<std::ptrdiff_t>(received));
    const auto response = knxnetip::parse_connect_response(datagram);
    if (!response) {
      continue;  // not a CONNECT_RESPONSE — ignore
    }
    if (response->status != static_cast<std::uint8_t>(knxnetip::ConnectStatus::NoError)) {
      std::cerr << "[ERROR] " << label()
                << " refused the connection: " << knxnetip::connect_status_text(response->status)
                << " (status " << static_cast<int>(response->status) << ")\n";
      disconnect();
      return false;
    }
    channel_ = response->channel;
    crd_address_ = response->crd_address;
    connected_ = true;
    lost_ = false;
    tx_sequence_ = 0;
    rx_has_expected_ = false;
    ack_received_ = false;
    pending_.clear();
    heartbeat_due_ms_ = now_ms() + (static_cast<std::int64_t>(options_.heartbeat_sec) * 1000);
    std::cout << "[INFO] Tunnel established to " << label() << " (channel "
              << static_cast<int>(channel_) << ", source " << individual_address_text(crd_address_)
              << ")\n";
    return true;
  }

  std::cerr << "[ERROR] " << label() << ": no CONNECT_RESPONSE within "
            << options_.connect_timeout_ms << " ms\n";
  disconnect();
  return false;
}

void TunnelingBusLink::disconnect() {
  if (fd_ >= 0 && connected_ && !lost_) {
    // Best effort: the device drops the tunnel after 120 s of silence anyway.
    // A tunnel already known to be dead must not be released twice.
    const knxnetip::Hpai hpai{0U, 0U};
    (void)send_to_device(knxnetip::build_disconnect_request(channel_, hpai));
  }
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  connected_ = false;
  lost_ = false;
  channel_ = 0;
  pending_.clear();
}

bool TunnelingBusLink::send_group_packet(std::uint16_t group_addr,
                                         const std::vector<std::uint8_t>& apdu) {
  if (!is_connected()) {
    return false;  // also covers fd_ < 0: connect() is what opens it
  }
  const auto host = knxnetip::ipv4_from_string(options_.host);
  if (!host) {     // GCOV_EXCL_BR_LINE - connect() validated the host
    return false;  // GCOV_EXCL_LINE
  }

  const auto cemi = cemi::encode_group_request(crd_address_, group_addr, apdu);
  if (cemi.empty()) {
    return false;
  }

  // Retransmissions reuse the *same* sequence number (KNXnet/IP rule).
  ack_wait_sequence_ = tx_sequence_;
  for (int attempt = 0; attempt <= options_.retries; ++attempt) {
    const auto request = knxnetip::build_tunnelling_request(channel_, tx_sequence_, cemi);
    if (!send_to_device(request)) {  // GCOV_EXCL_BR_LINE - a local sendto() does not fail
      std::cerr << "[WARN] KNXnet/IP tunnel send failed: " << std::strerror(errno)
                << "\n";  // GCOV_EXCL_LINE
      return false;       // GCOV_EXCL_LINE
    }

    ack_received_ = false;
    const std::int64_t deadline = now_ms() + std::max(1, options_.ack_timeout_ms);
    for (;;) {
      if (ack_received_) {
        break;
      }
      if (now_ms() >= deadline) {  // GCOV_EXCL_BR_LINE - the poll timeout ends the wait first
        break;                     // GCOV_EXCL_LINE
      }
      const int remaining = static_cast<int>(deadline - now_ms());
      if (!receive_and_process(std::max(1, remaining))) {
        break;  // nothing readable — retransmit
      }
    }
    if (ack_received_) {
      tx_sequence_ = static_cast<std::uint8_t>((tx_sequence_ + 1) & 0xFF);
      return true;
    }
    if (attempt < options_.retries) {
      std::cerr << "[WARN] No TUNNELLING_ACK for sequence " << static_cast<int>(tx_sequence_)
                << " — retransmitting (" << (attempt + 1) << "/" << options_.retries << ")\n";
    }
  }

  std::cerr << "[ERROR] " << label() << ": telegram dropped after " << options_.retries
            << " retransmissions\n";
  // The sequence is consumed either way: the next telegram must not reuse it.
  tx_sequence_ = static_cast<std::uint8_t>((tx_sequence_ + 1) & 0xFF);
  return false;
}

bool TunnelingBusLink::receive_and_process(int timeout_ms) {
  if (fd_ < 0) {
    return false;
  }
  struct pollfd pfd {};
  pfd.fd = fd_;
  pfd.events = POLLIN;
  const int rc = ::poll(&pfd, 1, std::max(0, timeout_ms));
  if (rc <= 0) {
    return false;
  }
  std::uint8_t buffer[kReceiveBufferBytes];  // NOLINT(modernize-avoid-c-arrays)
  const ssize_t received = ::recv(fd_, buffer, sizeof(buffer), 0);
  if (received <= 0) {  // GCOV_EXCL_BR_LINE - poll() reported readability
    return false;       // GCOV_EXCL_LINE
  }
  const std::vector<std::uint8_t> datagram(buffer, buffer + static_cast<std::ptrdiff_t>(received));
  process(datagram);
  return true;
}

void TunnelingBusLink::process(const std::vector<std::uint8_t>& datagram) {
  const auto frame = knxnetip::parse_frame(datagram);
  if (!frame) {
    return;  // unknown service types are ignored
  }

  switch (frame->service) {
    case knxnetip::ServiceType::TunnellingRequest: {
      const auto request = knxnetip::parse_tunnelling_request(datagram);
      if (!request) {
        return;
      }
      // Every received request is ACKed with its own sequence number, even
      // when it is discarded below.
      if (request->channel == channel_) {
        (void)send_to_device(knxnetip::build_tunnelling_ack(channel_, request->sequence));
      }
      if (request->channel != channel_) {
        return;
      }

      bool expected = true;
      if (rx_has_expected_) {
        expected = request->sequence == rx_expected_;
      }
      rx_has_expected_ = true;
      rx_expected_ = static_cast<std::uint8_t>((request->sequence + 1) & 0xFF);
      if (!expected) {
        // Duplicate or out-of-order frame: ACKed above, never delivered.
        std::cerr << "[WARN] Discarding out-of-sequence tunnel frame "
                  << static_cast<int>(request->sequence) << " (expected "
                  << static_cast<int>(rx_expected_) << ")\n";
        return;
      }

      const auto message = cemi::parse_ldata_ind(request->cemi);
      if (!message) {
        return;  // L_Data.con (our own write) or individual traffic
      }
      pending_.emplace_back(message->destination, message->apdu);
      ++telegram_count_;
      return;
    }

    case knxnetip::ServiceType::TunnellingAck: {
      const auto ack = knxnetip::parse_tunnelling_ack(datagram);
      if (!ack) {
        return;
      }
      if (ack->channel != channel_) {
        return;
      }
      if (ack->sequence == ack_wait_sequence_) {
        ack_received_ = true;
      }
      return;
    }

    case knxnetip::ServiceType::ConnectionStateResponse: {
      const auto response =
          knxnetip::parse_status_response(datagram, knxnetip::ServiceType::ConnectionStateResponse);
      if (!response) {
        return;
      }
      if (response->channel != channel_) {
        return;
      }
      if (response->status != 0x00) {
        std::cerr << "[WARN] " << label() << " reported connection state error "
                  << static_cast<int>(response->status) << " — reconnecting\n";
        lost_ = true;
      }
      return;
    }

    case knxnetip::ServiceType::DisconnectRequest:
      connected_ = false;
      lost_ = true;
      return;

    default:
      return;  // routing indications, description responses, …
  }
}

bool TunnelingBusLink::poll_group_telegram(std::uint16_t& out_group_addr,
                                           std::vector<std::uint8_t>& out_apdu) {
  if (lost_) {
    return false;
  }
  if (pending_.empty()) {
    // Drain everything the socket has; received telegrams are queued so a
    // datagram never has to be delivered twice.
    while (receive_and_process(0)) {
      if (!pending_.empty()) {
        break;
      }
    }
  }
  if (pending_.empty()) {
    return false;
  }
  out_group_addr = pending_.front().first;
  out_apdu = pending_.front().second;
  pending_.pop_front();
  return true;
}

void TunnelingBusLink::send_heartbeat_if_due() {
  // The helper is only reached while connected (the only caller checks fd_ and
  // lost_ first), so this guard cannot be exercised.
  if (!connected_) {  // GCOV_EXCL_BR_LINE
    return;           // also covers fd_ < 0: connect() is what opens it
  }
  const std::int64_t current = now_ms();
  if (current < heartbeat_due_ms_) {
    return;
  }
  const auto host_unused = knxnetip::ipv4_from_string(options_.host);
  if (!host_unused) {  // GCOV_EXCL_BR_LINE - connect() validated the host
    return;            // GCOV_EXCL_LINE
  }
  const knxnetip::Hpai hpai{0U, 0U};
  (void)send_to_device(knxnetip::build_connectionstate_request(channel_, hpai));

  heartbeat_due_ms_ = current + (static_cast<std::int64_t>(options_.heartbeat_sec) * 1000);
}

BusLink::WaitResult TunnelingBusLink::wait_for_activity(int timeout_ms) {
  if (fd_ < 0) {
    return WaitResult::Error;
  }
  if (lost_) {
    return WaitResult::Error;
  }
  send_heartbeat_if_due();

  // Never block past the next heartbeat: the tunnel dies after 120 s without
  // traffic on the device side.
  std::int64_t slice = timeout_ms;
  if (connected_) {  // GCOV_EXCL_BR_LINE - wait_for_activity() is only used while connected, so the
                     // else-arm is unreachable
    const std::int64_t until_heartbeat = heartbeat_due_ms_ - now_ms();
    if (until_heartbeat < slice) {
      slice = until_heartbeat;
    }
  }
  if (slice < 0) {
    slice = 0;
  }

  struct pollfd pfd {};
  pfd.fd = fd_;
  pfd.events = POLLIN;
  const int rc = ::poll(&pfd, 1, static_cast<int>(slice));
  if (rc < 0) {  // GCOV_EXCL_BR_LINE - a UDP socket without signals does not fail poll()
    return errno == EINTR ? WaitResult::Timeout : WaitResult::Error;  // GCOV_EXCL_LINE
  }
  if (rc == 0) {
    send_heartbeat_if_due();  // the timer expired during the wait
    return WaitResult::Timeout;
  }
  if ((pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {  // GCOV_EXCL_BR_LINE
    return WaitResult::Error;                                 // GCOV_EXCL_LINE
  }
  if ((pfd.revents & POLLIN) != 0) {  // GCOV_EXCL_BR_LINE - poll() only reports POLLIN here
    (void)receive_and_process(0);
    if (lost_) {
      return WaitResult::Error;
    }
    // Only a queued telegram counts as data; an ACK or a heartbeat response
    // just means "look at the socket again".
    if (pending_.empty()) {
      return WaitResult::Timeout;
    }
    return WaitResult::GroupData;
  }
  return WaitResult::Timeout;  // GCOV_EXCL_LINE - every revents combination is handled
}

void TunnelingBusLink::set_nonblocking(bool enable) {
  if (fd_ < 0) {
    return;
  }
  const int flags = ::fcntl(fd_, F_GETFL, 0);
  if (flags < 0) {  // GCOV_EXCL_BR_LINE - fcntl on a valid fd cannot fail here
    return;         // GCOV_EXCL_LINE
  }
  ::fcntl(fd_, F_SETFL, enable ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
}

std::string TunnelingBusLink::label() const {
  return "KNXnet/IP tunnel " + options_.host + ":" + std::to_string(options_.port);
}

}  // namespace cvknx
