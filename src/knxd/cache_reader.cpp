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
 * @file cache_reader.cpp
 * @brief Implementation of the resilient knxd -> SharedGroupCache pump.
 */

#include "knxd/cache_reader.h"

#include <poll.h>
#include <time.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <vector>

#include "bus/bus_link.h"
#include "knxd/knxd_protocol.h"
#include "state/shared_group_cache.h"
#include "state/shared_tx_queue.h"
#include "util/debug_log.h"
#include "util/hex.h"

namespace cvknx {
namespace {}  // namespace

namespace detail {

void sleep_interruptible(int duration_ms, int slice_ms, const std::function<bool()>& should_stop) {
  const int slice = std::max(1, slice_ms);
  int remaining = duration_ms;
  while (remaining > 0) {
    if (should_stop && should_stop()) {
      return;
    }
    const int chunk = std::min(remaining, slice);
    struct timespec request {};
    request.tv_sec = chunk / 1000;
    request.tv_nsec = static_cast<long>(chunk % 1000) * 1000000L;
    // EINTR (signal) is expected — the loop re-checks the stop flag.
    (void)::nanosleep(&request, nullptr);
    remaining -= chunk;
  }
}

bool stop_requested(const std::function<bool()>& should_stop) {
  return static_cast<bool>(should_stop) && should_stop();
}

}  // namespace detail

namespace {

/// Push every pending group telegram into the shared cache.
/// @return number of telegrams that were pushed.
int drain_telegrams(BusLink& link, SharedGroupCache& cache) {
  int pushed = 0;
  uint16_t addr = 0;
  std::vector<uint8_t> apdu;
  while (link.poll_group_telegram(addr, apdu)) {
    ApduType apdu_type{};
    std::vector<uint8_t> value_data;
    if (!parse_apdu(apdu, apdu_type, value_data) || apdu_type == ApduType::Read) {
      continue;
    }
    cache.push(addr, value_data);
    DebugLog::knxd_recv("cache_push", KnxGroupAddress::from_eibaddr(addr).to_string(),
                        hex_encode(value_data.data(), value_data.size()));
    ++pushed;
  }
  return pushed;
}

}  // namespace

namespace detail {

/// Send every telegram waiting in the shared TX ring.
/// Clears the eventfd first, so a push during draining re-arms it.
/// @return number of telegrams handed to the bus without error.
int drain_tx_queue(BusLink& link, SharedTxQueue& queue) {
  queue.drain_notifications();
  int sent = 0;
  uint16_t addr = 0;
  std::vector<uint8_t> apdu;
  while (queue.peek(addr, apdu)) {
    if (!link.send_group_packet(addr, apdu)) {
      // The link is gone.  The telegram stays queued and is sent after the
      // reconnect — a write must never be lost silently.
      std::cerr << "[WARN] Failed to send queued telegram to " << link.label() << "\n";
      break;
    }
    queue.pop_drop();
    ++sent;
    DebugLog::knxd_send("tx_queue", KnxGroupAddress::from_eibaddr(addr).to_string(),
                        hex_encode(apdu.data(), apdu.size()));
  }
  return sent;
}

/// Wait for bus traffic, an outgoing telegram, or the timeout — one poll().
/// Never busy-loops: the eventfd is consumed by drain_tx_queue().
Activity wait_for_activity(BusLink& link, SharedTxQueue* tx_queue, int timeout_ms) {
  if (tx_queue == nullptr || tx_queue->eventfd() < 0) {
    const auto result = link.wait_for_activity(timeout_ms);
    if (result == BusLink::WaitResult::GroupData) {
      return Activity::Data;
    }
    if (result == BusLink::WaitResult::Timeout) {
      return Activity::Timeout;
    }
    return Activity::Error;
  }

  struct pollfd fds[2]{};
  nfds_t count = 0;
  int link_index = -1;
  const int link_fd = link.get_fd();
  if (link_fd >= 0) {
    fds[count].fd = link_fd;
    fds[count].events = POLLIN;
    link_index = static_cast<int>(count);
    ++count;
  }
  const int tx_index = static_cast<int>(count);
  fds[count].fd = tx_queue->eventfd();
  fds[count].events = POLLIN;
  ++count;

  const int rc = ::poll(fds, count, timeout_ms);
  if (rc <= 0) {
    return Activity::Timeout;  // timeout or EINTR — the caller re-checks stop
  }
  if ((fds[tx_index].revents & POLLIN) != 0) {
    return Activity::Tx;
  }
  // Without a link fd only the eventfd was polled — and that wake-up returned
  // above, so the false arm cannot be reached.
  if (link_index >= 0) {  // GCOV_EXCL_BR_LINE
    if ((fds[link_index].revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
      return Activity::Error;
    }
    // poll() reports POLLIN for any readable socket, so the false arm (a
    // readable link fd without POLLIN or an error flag) cannot occur.
    if ((fds[link_index].revents & POLLIN) != 0) {  // GCOV_EXCL_BR_LINE
      return Activity::Data;
    }
  }
  return Activity::Timeout;  // GCOV_EXCL_LINE - every revents combination returned above
}

}  // namespace detail

void run_cache_reader(BusLink& link, SharedGroupCache& cache, SharedTxQueue* tx_queue,
                      const CacheReaderOptions& options, const std::function<bool()>& should_stop) {
  using detail::Activity;
  using detail::drain_tx_queue;
  using detail::sleep_interruptible;
  using detail::stop_requested;
  using detail::wait_for_activity;

  const int initial_backoff_ms = std::max(1, options.reconnect_backoff_ms);
  const int max_backoff_ms = std::max(initial_backoff_ms, options.max_reconnect_backoff_ms);
  const int wait_timeout_ms = std::max(1, options.wait_timeout_ms);

  int backoff_ms = initial_backoff_ms;
  bool connected = false;

  bool logged_failure = false;
  std::chrono::steady_clock::time_point last_log{};

  // Passing an empty std::function is allowed and means "never stop".
  const auto stop = [&should_stop]() { return stop_requested(should_stop); };

  // Rate-limited warning while knxd is unreachable.
  const int retry_log_interval_ms = std::max(0, options.retry_log_interval_ms);
  const auto log_retry = [&logged_failure, &last_log,
                          retry_log_interval_ms](const std::string& reason) {
    const auto now = std::chrono::steady_clock::now();
    const auto since_last_log =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - last_log).count();
    const bool interval_passed = since_last_log >= retry_log_interval_ms;
    const bool due = !logged_failure || interval_passed;
    if (due) {
      // Emitted as a single line: the reader shares stderr with all workers.
      std::cerr << "[WARN] " + reason + " — retrying in the background\n";
      last_log = now;
      logged_failure = true;
    }
  };

  // Start out unavailable: until the first handshake succeeds there is no
  // bus connection, and /r clients should see that right away.
  cache.publish_bus_status(false);

  while (!stop()) {
    // ---------------- (Re)connect ----------------
    if (!connected) {
      if (link.connect()) {
        link.set_nonblocking(true);
        connected = true;
        backoff_ms = initial_backoff_ms;
        cache.publish_bus_status(true);

        if (logged_failure) {
          std::cout << "[INFO] Bus connection restored to " << link.label() << "\n";
          std::cout << std::flush;  // stdout is block-buffered when redirected
          logged_failure = false;
        }
        continue;  // immediately look for pending telegrams
      }

      // Make sure a half-open connection does not survive the failed attempt.
      link.disconnect();
      cache.publish_bus_status(false);  // keeps the first outage timestamp

      log_retry("Cannot connect to " + link.label());
      sleep_interruptible(backoff_ms, options.sleep_slice_ms, should_stop);
      backoff_ms = std::min(backoff_ms * 2, max_backoff_ms);
      continue;
    }

    // ---------------- Drain pending writes ----------------
    // Always before waiting: a telegram queued while we were busy must not
    // wait for the next poll slice.  Re-loop when something was sent so the
    // stop predicate and the queue are re-evaluated before blocking again.
    if (tx_queue != nullptr && drain_tx_queue(link, *tx_queue) > 0) {
      continue;
    }

    // ---------------- Wait for activity ----------------
    const auto activity = wait_for_activity(link, tx_queue, wait_timeout_ms);
    if (activity == Activity::Tx) {
      continue;  // a worker queued a telegram — send it right away
    }
    if (activity == Activity::Data && drain_telegrams(link, cache) > 0) {
      backoff_ms = initial_backoff_ms;
    }
    if (activity == Activity::Error) {
      // Connection lost (daemon restarted / "Link down, terminating" /
      // router gone).  Never fall through to a busy loop: back off, then
      // reconnect.
      std::cerr << "[WARN] Lost connection to " + link.label() + " — reconnecting\n";
      logged_failure = true;
      last_log = std::chrono::steady_clock::now();

      link.disconnect();
      connected = false;
      cache.publish_bus_status(false);
      sleep_interruptible(backoff_ms, options.sleep_slice_ms, should_stop);
      backoff_ms = std::min(backoff_ms * 2, max_backoff_ms);
    }
    // Activity::Timeout needs no action — the loop re-checks the stop flag.
  }
}

}  // namespace cvknx
