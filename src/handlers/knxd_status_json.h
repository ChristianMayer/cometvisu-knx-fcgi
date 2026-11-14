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
 * @file knxd_status_json.h
 * @brief Report a knxd outage to the client in /r and /w responses.
 *
 * The cache keeps serving the last known values while knxd is unreachable, so
 * /r looks perfectly healthy — the client would never notice that the values
 * stopped updating.  Whenever the cache reader has no knxd connection, /r and
 * /w add an additional object to the response:
 *
 * @code
 *   {"d":{...},"i":42,"knxd":{"ok":false,"since":1763000000,"lastOk":1762990000}}
 * @endcode
 *
 * - `ok` is always `false` in this block (its presence already means "knxd is
 *   not available"; the field makes that explicit for the client).
 * - `since` is the Unix time (seconds) at which the current outage started.
 * - `lastOk` is the Unix time (seconds) of the last successful connection.
 * Both timestamps are omitted if unknown (never connected / not yet recorded).
 */

#ifndef COMETVISU_KNXD_FCGI_KNXD_STATUS_JSON_H_
#define COMETVISU_KNXD_FCGI_KNXD_STATUS_JSON_H_

#include "state/shared_group_cache.h"

namespace cvknxd {

class JsonBuilder;

/// Append `"knxd":{"ok":false,...}` when knxd is unavailable.
///
/// @param json              Builder positioned inside the response object.
/// @param status            State published by the cache reader.
/// @param force_unavailable Report the outage even if @p status still says the
///                          connection is up (used by /w after a failed send,
///                          where the write itself proves knxd is not there).
/// @return true if the block was appended.
bool add_knxd_outage(JsonBuilder& json, const SharedGroupCache::KnxdStatus& status,
                     bool force_unavailable = false);

}  // namespace cvknxd

#endif  // COMETVISU_KNXD_FCGI_KNXD_STATUS_JSON_H_
