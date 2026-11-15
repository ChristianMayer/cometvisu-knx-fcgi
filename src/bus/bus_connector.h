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
 * @file bus_connector.h
 * @brief Bounded connect-with-retry helper for any BusLink.
 *
 * The long-running retry loop lives in run_cache_reader(); this helper is for
 * the one-off startup probe, where a few short attempts give a useful log line
 * without delaying the FCGI listener.
 */

#pragma once

#include "bus/bus_link.h"

namespace cvknx {

/// Connect to the bus with exponential backoff retry.
/// Attempts to connect immediately, then retries on failure with increasing delays.
///
/// Standard retry (has_worked_before=false):
///   try -> wait 500ms -> try -> wait 1s -> try -> wait 2s -> try  (4 attempts total)
///
/// Extended retry (has_worked_before=true):
///   try -> wait 500ms -> try -> wait 1s -> try -> wait 2s -> try
///   -> wait 4s -> try -> wait 8s -> try  (6 attempts total)
///
/// @param link The bus link to connect.
/// @param has_worked_before If true, use extended retry delays (up to 8s).
/// @param custom_delays_ms Optional array of retry delays in milliseconds.
///        When nullptr (default), uses built-in defaults:
///        {500, 1000, 2000, 4000, 8000}.
///        Must have at least 5 elements when provided.
/// @return true if connection succeeded.
[[nodiscard]] bool connect_bus_with_retry(BusLink& link, bool has_worked_before,
                                          const int* custom_delays_ms = nullptr);

}  // namespace cvknx
