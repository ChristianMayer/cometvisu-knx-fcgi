# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Breaking changes

- **`KNX_TRANSPORT` is mandatory.** The backend no longer defaults to knxd.
  All three transports — `knxd`, `ip-tunnel`, `ip-routing` — are equally
  supported and the operator selects one explicitly. An unset or unknown value
  logs `[ERROR]` and exits with status 1; it is never replaced by a silent
  default (a wrong `ip-*` configuration must not look like a knxd outage).

  Migration: add `KNX_TRANSPORT=knxd` to the environment of every existing
  deployment to get 1.x behaviour unchanged.

- **The outage block in `/r` and `/w` is transport-dependent.** It is still
  `"knxd": {"ok": false, ...}` when `KNX_TRANSPORT=knxd`, but `"bus": {...}`
  for the KNXnet/IP transports. The inner shape (`ok`/`since`/`lastOk`) is
  identical; clients must ignore keys they do not know.

- The build-time condition for knxd is now a CMake option,
  `KNX_BUILD_KNXD_TRANSPORT` (default `ON`). With `-DKNX_BUILD_KNXD_TRANSPORT=OFF`
  the project builds and runs without any knxd header or daemon.

### Added

- **KNXnet/IP IP tunneling** (`KNX_TRANSPORT=ip-tunnel`, `src/knxnetip/`): a
  full tunnelling client — CONNECT/CONNECTIONSTATE/DISCONNECT handshake,
  sequence numbers with duplicate and out-of-order rejection, TUNNELLING_ACK
  with retransmission of the same sequence, heartbeat every
  `KNX_IP_HEARTBEAT_SEC`, reconnect with the existing exponential backoff.
  One tunnel serves all workers.
- **KNXnet/IP IP routing** (`KNX_TRANSPORT=ip-routing`): multicast member of
  `224.0.23.12`, stateless fire-and-forget sending, TTL 16, loop enabled. Works
  with **no knxd installed**. Own echoes (`L_Data.req`) are dropped by the
  codec, so a write is never delivered twice.
- **Shared TX queue** (`src/state/shared_tx_queue.*`): a 256-slot ring in
  shared memory plus an inherited `eventfd`. Workers only enqueue group
  telegrams; the cache reader is the single owner of the bus connection for
  RX *and* TX. `FCGI_WORKERS` therefore no longer multiplies knxd client slots
  (one instead of `FCGI_WORKERS + 1`) or KNXnet/IP tunnels (one instead of one
  per worker). A telegram that cannot be sent stays queued — a write is never
  lost silently — and a write during an outage is rejected (`503`) instead of
  being queued.
- **Transport seam** (`src/bus/`): one `BusLink` interface, one implementation
  per transport, and a `BusFactory` as the only place that knows them.
  Handlers, router and cache never branch on the transport.
- **cEMI codec** (`src/knxnetip/cemi.*`) and **KNXnet/IP framing**
  (`src/knxnetip/knxnetip_frame.*`) with complete hex vectors in the unit
  tests (`test_cemi`, `test_knxnetip_frame`).
- **Discovery**: `cometvisu-knx-fcgi --discover` sends a SEARCH_REQUEST and
  prints every answering device with its individual and multicast address
  (`src/knxnetip/discovery.*`).
- **Coverage gate** (`ENABLE_COVERAGE=ON`, `tools/coverage.sh`,
  `tools/check_coverage.py`): every line and every branch arm in `src/` must be
  reached by a test.  New files must be at 100 %; files in
  `tools/coverage-baseline.json` must not regress, and `--strict` enforces
  100 % everywhere.  New tests: `test_knxnetip_frame_negative`,
  `test_transport_edge_cases` (plus many added cases).
- `test_bus_config`, `test_shared_tx_queue`, `test_shared_tx_bus_link`,
  `test_knxnetip_frame`, `test_cemi`, `test_knxnetip_routing`,
  `test_knxnetip_tunnel`, `test_discovery`.

### Fixed

- **The cache reader ignored a dead bus link when the shared TX queue was in
  use.**  The loop polls the link fd itself, so a device that dropped the
  tunnel (`CONNECTIONSTATE_RESPONSE` with status `0x04`) or a link that
  reported itself disconnected was never reconnected — writes stayed queued
  forever.  Every iteration now re-checks `BusLink::is_connected()`.
  (Found by `test_transport_recovery`.)
- **A permanently failing send could spin the cache reader at 100 % CPU.**
  While a backlog exists the loop polls with a zero timeout; with a link that
  keeps rejecting sends that became a busy loop.  A failed send now falls
  through to a normal (sleeping) wait and the telegram stays queued.
- **A failed send no longer consumes the telegram.**  The queue is peeked, and
  the slot is only dropped after the bus link accepted the telegram — a write
  is never lost silently.
- **TX bursts no longer starve inbound traffic.**  At most 8 telegrams are sent
  per batch, with a zero-timeout poll in between, so received telegrams are
  processed during a write burst and `SIGTERM` is honoured between telegrams.

### Tests

- New multi-process and failure-mode suites: `test_tx_queue_multiprocess`
  (forked producers, killed producers, robust-mutex recovery under load, full
  queue, consumer restart), `test_cache_reader_backpressure` (slow bus, silent
  bus, backlog across an outage, prompt shutdown, concurrent handlers),
  `test_transport_recovery` (real tunnelling link + queue against a
  rate-limited, silent and restarting router) and new FCGI-level cases in
  `test_concurrent_clients` (concurrent writes across forked workers delivered
  exactly once, writes during an outage rejected and not queued, long-polls not
  delaying writes).  The tunnelling suite gained delayed-ACK retransmission,
  stale-ACK, inbound-burst and RX/TX interleaving cases.

### Changed

- **The project is renamed from `cometvisu-knxd-fcgi` to `cometvisu-knx-fcgi`.**
  The GitHub repository moved to
  <https://github.com/CometVisu/cometvisu-knx-fcgi> (fork:
  <https://github.com/ChristianMayer/cometvisu-knx-fcgi>).  The CMake project
  name, the executable, the install target, the `--help`/`--version` output,
  the startup log line and the VS Code launch/task configurations all use the
  new name.  Deployments that start the binary by path (`spawn-fcgi`, container
  entrypoints, systemd units) must be updated.  A symlink
  `cometvisu-knxd-fcgi -> cometvisu-knx-fcgi` keeps existing setups working
  during the transition.
- Namespace `cvknxd` → `cvknx`, CMake targets `cvknxd_*` → `cvknx_*`,
  compile definition `CVKNXD_BINARY` → `CVKNX_BINARY`. All headers use
  `#pragma once`.
- The knxd client (now `KnxdClient`, a `BusLink`) opens one read-write group
  socket in the cache reader instead of a write-only socket per worker.
- `connect_knxd_with_retry()` became the transport-neutral
  `connect_bus_with_retry()` (`src/bus/bus_connector.*`) and is used for the
  short startup probe.
- **Development container** (`.devcontainer/`) with the complete toolchain
  (g++, clang, cmake, ninja, clang-format, clang-tidy, gdb), `libfcgi-dev`,
  `knxd`/`knxd-dev` for the knxd transport, and the networking tools needed for
  KNXnet/IP work.

### Fixed

- **`KnxdClient::connect()` leaked a file descriptor** when the socket path was
  longer than `sockaddr_un::sun_path`: the early return left the freshly created
  socket open. Every rejected attempt (a misconfigured `KNXD_SOCKET`) burned one
  descriptor per reconnect, so the cache reader would eventually exhaust them.

- **A POST/PUT request could kill a worker process.** `read_request()` read the
  body with `FCGI_fread(..., stdin)` in *both* modes. In direct-socket mode that
  stream belongs to the spawn-fcgi layer and returns a meaningless byte count;
  adding it to the running total overflowed and the following
  `std::string::resize()` threw `std::length_error` out of `run()`, terminating
  the worker (the supervisor restarted it, but the request was lost). The
  direct-socket path now reads the body from the request's FCGX stream
  (`FCGX_GetStr(request_.in)`), and both paths clamp the reported count.

### Tests

- **Coverage gate is enforced in CI** (`tools/check_coverage.py`, job
  `coverage`): every line and every branch arm under `src/` must be reached by a
  test.  **`tools/coverage.sh --strict` is clean — 100 % lines and 100 %
  branches for every file**; the committed baseline records that floor so a new
  gap in any file fails the build.
- `tests/unit/test_fcgi_server_run.cpp`: full FastCGI record protocol against a
  child running `FcgiServer::run()` — parameter parsing, POST bodies, the
  long-poll capacity limiting, handler exceptions and the shutdown wake-up.
- `tests/e2e/test_spawn_mode_e2e.cpp`: the built binary in spawn-fcgi mode,
  driven over stdin/stdout (including the POST body paths).
- `tests/e2e/test_main_e2e.cpp`: command line, configuration validation and the
  fork-based supervisor — worker and cache-reader restarts after a kill, token
  restoration after a worker dies during a long poll, and the SIGTERM shutdown.
  A fake KNXnet/IP device covers `--discover` with a real answer.
- `tests/unit/test_knxd_client_protocol.cpp`: scripted fake knxd for the eibd
  handshake, group writes, APDU/injected group packets, partial and unparsable
  frames, handshake deadlines and recovery after a broken socket.
- New suites: `test_fcgi_request`, the login config-block builder, the cache
  reader's direct `poll()` wake-up paths, robust-mutex recovery (a forked child
  dies holding the shared cache lock) and full-table/age-filter cases for
  `SharedGroupCache`.


## [1.2.1] — 2026-11-15

### Fixed

- **Startup log lines were repeated once per worker process.**  When stdout is
  not a terminal — the normal case in a container — the parent's log output is
  buffered.  Each worker forked with those lines still pending, inherited a copy
  of the buffer and printed it again, so `docker logs` showed the startup banner
  up to `FCGI_WORKERS` times.  All forks now flush the buffered output first
  (`src/util/fork_safety.{h,cpp}`, covered by `test_fork_safety`).
- **Log lines of concurrently starting processes could interleave.**  The worker
  and cache-reader warnings are now emitted as one line each, so ten starting
  workers can no longer produce output like `[WARN] [WARN] Worker pid=Cannot
  connect …`.

## [1.2.0] — 2026-11-14

### Added

- **`/r` and `/w` report a knxd outage to the client.**  A dead knxd used to be
  invisible over HTTP: `/r` kept answering `200` with the last known — but
  frozen — values, and only `/w` failed, so a CometVisu client could not tell
  "the bus is quiet" from "the connection is gone".  The cache reader now
  publishes its connection state (available / last successful connection /
  start of the current outage) in the shared memory region, and both handlers
  append a block while knxd is unreachable:

  ```json
  {"d":{...},"i":42,"knxd":{"ok":false,"since":1763000000,"lastOk":1762990000}}
  {"error":"write failed","knxd":{"ok":false,"since":1763000000,"lastOk":1762990000}}
  ```

  The field is absent while knxd is reachable (no overhead in the normal case).
  `since` is the Unix time the current outage started, `lastOk` the Unix time of
  the last successful connection; both are omitted when unknown (knxd never
  connected since startup, or the outage start was not recorded yet).
  A read is not successful while the bus connection is gone, so `/r` answers
  `503` — and it answers immediately instead of waiting for the long-poll
  timeout, because nothing can arrive.  The body still carries the last known
  values, so a client may keep showing them and add a "connection lost"
  marker.  `/w` answers `503` with `{"error":"write failed",...}`.
  `JsonBuilder` gained `add_bool()` for the `ok` literal.

### Fixed

- **The router dropped the `/w` response body.**  `Router::route()` only copied
  `http_status` for `/w`, so error details never reached the client.
- **Configurations with more than 250 group addresses lost all values above
  address ~250** ([CometVisu#1521](https://github.com/CometVisu/CometVisu/issues/1521)).
  `QueryString` capped the number of values per key at 250 (and total key/value
  pairs at 256) and silently dropped the excess.  Since CometVisu subscribes
  every address of a visualization in one request, no `GroupValueRead` was sent
  for the dropped addresses and the long-poll filter never matched them — the
  corresponding tiles stayed empty forever (independent of the order in the
  config).  The limits are now `kMaxValuesPerKey = 4096`,
  `kMaxTotalPairs = 8192` and `kMaxQueryParams = 256`, and they are public
  constants on `QueryString`.

### Changed

- **Query-string limit violations are no longer silent.**  `QueryString`
  reports truncation via `limit_exceeded()`; `/r` and `/w` answer with
  `400 {"error":"too many addresses"}` instead of returning (or writing) an
  incomplete address list.

### Tests

- **A CI job can no longer hang on a leaked test worker.**  The fork-based
  tests (`test_fork_workers`, `test_concurrent_clients`) left forked workers
  behind on early assertion failures and terminated them with `SIGTERM`,
  which a worker blocked in `FCGX_Accept_r()` may ignore.  A surviving worker
  keeps the pipe open that ctest uses to capture test output, so ctest waits
  forever for an EOF that never comes and the job sits at
  `Start 23: test_fork_workers` for hours.  Workers now die with their parent
  (`PR_SET_PDEATHSIG`), are killed with `SIGKILL` and reaped per pid, and are
  tracked so `TearDown()` reaps them even after a failed `ASSERT`.
- `test_fork_workers`'s fork-failure test no longer lowers `RLIMIT_NPROC`
  (which counts the tasks of the whole real UID and therefore behaves
  completely differently on a CI runner than on a developer machine); the
  fork primitive is injected instead, making the failure point deterministic.
  A new test `WorkersDieWithTheirParent` covers the orphan scenario.
- CI runs `ctest --timeout 120` and the job has a `timeout-minutes`
  backstop, so a wedged test fails within seconds instead of blocking the
  workflow.

## [1.1.0] — 2026-11-01

### Fixed

- **Cache reader burned 100% CPU when knxd was missing.**
  `KnxdClient::wait_for_activity()` reported "not connected" and
  `POLLHUP`/`POLLERR`/`POLLNVAL` as a plain timeout, so the cache reader
  loop retried `poll()` in a tight userspace loop and never reconnected —
  `/r` kept returning stale values forever while one CPU core was saturated.
  `WaitResult` now has an `Error` state and the loop (new
  `run_cache_reader()` in `src/knxd/cache_reader.cpp`) reconnects forever
  with an exponential backoff (500 ms … 30 s, rate-limited logging).
- The daemon no longer refuses to start (or exits) when knxd is unavailable:
  parent, cache reader and workers log a warning and retry in the background,
  so knxd may be restarted at any time without restarting the backend.
- `KnxdClient::connect()` now remembers the socket path even when the attempt
  fails, so `reconnect()` can recover once knxd is back.
- **A `/w` request can no longer block forever on the knxd handshake.**
  `open_group_socket()` read the `EIB_OPEN_GROUPCON` reply from a blocking
  socket without a deadline, so a knxd that accepted the connection but never
  answered hung the worker (and with it the request) indefinitely.  The reply
  is now read with a deadline (default 5 s, `set_handshake_timeout_ms()`).
- **Writes and logins are no longer rejected with 503.** The concurrency and
  load-shedding semaphores applied to `/w`/`/l` as well, so a pool full of
  long-polls made switch operations fail.  Both now only guard `/r`.
  Tokens are released via RAII (also when a handler throws — that used to
  terminate the worker process; it now returns HTTP 500), and a worker that
  dies restores its token instead of permanently shrinking the pool.
- **The parent supervisor restarts dead children.** The SIGCHLD handler used
  to `waitpid()` all children itself, so the supervisor never saw an exit
  status: a crashed worker was never replaced and a dead cache reader was
  never restarted (leaving `/r` on stale data forever).  The parent is now the
  only reaper, keeps the listen socket open for replacements, restarts workers
  and the cache reader with a crash-loop backoff, and no longer ignores
  SIGTERM while blocked in `wait()` (`sigaction()` without `SA_RESTART`).
- `FcgiRequest::path()` falls back to the last component of `SCRIPT_NAME` when
  no `REQUEST_URI` is passed (pure-CGI setups such as `cgi-fcgi`).

## [1.0.0] — 2026-07-05

### Added

- Initial release of cometvisu-knxd-fcgi.
- FastCGI backend implementing the CometVisu Protocol.
- `/l` (login) endpoint with session management.
- `/r` (read) endpoint with long-poll support via `poll()`.
- `/w` (write) endpoint for sending KNX telegrams.
- knxd Unix socket client with eibd binary protocol support.
- Address cache integration with knxd's built-in cache.
- Configurable knxd socket path (`KNXD_SOCKET` env var).
- Configurable long-poll timeout (`LONGPOLL_TIMEOUT_SEC` env var).
- Debug logging via `DEBUG_BACKEND` environment variable.
- JSON response builder (no external JSON library dependency).
- Query string parser.
- Comprehensive test suite: unit, integration, and end-to-end tests.
- CMake build system with Google Test integration.
- GPLv3 licensed.
