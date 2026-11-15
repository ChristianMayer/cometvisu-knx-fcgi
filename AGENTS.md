# AGENTS.md — Instructions for LLM Coding Agents

This file provides guidelines for AI coding agents working on the
`cometvisu-knx-fcgi` project.

**Roadmap:** [ROADMAP.md](ROADMAP.md) lists the deferred work (the `/f`
endpoint, KNX IP Secure, hardware interop).  The transport layer (phases P0–P8
of the former `PLAN-KNXNETIP.md`) shipped in 2.0.0; this file is the design
record, the git history holds the implementation plan.

**Development container:** `.devcontainer/` provides the full toolchain
(g++, clang, cmake, ninja, clang-format, clang-tidy, gdb, `libfcgi-dev`,
`knxd`/`knxd-dev`, `tcpdump`, `socat`).  Use it for reproducible builds.

## Development Methodology

**Test-Driven Development (TDD) is mandatory.** The cycle is:

1. **Write a failing test** — define the expected behavior.
2. **Write the minimum code** to make the test pass.
3. **Refactor** — clean up while keeping tests green.
4. **Repeat.**

Never write implementation code before the corresponding test exists.

## Build & Test Commands

```bash
# Configure (once)
cmake -B build -DCMAKE_BUILD_TYPE=Debug

# Build
cmake --build build -j$(nproc)

# Run all tests
ctest --test-dir build --output-on-failure

# Run a specific test
ctest --test-dir build -R test_query_string --output-on-failure

# Run with verbose output
ctest --test-dir build -V
```

## Code Coverage

Every executable line and every branch arm in `src/` must be reached by a
test.  The gate is `tools/check_coverage.py`; `tools/coverage.sh` builds an
instrumented tree, runs the whole suite and then gates.

- `tools/coverage.sh` — ratchet mode (default).  A file listed in
  `tools/coverage-baseline.json` must not drop below its recorded numbers; any
  file **not** listed (i.e. everything new) must be at 100 % lines *and*
  100 % branch arms.
- `tools/coverage.sh --strict` — the target state: 100 % everywhere.  Use it
  to prove a gap is closed.
- `tools/coverage.sh --list-uncovered` — prints the exact line numbers and
  branch arms that are still missing.

Rules:

1. **New code is never added to the baseline.**  Only legacy files are listed
   there.  If a change adds a file, it must be fully covered in the same PR.
2. When a legacy gap is closed, re-record with
   `python3 tools/check_coverage.py --write-baseline tools/coverage-baseline.json`
   — never lower an existing entry by hand.
3. **CI enforces the gate on every pull request** (`.github/workflows/ci.yml`,
   job `coverage`): it builds with `ENABLE_COVERAGE=ON`, runs the suite, runs
   `tools/check_coverage.py` against the committed baseline, writes the
   per-file table into the job summary and uploads the report.  A regression or
   an uncovered new file fails the PR.
4. Use a separate build directory (`build-cov`, the default of
   `tools/coverage.sh`).  `ENABLE_COVERAGE=ON` adds
   `--coverage -O0 -g -fprofile-update=atomic`; the atomic update is required
   because the tests fork workers that write the same `.gcda` files.

Exclusions are acceptable **only** for code that a test process cannot reach:
`fork()` children that `_Exit()` before flushing, signal handlers, and
OS-failure paths that cannot be injected (`socket()`, `mmap()`, `eventfd()`,
a refused multicast join, `poll()` errors on a valid fd, `EINTR`/`EAGAIN` on a
few-byte write).  Mark them inline and say why:

```cpp
if (fd_ < 0) {  // GCOV_EXCL_BR_LINE - only fails on fd exhaustion
  ...
}
```

Supported by the checker: `GCOV_EXCL_LINE`, `GCOV_EXCL_BR_LINE`,
`GCOV_EXCL_START` / `GCOV_EXCL_STOP`.  Exception-unwind arms are ignored.

**A marker must sit on the line it describes.**  `GCOV_EXCL_LINE` and
`GCOV_EXCL_BR_LINE` apply to the line that carries the comment; a comment line
above the code does not exclude anything, so a *statement* marker belongs at the
end of the statement and a *range* marker (`START`/`STOP`) on its own line
before the first and after the last line to skip.

The checker also ignores a line whose entire content is closing braces
(`}`, `});`, `};`).  gcc emits a counter for the exit block of some functions
even though nothing can execute there — such a line carries no logic, so it is
neither counted nor expected to be hit.

**Unreachable-by-construction code should be simplified, not excluded.**
A condition that can never be false (a guard duplicated below its caller, a
`contains()` check after the loop that adds to the set, an `||` arm with an
identical operand) is dead weight: delete it, or turn an exhaustive `switch`
over an enum into an `if` chain so every arm stays reachable.  Exclusions are
the last resort for what the operating system could do but a test cannot.

## Current coverage state

**Everything under `src/` is at 100 % lines and 100 % branches.**
`tools/coverage.sh --strict` is clean, and `tools/coverage-baseline.json` only
exists as a ratchet: every file is recorded at 100 %, so a new gap in *any*
file fails the gate.

The last two legacy files were closed with process-level harnesses rather than
unit tests, because their logic only exists in a running system:

| File | how it is covered |
|---|---|
| `src/fcgi/fcgi_server.cpp` | `tests/unit/test_fcgi_server_run.cpp` drives the real FastCGI record protocol over a Unix socket (a child runs `run()`, the test speaks BEGIN_REQUEST/PARAMS/STDIN and reads STDOUT/END_REQUEST); `tests/e2e/test_spawn_mode_e2e.cpp` covers the spawn-fcgi mode of the built binary. |
| `src/main.cpp` | `tests/e2e/test_main_e2e.cpp` starts the built binary: command line (`-v`/`-h`/`--discover`), configuration validation, and the fork-based supervisor — it kills a worker and the cache reader to force restarts, kills a worker holding long-poll tokens, and shuts the pool down with SIGTERM. |

Coverage builds install a SIGTERM handler in the worker/cache-reader children
(`KNX_COVERAGE` only) so their counters are flushed when the supervisor stops
them; without it every line a child executed would look dead.

## Coding Style

- **C++20** standard.
- Clang-format: Google style, 100 character line limit.
- **Indentation: 2 spaces. Tab width: 8 spaces.** Never use tabs.
- `.clang-format` and `.clang-tidy` are in the repository root.
- Run `clang-format -i <file>` before committing.
- Header guards: `#pragma once` (modern and supported everywhere).
- Namespace: `cvknx` (CometVisu KNX Backend).

## Copyright Header

**Every source file (.cpp, .h, CMakeLists.txt) MUST start with the GPLv3
copyright header.** This is mandatory for all files, now and in the future.

For C++ files (`.cpp`, `.h`):
```cpp
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
```

For CMake files (`CMakeLists.txt`), use `#` instead of `//`:
```cmake
# Copyright (C) 2026 Christian Mayer and the CometVisu contributors
#
# This program is free software: ...
# ...
```

The copyright header must be followed by exactly one blank line before the
actual file content (e.g., `#pragma once` or `#include`).

## File Organization

| Directory        | Purpose                                      |
|------------------|----------------------------------------------|
| `src/`           | Production source code                       |
| `src/bus/`       | Transport seam: `BusLink`, `BusConfig`, `BusFactory`, shared TX link, cache reader loop |
| `src/knxnetip/`  | cEMI codec, KNXnet/IP framing, routing/tunnelling links, discovery |
| `src/fcgi/`      | FastCGI protocol implementation              |
| `src/router/`    | URL routing and handler dispatch             |
| `src/handlers/`  | `/l`, `/r`, `/w` endpoint handlers           |
| `src/knxd/`      | knxd Unix socket client and eibd protocol    |
| `src/state/`     | Session store, shared & local group caches, shared TX queue |
| `src/util/`      | Query string parser, JSON builder, hex utils |
| `tests/unit/`    | Unit tests (mocked dependencies)             |
| `tests/integration/` | Integration tests (real or fake socket)  |
| `tests/e2e/`     | End-to-end tests (full FCGI cycle)           |
| `mocks/`         | Mock/fake implementations for testing        |

## Key Design Decisions

1. **Fork-based worker pool**: The FCGI accept loop uses multiple forked
   worker processes, each running its own `FCGX_Accept_r()` loop on the
   shared listen socket. The OS serializes accept calls across processes.
   This is critical because long-poll `/r` requests block for up to 300
   seconds — without multiple workers, one long-poll would block all other
   clients. Worker count is configurable via `FCGI_WORKERS` (default: 20).
   Workers never open a bus connection of their own (see decision 2).
   All shared state (SharedGroupCache, SharedTxQueue, SessionStore) is
   protected by process-shared mutexes for safe access across processes.
   **The parent supervises the pool and is the only process that reaps
   children** (no SIGCHLD handler that calls `waitpid()` — it would steal the
   exit statuses).  Dead workers and a dead cache reader are restarted with a
   crash-loop backoff (100 ms … 30 s).  The parent therefore keeps the listen
   socket open so replacements inherit it; closing it made every respawned
   worker fall back to spawn-fcgi mode and exit immediately.
   Fork-based workers avoid Docker < 20.10.10 seccomp incompatibility where
   `clone3`/`clone` with process-sharing flags (CLONE_VM, CLONE_THREAD) is
   blocked, preventing `std::thread` creation.

2. **Transports and the single bus owner**: `KNX_TRANSPORT` is **required**
   (`knxd`, `ip-tunnel`, `ip-routing`); there is no default, so a wrong `ip-*`
   setup can never be mistaken for a knxd outage.  Handlers, router and cache
   only ever see the `BusLink` interface (`src/bus/bus_link.h`); the
   `BusFactory` is the only place that knows the implementations.  Never add
   an `if (transport == ...)` outside `main.cpp` / `BusFactory`.
   - **Exactly one process owns the bus connection** — the cache reader — for
     RX *and* TX of every transport.  Workers only push into the shared TX
     queue (`SharedTxQueue`, mmap ring + inherited `eventfd`) via
     `SharedTxBusLink`.  `FCGI_WORKERS` therefore multiplies neither knxd
     client slots (one instead of `FCGI_WORKERS + 1`) nor KNXnet/IP tunnels
     (one instead of one per worker).
   - **knxd** (`KnxdClient`): one read-write group socket, opened by the cache
     reader.  Every `EIB_OPEN_GROUPCON` reply is read with a deadline
     (`set_handshake_timeout_ms()`, default 5 s) — a knxd that accepts the
     connection but never answers must not block the caller.
   - **ip-routing** (`RoutingBusLink`): one UDP socket, joined to
     `KNX_IP_MULTICAST`, TTL 16, loop enabled.  Stateless: no ACKs, no
     heartbeat.  Own echoes come back as `L_Data.req` and are dropped by the
     cEMI codec.
   - **ip-tunnel** (`TunnelingBusLink`): CONNECT / CONNECTIONSTATE /
     DISCONNECT, one sequence counter, every inbound request ACKed,
     out-of-sequence frames ACKed **and discarded** (a duplicate toggle would
     switch twice), retransmission with the *same* sequence on a missing ACK,
     heartbeat every `KNX_IP_HEARTBEAT_SEC` (devices drop a tunnel after 120 s
     of silence).
   - **cEMI trap**: only `L_Data.ind` (0x29) reaches the cache.  `L_Data.con`
     (0x2E) confirms *our own* write and must be dropped, otherwise every
     write is delivered twice.
   - **Outage key**: `/r` and `/w` report `"knxd": {...}` for the knxd
     transport and `"bus": {...}` for the KNXnet/IP transports — same inner
     shape, key from `BusConfig::outage_key()`.
   - **TX queue semantics**: a telegram that cannot be sent stays queued (a
     write is never lost silently), but a write is **rejected** while the bus
     is unavailable — `SharedTxBusLink` returns `false`, `/w` answers `503`
     instead of pretending success.

3. **Shared-memory group cache (`SharedGroupCache`)**: A fixed-size
   open-addressing hash table (2048 entries) in `mmap(MAP_SHARED|MAP_ANONYMOUS)`
   memory, accessible by all worker processes.  Each entry stores the KNX group
   address, APDU value data (up to 14 bytes), a Unix timestamp, and the
   `pushed_at` position.  Synchronization:
   - `pthread_mutex_t` with `PTHREAD_PROCESS_SHARED` and `PTHREAD_MUTEX_ROBUST`
     protects all read/write access to cache entries.
   - `pthread_cond_t` with `PTHREAD_PROCESS_SHARED` and `CLOCK_MONOTONIC`
     enables zero-CPU blocking for long-poll readers.
   - `std::atomic<uint32_t> position` — lock-free reads, monotonically
     increasing across all workers.  The single source of truth for the `i`
     field in `/r` responses.
   - `std::atomic<uint32_t> generation` — incremented on each push, used by
     `wait_for_new_data()` for efficient change detection.

4. **Long-poll**: For `/r` requests, the ReadHandler blocks on the shared
   condition variable via `pthread_cond_timedwait()` instead of polling a knxd
   socket fd.  When the cache reader pushes new data, it broadcasts the
   condition variable — all blocked workers wake immediately (microsecond
   latency) and call `get_delta()` to find matching entries.  Zero CPU while
   waiting.  The `wait_for_new_data()` method handles spurious wakeups and
   robust-mutex recovery (EOWNERDEAD).

   Only `/r` takes a load-shedding/concurrency token; `/w` and `/l` never do,
   because a spurious 503 on a write surfaces in CometVisu as a failed switch
   operation.  Tokens are released via RAII (a throwing handler yields HTTP 500
   instead of terminating the worker) and the parent restores the token of a
   worker that died, so the pool cannot shrink permanently.

5. **KNX semantic correctness rules** — these are non-negotiable constraints
   that every handler implementation MUST satisfy:

   **Rule 1 — No duplicate delivery**: Every KNX telegram must be delivered
   to the client exactly once.  Duplicate delivery is a correctness bug
   because KNX telegrams carry semantic meaning beyond their data value:
   a "toggle" command toggles twice if delivered twice, a "scene trigger"
   triggers the scene twice, a "step" dimming command steps twice.  The
   handler must use `already_written` deduplication within a single response
   AND must never return data with a stale position (`i`) that would cause
   re-reporting of the same telegram on the next poll.

   **Rule 2 — Immediate delivery**: Group telegrams arriving on the knxd
   group socket must be processed without delay.  The cache reader process
   pushes them to the shared cache and broadcasts the condition variable
   immediately.  Blocked long-poll readers wake and query `get_delta()`
   with zero additional latency.

   **Rule 3 — Authoritative index**: The `i` value in every response must
   come from the shared cache's `position` counter.  This counter is
   `std::atomic<uint32_t>`, incremented atomically on each push, and shared
   across all worker processes via `mmap`.  The handler must never fabricate,
   locally infer, or manipulate the position.  The shared counter guarantees
   monotonicity — `i` never goes backward, preventing duplicate delivery
   across worker boundaries.

6. **No external JSON library**: The JSON responses are simple enough to build
   with a minimal, purpose-built `JsonBuilder`.

7. **No external HTTP parser**: FastCGI provides parsed `QUERY_STRING` via
   `FCGI_PARAMS`. We parse the query string ourselves with `QueryString`.

8. **Cache reader process**: A dedicated child process (forked before the
   worker pool) that owns the single bus connection (`BusLink`) for RX *and*
   TX.  It runs a `poll()`-based loop over `{link_fd, tx_eventfd}`: drain the
   shared TX queue first, then drain all pending group telegrams, parse APDU
   data, and push value data to the shared cache.  Each push increments the
   shared position counter and broadcasts the condition variable to wake
   blocked long-poll readers.
   The loop lives in `run_cache_reader()` (`src/bus/cache_reader.{h,cpp}`, unit
   tested via `MockBusLink`) and is resilient by design: the bus may be absent
   at startup or restart at any time (`Link down, terminating`), so every
   failure path reconnects forever with an exponential backoff (500 ms … 30 s)
   and an interruptible sleep.  It must never busy-loop — a spin here burns a
   full CPU core on an embedded system, which is exactly the bug this design
   prevents.  `wait_for_activity()` therefore distinguishes `Timeout` (no data,
   connection fine) from `Error` (POLLHUP/POLLERR/POLLNVAL or not connected),
   and the eventfd is always consumed before draining so it cannot stay
   readable forever.

9. **Bus availability is reported to the client**: the cache reader publishes
   its connection state into the shared memory region
   (`SharedGroupCache::publish_bus_status()`, read back with `bus_status()`):
   `available`, `last_ok` (Unix time of the last successful connection) and
   `down_since` (Unix time the current outage started, 0 while connected).
   Every retry keeps the first `down_since` of the current outage.  The state
   starts as "available" — until the reader reports otherwise no read may be
   rejected.
   Whenever the connection is down, `/r` and `/w` append
   `"knxd":{"ok":false,"since":<ts>,"lastOk":<ts>}` (key `"bus"` for the
   KNXnet/IP transports) via
   `add_bus_outage()` (`src/handlers/bus_status_json.{h,cpp}`); the field is
   absent while knxd is reachable, so the normal case costs no bytes.  `/r`
   answers **`503`** (a read is not successful while the bus is unreachable) and
   skips the long-poll wait — nothing can arrive, so waiting would only delay
   the message; the body still carries the last known values.  `/w` answers
   `503 {"error":"write failed",...}` and passes `force_unavailable = true`,
   because a failed send proves the outage even if the shared state has not
   caught up yet.  Without this the client cannot distinguish "bus is quiet"
   from "connection lost" and shows frozen values forever.

## Knxd Protocol Details

The eibd client protocol over Unix socket:

### Wire Format
```
[2 bytes: payload length (big-endian)] [payload]
```

### Payload Structure
```
[2 bytes: message type] [message-specific data]
```

### Key Message Types (from `eibtypes.h`)

| Constant                | Value    | Purpose                        |
|-------------------------|----------|--------------------------------|
| `EIB_OPEN_GROUPCON`     | `0x0026` | Open group socket connection   |
| `EIB_GROUP_PACKET`      | `0x0027` | Send group telegram            |
| `EIB_APDU_PACKET`       | `0x0025` | Received group telegram        |

### Group Address Format
KNX three-level address `X/Y/Z` → 16-bit:
```
uint16_t addr = (X << 11) | (Y << 8) | Z;
```

### APDU for Group Value Write
```
byte 0: 0x00
byte 1: 0x80 | (first_data_byte & 0x3F)   // 0x80 = write, 0x40 = response, 0x00 = read
bytes 2..N: remaining data bytes
```

For multi-byte values (e.g., DPT 9.001 temperature: 2 bytes):
```
0x00 0x80 0x0c 0x6f   → A_GroupValue_Write, data=0c6f
```

### Group Socket Open (EIB_OPEN_GROUPCON)
```
[0x00 0x26] [write_only_byte]
```
Response: empty success or error message.

### Sending a Group Packet (EIB_GROUP_PACKET)
```
[0x00 0x27] [dest_addr_hi] [dest_addr_lo] [APDU bytes...]
```

### Receiving a Group Packet (EIB_APDU_PACKET)
```
[0x00 0x25] [src_pa_hi] [src_pa_lo] [dst_ga_hi] [dst_ga_lo] [APDU bytes...]
```
Note: The destination group address (dst_ga) is at offset 2-3 of the payload
(after the 2-byte type), NOT at offset 0-1 (which is the source physical address).

knxd also echoes injected telegrams back as GROUP_PACKET messages:
```
[0x00 0x27] [src_pa_hi] [src_pa_lo] [dst_ga_hi] [dst_ga_lo] [APDU bytes...]
```
Both message types are handled identically by the cache reader — the
payload structure is the same (src_pa + dst_ga + apdu).

## APDU Decoding

Given APDU bytes `[b0, b1, b2, ...]`:
- `b0` = always 0x00 for group value
- `b1 & 0xC0` = APCI (Application Layer Protocol Control Information):
  - `0x00` = A_GroupValue_Read
  - `0x40` = A_GroupValue_Response
  - `0x80` = A_GroupValue_Write
- For 1-byte values: `b1 & 0x3F` is the value
- For multi-byte values: `b2, b3, ...` are the remaining data bytes

## Hex Encoding for CometVisu

KNX data values are transmitted as hex strings in CometVisu JSON:
- Single byte `0x42` → `"42"`
- Two bytes `0x0c 0x6f` → `"0c6f"`
- Always lowercase, no spaces, no `0x` prefix.

## Test Mocking Strategy

For unit tests of the shared cache, use `SharedGroupCache::create()` which
allocates a real mmap'd region — this works fine in single-process tests.
For multi-instance tests (simulating multiple workers), use `attach()` to
share the same underlying `SharedCacheData` pointer.

For integration tests of the ReadHandler, push test data directly to the
shared cache via `cache_.push()` before calling `handle()`.  For poll-loop
tests that need deferred data arrival, use a separate thread that calls
`cache_.push()` after a short delay while the handler is blocked in
`wait_for_new_data()`.

### Concurrency and failure-mode tests (mandatory for transport code)

The multi-process parts of the system are the ones that break silently, so they
each own a dedicated suite — extend them, do not weaken them:

| Suite | What it proves |
|---|---|
| `tests/integration/test_tx_queue_multiprocess.cpp` | forked producers lose/duplicate/reorder nothing, a killed producer does not wedge the ring, `EOWNERDEAD` recovery under load, full queue rejects cleanly, a restarted consumer finds the backlog intact |
| `tests/unit/test_cache_reader_backpressure.cpp` | slow/silent bus, backlog preserved across an outage, prompt shutdown with a large backlog, concurrent handler enqueues |
| `tests/integration/test_transport_recovery.cpp` | real `TunnelingBusLink` + queue against a rate-limited / silent / restarting router: exactly-once delivery, backlog survives the outage, device-side tunnel loss triggers a reconnect |
| `test_concurrent_clients` (new cases) | many FCGI clients across forked workers reach the bus exactly once; writes during an outage are rejected **and not queued**; blocked long-polls never delay `/w` |

Rules these tests encode (do not regress them):
- the TX ring is peek-then-drop: a telegram is only removed after the link
  accepted it;
- at most `kMaxTxBatch` telegrams are sent per drain call, with a zero-timeout
  poll in between, so RX and `should_stop()` are never starved;
- a failed send must fall through to a sleeping wait — a backlog plus a failing
  link must never spin;
- every loop iteration re-checks `BusLink::is_connected()`: a link can die
  between two polls (device dropped the tunnel) and the reconnect depends on it.

For transport-independent tests, `MockBusLink` in `mocks/mock_bus_link.h`
provides a scriptable `BusLink` (connection failures, send failures, queued
inbound telegrams).  ReadHandler tests do not need it: they push directly into
the shared cache.

## Reference: Existing CometVisu Backend Implementations

- PHP: https://github.com/CometVisu/cometvisu-bsp/blob/master/src/php/helper/KnxHelper.php
- Perl (linkback): part of the CometVisu distribution

These handle address parsing, hex conversion, and the full protocol flow.

## Logging

Use simple stderr-based logging with levels:
- `ERROR`: Fatal problems
- `WARN`: Recoverable issues
- `INFO`: Normal operational messages
- `DEBUG`: Detailed diagnostics

Format: `[LEVEL] [timestamp] message`
