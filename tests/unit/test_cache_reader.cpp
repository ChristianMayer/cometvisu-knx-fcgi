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
 * @file test_cache_reader.cpp
 * @brief Tests for the knxd -> SharedGroupCache reader loop.
 *
 * The loop must survive an absent or dying knxd: it waits between reconnect
 * attempts (never spinning), retries forever, and resumes pushing telegrams
 * once knxd comes back.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

#include "../../mocks/mock_bus_link.h"
#include "../../src/knxd/cache_reader.h"
#include "../../src/state/shared_group_cache.h"
#include "../../src/state/shared_tx_queue.h"

namespace cvknx {
namespace {

using Clock = std::chrono::steady_clock;

/// Options with tiny delays so tests run fast while still measuring real sleeps.
CacheReaderOptions FastOptions() {
  CacheReaderOptions options;
  options.wait_timeout_ms = 5;
  options.reconnect_backoff_ms = 20;
  options.max_reconnect_backoff_ms = 20;
  options.sleep_slice_ms = 2;
  return options;
}

int64_t ElapsedMs(Clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
}

TEST(CacheReaderTest, StopsImmediatelyWithoutTouchingKnxdWhenAlreadyStopped) {
  MockBusLink knxd;
  SharedGroupCache cache;
  ASSERT_TRUE(cache.create());

  run_cache_reader(knxd, cache, nullptr, FastOptions(), [] { return true; });

  EXPECT_EQ(knxd.connect_calls(), 0);
  EXPECT_EQ(knxd.wait_calls(), 0);
}

TEST(CacheReaderTest, WaitsBetweenReconnectAttemptsWhenKnxdIsMissing) {
  MockBusLink knxd;
  knxd.set_connection_success(false);  // knxd is not there (no /run/knx)
  SharedGroupCache cache;
  ASSERT_TRUE(cache.create());

  const auto start = Clock::now();
  // Stop after the fourth failed connect attempt.
  run_cache_reader(knxd, cache, nullptr, FastOptions(),
                   [&knxd] { return knxd.connect_calls() >= 4; });

  EXPECT_GE(knxd.connect_calls(), 4);
  // Three backoff sleeps of 20 ms must have happened — a busy loop would
  // return in well under a millisecond and fail here.
  EXPECT_GE(ElapsedMs(start), 50);
}

TEST(CacheReaderTest, ReconnectsWhenTheConnectionIsLost) {
  MockBusLink knxd;
  knxd.set_connection_success(true);
  knxd.set_wait_error_count(3);  // first three waits report a dead connection
  SharedGroupCache cache;
  ASSERT_TRUE(cache.create());

  const auto start = Clock::now();
  // 1 initial connect + 3 reconnects.
  run_cache_reader(knxd, cache, nullptr, FastOptions(),
                   [&knxd] { return knxd.connect_calls() >= 4; });

  EXPECT_GE(knxd.connect_calls(), 4);
  // One backoff sleep per lost connection — again, no busy loop.
  EXPECT_GE(ElapsedMs(start), 50);
}

TEST(CacheReaderTest, DoesNotReconnectWhileTheConnectionIsHealthy) {
  MockBusLink knxd;
  knxd.set_connection_success(true);
  SharedGroupCache cache;
  ASSERT_TRUE(cache.create());

  run_cache_reader(knxd, cache, nullptr, FastOptions(),
                   [&knxd] { return knxd.wait_calls() >= 20; });

  EXPECT_EQ(knxd.connect_calls(), 1);
  EXPECT_GE(knxd.wait_calls(), 20);
}

TEST(CacheReaderTest, PushesReceivedTelegramsIntoSharedCache) {
  MockBusLink knxd;
  knxd.set_connection_success(true);
  // APDU for A_GroupValue_Write with value 0x42.
  knxd.enqueue_telegram(0x0A03, {0x00, 0x80, 0x42});
  SharedGroupCache cache;
  ASSERT_TRUE(cache.create());

  run_cache_reader(knxd, cache, nullptr, FastOptions(),
                   [&cache] { return cache.get(0x0A03).has_value(); });

  const auto value = cache.get(0x0A03);
  ASSERT_TRUE(value.has_value());
  ASSERT_EQ(value->size(), 1U);
  EXPECT_EQ((*value)[0], 0x42);
  EXPECT_EQ(cache.position(), 1U);
}

TEST(CacheReaderTest, IgnoresGroupValueReadTelegrams) {
  MockBusLink knxd;
  knxd.set_connection_success(true);
  // A_GroupValue_Read has no value — it must not enter the cache.
  knxd.enqueue_telegram(0x0A03, {0x00, 0x00});
  SharedGroupCache cache;
  ASSERT_TRUE(cache.create());

  run_cache_reader(knxd, cache, nullptr, FastOptions(), [&knxd] { return knxd.wait_calls() >= 5; });

  EXPECT_FALSE(cache.get(0x0A03).has_value());
  EXPECT_EQ(cache.position(), 0U);
}

// ================================================================
// The reader publishes the knxd connection state for /r and /w.
// ================================================================

TEST(CacheReaderTest, PublishesAvailabilityWhileConnected) {
  MockBusLink knxd;
  knxd.set_connection_success(true);
  SharedGroupCache cache;
  ASSERT_TRUE(cache.create());

  run_cache_reader(knxd, cache, nullptr, FastOptions(), [&knxd] { return knxd.wait_calls() >= 3; });

  const auto status = cache.bus_status();
  EXPECT_TRUE(status.available);
  EXPECT_GT(status.last_ok, 0U) << "last successful connection must be stamped";
  EXPECT_EQ(status.down_since, 0U);
}

TEST(CacheReaderTest, PublishesOutageWhenKnxdIsMissing) {
  MockBusLink knxd;
  knxd.set_connection_success(false);  // no /run/knx
  SharedGroupCache cache;
  ASSERT_TRUE(cache.create());

  run_cache_reader(knxd, cache, nullptr, FastOptions(),
                   [&knxd] { return knxd.connect_calls() >= 2; });

  const auto status = cache.bus_status();
  EXPECT_FALSE(status.available);
  EXPECT_GT(status.down_since, 0U) << "the outage start must be stamped";
}

TEST(CacheReaderTest, PublishesOutageWhileDisconnected) {
  MockBusLink knxd;
  knxd.set_connection_success(true);
  knxd.set_wait_error_count(1);  // connection dies on the first wait
  SharedGroupCache cache;
  ASSERT_TRUE(cache.create());

  // Stop as soon as the reader has connected once and then reported the
  // outage (before it reconnects) — the initial "unavailable" publish must
  // not end the loop early.
  bool saw_connected = false;
  run_cache_reader(knxd, cache, nullptr, FastOptions(), [&cache, &saw_connected] {
    const auto status = cache.bus_status();
    if (status.available) {
      saw_connected = true;
      return false;
    }
    return saw_connected && status.down_since > 0;
  });

  const auto status = cache.bus_status();
  EXPECT_TRUE(saw_connected);
  EXPECT_FALSE(status.available);
  EXPECT_GT(status.down_since, 0U);
  EXPECT_GT(status.last_ok, 0U) << "the last working connection must stay reported";
}

// ---- Shared TX queue integration ----

TEST(CacheReaderTest, DrainsTxQueueBeforePolling) {
  MockBusLink knxd;
  SharedGroupCache cache;
  ASSERT_TRUE(cache.create());
  SharedTxQueue tx;
  ASSERT_TRUE(tx.create());
  ASSERT_TRUE(tx.push(0x0A03, {0x00, 0x80, 0x42}));

  run_cache_reader(knxd, cache, &tx, FastOptions(),
                   [&knxd] { return !knxd.sent_packets().empty(); });

  ASSERT_EQ(knxd.sent_packets().size(), 1U);
  EXPECT_EQ(knxd.sent_packets()[0].group_addr, 0x0A03);
  EXPECT_EQ(tx.size(), 0U);
}

TEST(CacheReaderTest, TxQueueDrainedWhenLinkIdle) {
  MockBusLink knxd;
  SharedGroupCache cache;
  ASSERT_TRUE(cache.create());
  SharedTxQueue tx;
  ASSERT_TRUE(tx.create());

  CacheReaderOptions options = FastOptions();
  // Long slice: only the eventfd can end the wait this quickly.
  options.wait_timeout_ms = 5000;

  std::thread producer([&tx] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    (void)tx.push(0x0A03, {0x00, 0x80, 0x42});
    tx.signal();
  });

  const auto start = Clock::now();
  run_cache_reader(knxd, cache, &tx, options, [&knxd] { return !knxd.sent_packets().empty(); });
  const int64_t elapsed = ElapsedMs(start);
  producer.join();

  EXPECT_LT(elapsed, 1000) << "the eventfd must wake the loop, not the poll slice";
  ASSERT_EQ(knxd.sent_packets().size(), 1U);
}

TEST(CacheReaderTest, TxIsNotReplayedWhileDisconnected) {
  // The link cannot send: the telegram must stay queued (no loss) instead of
  // being reported as sent, and the loop must not spin.
  MockBusLink knxd;
  knxd.set_send_fail_count(1000);  // link is up but every send fails
  SharedGroupCache cache;
  ASSERT_TRUE(cache.create());
  SharedTxQueue tx;
  ASSERT_TRUE(tx.create());
  ASSERT_TRUE(tx.push(0x0A03, {0x00, 0x80, 0x42}));

  const auto deadline = Clock::now() + std::chrono::milliseconds(200);
  run_cache_reader(knxd, cache, &tx, FastOptions(),
                   [&deadline] { return Clock::now() >= deadline; });

  EXPECT_TRUE(knxd.sent_packets().empty());
  EXPECT_EQ(tx.size(), 1U) << "unsent telegrams must not be dropped";
}

}  // namespace
}  // namespace cvknx
